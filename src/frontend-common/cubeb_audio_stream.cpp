// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cubeb_audio_stream.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common_host.h"
#include "core/host.h"
#include "core/settings.h"
#include "cubeb/cubeb.h"
#include "fmt/format.h"
Log_SetChannel(CubebAudioStream);

#ifdef _WIN32
#include "common/windows_headers.h"
#include <objbase.h>
#pragma comment(lib, "Ole32.lib")
#endif

static void StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state);

CubebAudioStream::CubebAudioStream(u32 sample_rate, u32 channels, u32 buffer_ms, AudioStretchMode stretch)
  : AudioStream(sample_rate, channels, buffer_ms, stretch)
{
}

CubebAudioStream::~CubebAudioStream()
{
  DestroyContextAndStream();
}

void CubebAudioStream::LogCallback(const char* fmt, ...)
{
  std::va_list ap;
  va_start(ap, fmt);
  std::string msg(StringUtil::StdStringFromFormatV(fmt, ap));
  va_end(ap);
  Log_DevPrintf("(Cubeb): %s", msg.c_str());
}

void CubebAudioStream::DestroyContextAndStream()
{
  if (stream)
  {
    cubeb_stream_stop(stream);
    cubeb_stream_destroy(stream);
    stream = nullptr;
  }

  if (m_context)
  {
    cubeb_destroy(m_context);
    m_context = nullptr;
  }

#ifdef _WIN32
  if (m_com_initialized_by_us)
  {
    CoUninitialize();
    m_com_initialized_by_us = false;
  }
#endif
}

bool CubebAudioStream::Initialize(u32 latency_ms)
{
#ifdef _WIN32
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  m_com_initialized_by_us = SUCCEEDED(hr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
  {
    Host::ReportErrorAsync("Error", "Failed to initialize COM for Cubeb");
    return false;
  }
#endif

  cubeb_set_log_callback(CUBEB_LOG_NORMAL, LogCallback);

  int rv =
    cubeb_init(&m_context, "DuckStation", g_settings.audio_driver.empty() ? nullptr : g_settings.audio_driver.c_str());
  if (rv != CUBEB_OK)
  {
    Host::ReportFormattedErrorAsync("Error", "Could not initialize cubeb context: %d", rv);
    return false;
  }

  cubeb_stream_params params = {};
  params.format = CUBEB_SAMPLE_S16LE;
  params.rate = m_sample_rate;
  params.channels = m_channels;
  params.layout = CUBEB_LAYOUT_UNDEFINED;
  params.prefs = CUBEB_STREAM_PREF_NONE;

  u32 latency_frames = GetBufferSizeForMS(m_sample_rate, (latency_ms == 0) ? m_buffer_ms : latency_ms);
  u32 min_latency_frames = 0;
  rv = cubeb_get_min_latency(m_context, &params, &min_latency_frames);
  if (rv == CUBEB_ERROR_NOT_SUPPORTED)
  {
    Log_DevPrintf("(Cubeb) Cubeb backend does not support latency queries, using latency of %d ms (%u frames).",
                  m_buffer_ms, latency_frames);
  }
  else
  {
    if (rv != CUBEB_OK)
    {
      Log_ErrorPrintf("(Cubeb) Could not get minimum latency: %d", rv);
      DestroyContextAndStream();
      return false;
    }

    const u32 minimum_latency_ms = GetMSForBufferSize(m_sample_rate, min_latency_frames);
    Log_DevPrintf("(Cubeb) Minimum latency: %u ms (%u audio frames)", minimum_latency_ms, min_latency_frames);
    if (latency_ms == 0)
    {
      // use minimum
      latency_frames = min_latency_frames;
    }
    else if (minimum_latency_ms > latency_ms)
    {
      Log_WarningPrintf("(Cubeb) Minimum latency is above requested latency: %u vs %u, adjusting to compensate.",
                        min_latency_frames, latency_frames);
      latency_frames = min_latency_frames;
    }
  }

  cubeb_devid selected_device = nullptr;
  const std::string& selected_device_name = g_settings.audio_output_device;
  cubeb_device_collection devices;
  bool devices_valid = false;
  if (!selected_device_name.empty())
  {
    rv = cubeb_enumerate_devices(m_context, CUBEB_DEVICE_TYPE_OUTPUT, &devices);
    devices_valid = (rv == CUBEB_OK);
    if (rv == CUBEB_OK)
    {
      for (size_t i = 0; i < devices.count; i++)
      {
        const cubeb_device_info& di = devices.device[i];
        if (di.device_id && selected_device_name == di.device_id)
        {
          Log_InfoPrintf("Using output device '%s' (%s).", di.device_id,
                         di.friendly_name ? di.friendly_name : di.device_id);
          selected_device = di.devid;
          break;
        }
      }

      if (!selected_device)
      {
        Host::AddOSDMessage(
          fmt::format("Requested audio output device '{}' not found, using default.", selected_device_name), 10.0f);
      }
    }
    else
    {
      Log_WarningPrintf("cubeb_enumerate_devices() returned %d, using default device.", rv);
    }
  }

  BaseInitialize();
  m_volume = 100;
  m_paused = false;

  char stream_name[32];
  std::snprintf(stream_name, sizeof(stream_name), "%p", this);

  rv = cubeb_stream_init(m_context, &stream, stream_name, nullptr, nullptr, selected_device, &params, latency_frames,
                         &CubebAudioStream::DataCallback, StateCallback, this);

  if (devices_valid)
    cubeb_device_collection_destroy(m_context, &devices);

  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("(Cubeb) Could not create stream: %d", rv);
    DestroyContextAndStream();
    return false;
  }

  rv = cubeb_stream_start(stream);
  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("(Cubeb) Could not start stream: %d", rv);
    DestroyContextAndStream();
    return false;
  }

  return true;
}

void StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state)
{
  // noop
}

long CubebAudioStream::DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
                                    long nframes)
{
  static_cast<CubebAudioStream*>(user_ptr)->ReadFrames(static_cast<s16*>(output_buffer), static_cast<u32>(nframes));
  return nframes;
}

void CubebAudioStream::SetPaused(bool paused)
{
  if (paused == m_paused || !stream)
    return;

  const int rv = paused ? cubeb_stream_stop(stream) : cubeb_stream_start(stream);
  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("Could not %s stream: %d", paused ? "pause" : "resume", rv);
    return;
  }

  m_paused = paused;
}

void CubebAudioStream::SetOutputVolume(u32 volume)
{
  if (volume == m_volume)
    return;

  int rv = cubeb_stream_set_volume(stream, static_cast<float>(volume) / 100.0f);
  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("cubeb_stream_set_volume() failed: %d", rv);
    return;
  }

  m_volume = volume;
}

std::unique_ptr<AudioStream> CommonHost::CreateCubebAudioStream(u32 sample_rate, u32 channels, u32 buffer_ms,
                                                                u32 latency_ms, AudioStretchMode stretch)
{
  std::unique_ptr<CubebAudioStream> stream(
    std::make_unique<CubebAudioStream>(sample_rate, channels, buffer_ms, stretch));
  if (!stream->Initialize(latency_ms))
    stream.reset();
  return stream;
}

std::vector<std::string> CommonHost::GetCubebDriverNames()
{
  std::vector<std::string> names;
  const char** cubeb_names = cubeb_get_backend_names();
  for (u32 i = 0; cubeb_names[i] != nullptr; i++)
    names.emplace_back(cubeb_names[i]);
  return names;
}

std::vector<std::pair<std::string, std::string>> CommonHost::GetCubebOutputDevices(const char* driver)
{
  std::vector<std::pair<std::string, std::string>> ret;
  ret.emplace_back(std::string(), Host::TranslateStdString("CommonHost", "Default Output Device"));

  cubeb* context;
  int rv = cubeb_init(&context, "DuckStation", (driver && *driver) ? driver : nullptr);
  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("cubeb_init() failed: %d", rv);
    return ret;
  }

  ScopedGuard context_cleanup([context]() { cubeb_destroy(context); });

  cubeb_device_collection devices;
  rv = cubeb_enumerate_devices(context, CUBEB_DEVICE_TYPE_OUTPUT, &devices);
  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("cubeb_enumerate_devices() failed: %d", rv);
    return ret;
  }

  ScopedGuard devices_cleanup([context, &devices]() { cubeb_device_collection_destroy(context, &devices); });

  for (size_t i = 0; i < devices.count; i++)
  {
    const cubeb_device_info& di = devices.device[i];
    if (!di.device_id)
      continue;

    ret.emplace_back(di.device_id, di.friendly_name ? di.friendly_name : di.device_id);
  }

  return ret;
}
