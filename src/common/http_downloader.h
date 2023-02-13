// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "common/types.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

class HTTPDownloader
{
public:
  enum : s32
  {
    HTTP_OK = 200
  };

  struct Request
  {
    using Data = std::vector<u8>;
    using Callback = std::function<void(s32 status_code, std::string content_type, Data data)>;

    enum class Type
    {
      Get,
      Post,
    };

    enum class State
    {
      Pending,
      Cancelled,
      Started,
      Receiving,
      Complete,
    };

    HTTPDownloader* parent;
    Callback callback;
    std::string url;
    std::string post_data;
    std::string content_type;
    Data data;
    u64 start_time;
    s32 status_code = 0;
    u32 content_length = 0;
    Type type = Type::Get;
    std::atomic<State> state{State::Pending};
  };

  HTTPDownloader();
  virtual ~HTTPDownloader();

  static std::unique_ptr<HTTPDownloader> Create(const char* user_agent = DEFAULT_USER_AGENT);
  static std::string URLEncode(const std::string_view& str);
  static std::string URLDecode(const std::string_view& str);
  static std::string GetExtensionForContentType(const std::string& content_type);

  void SetTimeout(float timeout);
  void SetMaxActiveRequests(u32 max_active_requests);

  void CreateRequest(std::string url, Request::Callback callback);
  void CreatePostRequest(std::string url, std::string post_data, Request::Callback callback);

  bool HasAnyRequests();
  void PollRequests();
  void WaitForAllRequests();

  static const char DEFAULT_USER_AGENT[];

protected:
  virtual Request* InternalCreateRequest() = 0;
  virtual void InternalPollRequests() = 0;

  virtual bool StartRequest(Request* request) = 0;
  virtual void CloseRequest(Request* request) = 0;

  void LockedAddRequest(Request* request);
  u32 LockedGetActiveRequestCount();
  void LockedPollRequests(std::unique_lock<std::mutex>& lock);

  float m_timeout;
  u32 m_max_active_requests;

  std::mutex m_pending_http_request_lock;
  std::vector<Request*> m_pending_http_requests;
};

} // namespace FrontendCommon
