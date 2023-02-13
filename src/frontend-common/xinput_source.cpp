// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "xinput_source.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/host.h"
#include "input_manager.h"
#include <cmath>
Log_SetChannel(XInputSource);

const char* XInputSource::s_axis_names[XInputSource::NUM_AXES] = {
  "LeftX",        // AXIS_LEFTX
  "LeftY",        // AXIS_LEFTY
  "RightX",       // AXIS_RIGHTX
  "RightY",       // AXIS_RIGHTY
  "LeftTrigger",  // AXIS_TRIGGERLEFT
  "RightTrigger", // AXIS_TRIGGERRIGHT
};
static const GenericInputBinding s_xinput_generic_binding_axis_mapping[][2] = {
  {GenericInputBinding::LeftStickLeft, GenericInputBinding::LeftStickRight},   // AXIS_LEFTX
  {GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown},      // AXIS_LEFTY
  {GenericInputBinding::RightStickLeft, GenericInputBinding::RightStickRight}, // AXIS_RIGHTX
  {GenericInputBinding::RightStickUp, GenericInputBinding::RightStickDown},    // AXIS_RIGHTY
  {GenericInputBinding::Unknown, GenericInputBinding::L2},                     // AXIS_TRIGGERLEFT
  {GenericInputBinding::Unknown, GenericInputBinding::R2},                     // AXIS_TRIGGERRIGHT
};

const char* XInputSource::s_button_names[XInputSource::NUM_BUTTONS] = {
  "DPadUp",        // XINPUT_GAMEPAD_DPAD_UP
  "DPadDown",      // XINPUT_GAMEPAD_DPAD_DOWN
  "DPadLeft",      // XINPUT_GAMEPAD_DPAD_LEFT
  "DPadRight",     // XINPUT_GAMEPAD_DPAD_RIGHT
  "Start",         // XINPUT_GAMEPAD_START
  "Back",          // XINPUT_GAMEPAD_BACK
  "LeftStick",     // XINPUT_GAMEPAD_LEFT_THUMB
  "RightStick",    // XINPUT_GAMEPAD_RIGHT_THUMB
  "LeftShoulder",  // XINPUT_GAMEPAD_LEFT_SHOULDER
  "RightShoulder", // XINPUT_GAMEPAD_RIGHT_SHOULDER
  "A",             // XINPUT_GAMEPAD_A
  "B",             // XINPUT_GAMEPAD_B
  "X",             // XINPUT_GAMEPAD_X
  "Y",             // XINPUT_GAMEPAD_Y
  "Guide",         // XINPUT_GAMEPAD_GUIDE
};
const u16 XInputSource::s_button_masks[XInputSource::NUM_BUTTONS] = {
  XINPUT_GAMEPAD_DPAD_UP,
  XINPUT_GAMEPAD_DPAD_DOWN,
  XINPUT_GAMEPAD_DPAD_LEFT,
  XINPUT_GAMEPAD_DPAD_RIGHT,
  XINPUT_GAMEPAD_START,
  XINPUT_GAMEPAD_BACK,
  XINPUT_GAMEPAD_LEFT_THUMB,
  XINPUT_GAMEPAD_RIGHT_THUMB,
  XINPUT_GAMEPAD_LEFT_SHOULDER,
  XINPUT_GAMEPAD_RIGHT_SHOULDER,
  XINPUT_GAMEPAD_A,
  XINPUT_GAMEPAD_B,
  XINPUT_GAMEPAD_X,
  XINPUT_GAMEPAD_Y,
  0x400, // XINPUT_GAMEPAD_GUIDE
};
static const GenericInputBinding s_xinput_generic_binding_button_mapping[] = {
  GenericInputBinding::DPadUp,    // XINPUT_GAMEPAD_DPAD_UP
  GenericInputBinding::DPadDown,  // XINPUT_GAMEPAD_DPAD_DOWN
  GenericInputBinding::DPadLeft,  // XINPUT_GAMEPAD_DPAD_LEFT
  GenericInputBinding::DPadRight, // XINPUT_GAMEPAD_DPAD_RIGHT
  GenericInputBinding::Start,     // XINPUT_GAMEPAD_START
  GenericInputBinding::Select,    // XINPUT_GAMEPAD_BACK
  GenericInputBinding::L3,        // XINPUT_GAMEPAD_LEFT_THUMB
  GenericInputBinding::R3,        // XINPUT_GAMEPAD_RIGHT_THUMB
  GenericInputBinding::L1,        // XINPUT_GAMEPAD_LEFT_SHOULDER
  GenericInputBinding::R1,        // XINPUT_GAMEPAD_RIGHT_SHOULDER
  GenericInputBinding::Cross,     // XINPUT_GAMEPAD_A
  GenericInputBinding::Circle,    // XINPUT_GAMEPAD_B
  GenericInputBinding::Square,    // XINPUT_GAMEPAD_X
  GenericInputBinding::Triangle,  // XINPUT_GAMEPAD_Y
  GenericInputBinding::System,    // XINPUT_GAMEPAD_GUIDE
};

