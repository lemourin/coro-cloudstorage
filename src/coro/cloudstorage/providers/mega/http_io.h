#ifndef CORO_CLOUDSTORAGE_HTTP_IO_H
#define CORO_CLOUDSTORAGE_HTTP_IO_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/interrupted_exception.h>
#include <coro/promise.h>
#include <coro/stdx/stop_source.h>
#include <mega.h>

namespace coro::cloudstorage {
class Mega;
}

namespace coro::cloudstorage::mega {

template <http::HttpClient HttpClient>
class HttpIO : public ::mega::HttpIO {
 public:
  template <typename F>
  HttpIO(const HttpClient& http, F on_event)
      : http_(http), on_event_(std::move(on_event)) {}

  void post(::mega::HttpReq* r, const char* data, unsigned size) final {
    Invoke(DoRequest(r, data, size));
  }
  void cancel(::mega::HttpReq* r) final {
    auto stop_source =
        reinterpret_cast<coro::stdx::stop_source*>(r->httpiohandle);
    stop_source->request_stop();
    delete stop_source;
  }
  m_off_t postpos(void*) final { return 0; }
  bool doio() final {
    auto io_ready = std::exchange(io_ready_, false);
    if (io_ready) {
      success = std::exchange(success, false);
    }
    return io_ready;
  }
  void setuseragent(std::string* useragent) final {
    if (useragent) {
      useragent_ = *useragent;
    }
  }
  void addevents(::mega::Waiter*, int) final {}

 private:
  friend class ::coro::cloudstorage::Mega;

  Task<> DoRequest(::mega::HttpReq* r, const char* data, unsigned size) {
    http::Request<std::string> request{
        .url = r->posturl,
        .method = r->method == ::mega::METHOD_POST ? http::Method::kPost
                                                   : http::Method::kGet};
    if (useragent_) {
      request.headers.emplace_back("User-Agent", *useragent_);
    }
    if (r->type == ::mega::REQ_JSON) {
      request.headers.emplace_back("Content-Type", "application/json");
    } else if (r->type == ::mega::REQ_BINARY &&
               ((data && size > 0) || !r->out->empty())) {
      request.headers.emplace_back("Content-Type", "application/octet-stream");
    }
    if (data && size > 0) {
      request.body = data;
    } else if (!r->out->empty()) {
      request.body = *r->out;
    }
    coro::stdx::stop_source stop_source;
    try {
      try {
        r->status = ::mega::REQ_INFLIGHT;
        r->httpiohandle = new coro::stdx::stop_source(stop_source);
        auto response =
            co_await http_.Fetch(std::move(request), stop_source.get_token());
        if (stop_source.get_token().stop_requested()) {
          throw InterruptedException();
        }
        auto content_length = GetContentLength(response.headers);
        if (!content_length) {
          throw http::HttpException(http::HttpException::kMalformedResponse);
        }
        int64_t response_size = 0;
        FOR_CO_AWAIT(const std::string& chunk, response.body) {
          if (stop_source.get_token().stop_requested()) {
            throw InterruptedException();
          }
          r->put(
              const_cast<void*>(reinterpret_cast<const void*>(chunk.c_str())),
              static_cast<unsigned>(chunk.size()));
          lastdata = r->lastdata = ::mega::Waiter::ds;
          io_ready_ = true;
          on_event_();
          response_size += static_cast<int64_t>(chunk.size());
        }
        if (*content_length != response_size) {
          throw http::HttpException(http::HttpException::kMalformedResponse);
        }
        io_ready_ = true;
        r->contentlength = *content_length;
        r->contenttype =
            http::GetHeader(response.headers, "Content-Type").value_or("");
        r->httpstatus = response.status;
        r->status = ::mega::REQ_SUCCESS;
        r->httpio = nullptr;
        lastdata = r->lastdata = ::mega::Waiter::ds;
        success_ = true;
      } catch (const http::HttpException&) {
        if (stop_source.get_token().stop_requested()) {
          throw InterruptedException();
        }
        io_ready_ = true;
        lastdata = r->lastdata = ::mega::Waiter::ds;
        r->status = ::mega::REQ_FAILURE;
        r->httpio = nullptr;
      }
      on_event_();
    } catch (const InterruptedException&) {
    }
  }

  template <typename T>
  std::optional<int64_t> GetContentLength(const T& headers) {
    std::optional<int64_t> content_length;
    for (const auto& [key, value] : headers) {
      if (http::ToLowerCase(key) == "content-length") {
        try {
          content_length = std::stol(value);
        } catch (const std::exception&) {
        }
      }
    }
    return content_length;
  }

  const HttpClient& http_;
  bool io_ready_ = false;
  bool success_ = false;
  std::optional<std::string> useragent_;
  std::function<void()> on_event_;
};

}  // namespace coro::cloudstorage::mega

#endif  // CORO_CLOUDSTORAGE_HTTP_IO_H
