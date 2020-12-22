#ifndef CORO_CLOUDSTORAGE_HTTP_IO_H
#define CORO_CLOUDSTORAGE_HTTP_IO_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/interrupted_exception.h>
#include <coro/semaphore.h>
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
    DoRequest(r, data, size);
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
  void setuseragent(std::string*) final {}
  void addevents(::mega::Waiter*, int) final {}

 private:
  friend class ::coro::cloudstorage::Mega;

  Task<> DoRequest(::mega::HttpReq* r, const char* data, unsigned size) {
    auto request = http::Request<std::string>{
        .url = r->posturl,
        .method = r->method == ::mega::METHOD_POST ? http::Method::kPost
                                                   : http::Method::kGet,
        .headers = {{"Content-Type", r->type == ::mega::REQ_JSON
                                         ? "application/json"
                                         : "application/octet-stream"}},
        .body = data ? std::string(data, size) : *r->out};
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
        FOR_CO_AWAIT(const std::string& chunk, response.body, {
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
        });
        if (*content_length != response_size) {
          throw http::HttpException(http::HttpException::kMalformedResponse);
        }
        io_ready_ = true;
        r->contentlength = *content_length;
        r->httpstatus = response.status;
        r->status = ::mega::REQ_SUCCESS;
        lastdata = r->lastdata = ::mega::Waiter::ds;
        success_ = true;
      } catch (const http::HttpException&) {
        if (stop_source.request_stop()) {
          throw InterruptedException();
        }
        io_ready_ = true;
        lastdata = r->lastdata = ::mega::Waiter::ds;
        r->status = ::mega::REQ_FAILURE;
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
  std::function<void()> on_event_;
};

}  // namespace coro::cloudstorage::mega

#endif  // CORO_CLOUDSTORAGE_HTTP_IO_H