XInputSource::XInputSource() = default;

XInputSource::~XInputSource() = default;

bool XInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  // xinput1_3.dll is flawed and obsolete, but it's also commonly used by wrappers.
  // For this reason, try to load it *only* from the application directory, and not system32.
  m_xinput_module = LoadLibraryExW(L"xinput1_3", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
  if (!m_xinput_module)
  {
    m_xinput_module = LoadLibraryW(L"xinput1_4");
  }
  if (!m_xinput_module)
  {
    m_xinput_module = LoadLibraryW(L"xinput9_1_0");
  }
  if (!m_xinput_module)
  {
    Log_ErrorPrintf("Failed to load XInput module.");
    return false;
  }

  // Try the hidden version of XInputGetState(), which lets us query the guide button.
  m_xinput_get_state =
    reinterpret_cast<decltype(m_xinput_get_state)>(GetProcAddress(m_xinput_module, reinterpret_cast<LPCSTR>(100)));
  if (!m_xinput_get_state)
    reinterpret_cast<decltype(m_xinput_get_state)>(GetProcAddress(m_xinput_module, "XInputGetState"));
  m_xinput_set_state =
    reinterpret_cast<decltype(m_xinput_set_state)>(GetProcAddress(m_xinput_module, "XInputSetState"));
  m_xinput_get_capabilities =
    reinterpret_cast<decltype(m_xinput_get_capabilities)>(GetProcAddress(m_xinput_module, "XInputGetCapabilities"));

  if (!m_xinput_get_state || !m_xinput_set_state || !m_xinput_get_capabilities)
  {
    Log_ErrorPrintf("Failed to get XInput function pointers.");
    return false;
  }

  ReloadDevices();
  return true;
}

void XInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) {}

bool XInputSource::ReloadDevices()
{
  bool changed = false;
  for (u32 i = 0; i < NUM_CONTROLLERS; i++)
  {
    XINPUT_STATE new_state;
    DWORD result = m_xinput_get_state(i, &new_state);

    if (result == ERROR_SUCCESS)
    {
      if (m_controllers[i].connected)
        continue;

      HandleControllerConnection(i);
      changed = true;
    }
    else if (result == ERROR_DEVICE_NOT_CONNECTED)
    {
      if (!m_controllers[i].connected)
        continue;

      HandleControllerDisconnection(i);
      changed = true;
    }
  }

  return changed;
}

void XInputSource::Shutdown()
{
  for (u32 i = 0; i < NUM_CONTROLLERS; i++)
  {
    if (m_controllers[i].connected)
      HandleControllerDisconnection(i);
  }

  if (m_xinput_module)
  {
    FreeLibrary(m_xinput_module);
    m_xinput_module = nullptr;
  }

  m_xinput_get_state = nullptr;
  m_xinput_set_state = nullptr;
  m_xinput_get_capabilities = nullptr;
}

void XInputSource::PollEvents()
{
  for (u32 i = 0; i < NUM_CONTROLLERS; i++)
  {
    const bool was_connected = m_controllers[i].connected;
    if (!was_connected)
      continue;

    XINPUT_STATE new_state;
    DWORD result = m_xinput_get_state(i, &new_state);

    if (result == ERROR_SUCCESS)
    {
      if (!was_connected)
        HandleControllerConnection(i);

      CheckForStateChanges(i, new_state);
    }
    else
    {
      if (result != ERROR_DEVICE_NOT_CONNECTED)
        Log_WarningPrintf("XInputGetState(%u) failed: 0x%08X / 0x%08X", i, result, GetLastError());

      if (was_connected)
        HandleControllerDisconnection(i);
    }
  }
}

std::vector<std::pair<std::string, std::string>> XInputSource::EnumerateDevices()
{
  std::vector<std::pair<std::string, std::string>> ret;

  for (u32 i = 0; i < NUM_CONTROLLERS; i++)
  {
    if (!m_controllers[i].connected)
      continue;

    ret.emplace_back(StringUtil::StdStringFromFormat("XInput-%u", i),
                     StringUtil::StdStringFromFormat("XInput Controller %u", i));
  }

  return ret;
}

std::optional<InputBindingKey> XInputSource::ParseKeyString(const std::string_view& device,
                                                            const std::string_view& binding)
{
  if (!StringUtil::StartsWith(device, "XInput-") || binding.empty())
    return std::nullopt;

  const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(7));
  if (!player_id.has_value() || player_id.value() < 0)
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::XInput;
  key.source_index = static_cast<u32>(player_id.value());

  if (StringUtil::EndsWith(binding, "Motor"))
  {
    key.source_subtype = InputSubclass::ControllerMotor;
    if (binding == "LargeMotor")
    {
      key.data = 0;
      return key;
    }
    else if (binding == "SmallMotor")
    {
      key.data = 1;
      return key;
    }
    else
    {
      return std::nullopt;
    }
  }
  else if (binding[0] == '+' || binding[0] == '-')
  {
    // likely an axis
    const std::string_view axis_name(binding.substr(1));
    for (u32 i = 0; i < std::size(s_axis_names); i++)
    {
      if (axis_name == s_axis_names[i])
      {
        // found an axis!
        key.source_subtype = InputSubclass::ControllerAxis;
        key.data = i;
        key.modifier = binding[0] == '-' ? InputModifier::Negate : InputModifier::None;
        return key;
      }
    }
  }
  else
  {
    // must be a button
    for (u32 i = 0; i < std::size(s_button_names); i++)
    {
      if (binding == s_button_names[i])
      {
        key.source_subtype = InputSubclass::ControllerButton;
        key.data = i;
        return key;
      }
    }
  }

  // unknown axis/button
  return std::nullopt;
}

std::string XInputSource::ConvertKeyToString(InputBindingKey key)
{
  std::string ret;

  if (key.source_type == InputSourceType::XInput)
  {
    if (key.source_subtype == InputSubclass::ControllerAxis && key.data < std::size(s_axis_names))
    {
      const char modifier = key.modifier == InputModifier::Negate ? '-' : '+';
      ret = StringUtil::StdStringFromFormat("XInput-%u/%c%s", key.source_index, modifier, s_axis_names[key.data]);
    }
    else if (key.source_subtype == InputSubclass::ControllerButton && key.data < std::size(s_button_names))
    {
      ret = StringUtil::StdStringFromFormat("XInput-%u/%s", key.source_index, s_button_names[key.data]);
    }
    else if (key.source_subtype == InputSubclass::ControllerMotor)
    {
      ret = StringUtil::StdStringFromFormat("XInput-%u/%sMotor", key.source_index, key.data ? "Large" : "Small");
    }
  }

  return ret;
}

std::vector<InputBindingKey> XInputSource::EnumerateMotors()
{
  std::vector<InputBindingKey> ret;

  for (u32 i = 0; i < NUM_CONTROLLERS; i++)
  {
    const ControllerData& cd = m_controllers[i];
    if (!cd.connected)
      continue;

    if (cd.has_large_motor)
      ret.push_back(MakeGenericControllerMotorKey(InputSourceType::XInput, i, 0));

    if (cd.has_small_motor)
      ret.push_back(MakeGenericControllerMotorKey(InputSourceType::XInput, i, 1));
  }

  return ret;
}

bool XInputSource::GetGenericBindingMapping(const std::string_view& device, GenericInputBindingMapping* mapping)
{
  if (!StringUtil::StartsWith(device, "XInput-"))
    return false;

  const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(7));
  if (!player_id.has_value() || player_id.value() < 0)
    return false;

  if (player_id.value() < 0 || player_id.value() >= static_cast<s32>(XUSER_MAX_COUNT))
    return false;

  // assume all buttons are present.
  const s32 pid = player_id.value();
  for (u32 i = 0; i < std::size(s_xinput_generic_binding_axis_mapping); i++)
  {
    const GenericInputBinding negative = s_xinput_generic_binding_axis_mapping[i][0];
    const GenericInputBinding positive = s_xinput_generic_binding_axis_mapping[i][1];
    if (negative != GenericInputBinding::Unknown)
      mapping->emplace_back(negative, StringUtil::StdStringFromFormat("XInput-%d/-%s", pid, s_axis_names[i]));

    if (positive != GenericInputBinding::Unknown)
      mapping->emplace_back(positive, StringUtil::StdStringFromFormat("XInput-%d/+%s", pid, s_axis_names[i]));
  }
  for (u32 i = 0; i < std::size(s_xinput_generic_binding_button_mapping); i++)
  {
    const GenericInputBinding binding = s_xinput_generic_binding_button_mapping[i];
    if (binding != GenericInputBinding::Unknown)
      mapping->emplace_back(binding, StringUtil::StdStringFromFormat("XInput-%d/%s", pid, s_button_names[i]));
  }

  if (m_controllers[pid].has_small_motor)
    mapping->emplace_back(GenericInputBinding::SmallMotor,
                          StringUtil::StdStringFromFormat("XInput-%d/SmallMotor", pid));
  if (m_controllers[pid].has_large_motor)
    mapping->emplace_back(GenericInputBinding::LargeMotor,
                          StringUtil::StdStringFromFormat("XInput-%d/LargeMotor", pid));

  return true;
}

void XInputSource::HandleControllerConnection(u32 index)
{
  Log_InfoPrintf("XInput controller %u connected.", index);

  XINPUT_CAPABILITIES caps = {};
  if (m_xinput_get_capabilities(index, 0, &caps) != ERROR_SUCCESS)
    Log_WarningPrintf("Failed to get XInput capabilities for controller %u", index);

  ControllerData& cd = m_controllers[index];
  cd.connected = true;
  cd.has_large_motor = caps.Vibration.wLeftMotorSpeed != 0;
  cd.has_small_motor = caps.Vibration.wRightMotorSpeed != 0;
  cd.last_state = {};

  InputManager::OnInputDeviceConnected(StringUtil::StdStringFromFormat("XInput-%u", index),
                                       StringUtil::StdStringFromFormat("XInput Controller %u", index));
}

void XInputSource::HandleControllerDisconnection(u32 index)
{
  Log_InfoPrintf("XInput controller %u disconnected.", index);
  InputManager::OnInputDeviceDisconnected(StringUtil::StdStringFromFormat("XInput-%u", index));
  m_controllers[index] = {};
}

void XInputSource::CheckForStateChanges(u32 index, const XINPUT_STATE& new_state)
{
  ControllerData& cd = m_controllers[index];
  if (new_state.dwPacketNumber == cd.last_state.dwPacketNumber)
    return;

  XINPUT_GAMEPAD& ogp = cd.last_state.Gamepad;
  const XINPUT_GAMEPAD& ngp = new_state.Gamepad;

#define CHECK_AXIS(field, axis, min_value, max_value)                                                                  \
  if (ogp.field != ngp.field)                                                                                          \
  {                                                                                                                    \
    InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::XInput, index, axis),                     \
                               static_cast<float>(ngp.field) / ((ngp.field < 0) ? min_value : max_value),              \
                               GenericInputBinding::Unknown);                                                          \
  }

  // Y axes is inverted in XInput when compared to SDL.
  CHECK_AXIS(sThumbLX, AXIS_LEFTX, 32768, 32767);
  CHECK_AXIS(sThumbLY, AXIS_LEFTY, -32768, -32767);
  CHECK_AXIS(sThumbRX, AXIS_RIGHTX, 32768, 32767);
  CHECK_AXIS(sThumbRY, AXIS_RIGHTY, -32768, -32767);
  CHECK_AXIS(bLeftTrigger, AXIS_LEFTTRIGGER, 0, 255);
  CHECK_AXIS(bRightTrigger, AXIS_RIGHTTRIGGER, 0, 255);

#undef CHECK_AXIS

  const u16 old_button_bits = ogp.wButtons;
  const u16 new_button_bits = ngp.wButtons;
  if (old_button_bits != new_button_bits)
  {
    for (u32 button = 0; button < NUM_BUTTONS; button++)
    {
      const u16 button_mask = s_button_masks[button];
      if ((old_button_bits & button_mask) != (new_button_bits & button_mask))
      {
        const GenericInputBinding generic_key = (button < std::size(s_xinput_generic_binding_button_mapping)) ?
                                                  s_xinput_generic_binding_button_mapping[button] :
                                                  GenericInputBinding::Unknown;
        const float value = ((new_button_bits & button_mask) != 0) ? 1.0f : 0.0f;
        InputManager::InvokeEvents(MakeGenericControllerButtonKey(InputSourceType::XInput, index, button), value,
                                   generic_key);
      }
    }
  }

  cd.last_state = new_state;
}

void XInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
  if (key.source_subtype != InputSubclass::ControllerMotor || key.source_index >= NUM_CONTROLLERS)
    return;

  ControllerData& cd = m_controllers[key.source_index];
  if (!cd.connected)
    return;

  const u16 i_intensity = static_cast<u16>(intensity * 65535.0f);
  if (key.data != 0)
    cd.last_vibration.wRightMotorSpeed = i_intensity;
  else
    cd.last_vibration.wLeftMotorSpeed = i_intensity;

  m_xinput_set_state(key.source_index, &cd.last_vibration);
}

void XInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                                    float small_intensity)
{
  if (large_key.source_index != small_key.source_index || large_key.source_subtype != InputSubclass::ControllerMotor ||
      small_key.source_subtype != InputSubclass::ControllerMotor)
  {
    // bonkers config where they're mapped to different controllers... who would do such a thing?
    UpdateMotorState(large_key, large_intensity);
    UpdateMotorState(small_key, small_intensity);
    return;
  }

  ControllerData& cd = m_controllers[large_key.source_index];
  if (!cd.connected)
    return;

  cd.last_vibration.wLeftMotorSpeed = static_cast<u16>(large_intensity * 65535.0f);
  cd.last_vibration.wRightMotorSpeed = static_cast<u16>(small_intensity * 65535.0f);
  m_xinput_set_state(large_key.source_index, &cd.last_vibration);
}

std::unique_ptr<InputSource> InputSource::CreateXInputSource()
{
  return std::make_unique<XInputSource>();
}
