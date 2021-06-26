#ifndef CORO_CLOUDSTORAGE_FUSE_TIMING_OUT_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_FUSE_TIMING_OUT_CLOUD_PROVIDER_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/timing_out_stop_token.h>
#include <coro/util/event_loop.h>
#include <coro/util/stop_token_or.h>

#include <utility>

namespace coro::cloudstorage::util {

template <typename CloudProviderT>
struct TimingOutCloudProvider {
  using Type = CloudProviderT;
  using Impl = CloudProviderT;
  using Item = typename CloudProviderT::Item;
  using PageData = typename CloudProviderT::PageData;
  using FileContent = typename CloudProviderT::FileContent;
  using ItemTypeList = typename CloudProviderT::ItemTypeList;

  class CloudProvider;
};

template <typename CloudProviderT>
class TimingOutCloudProvider<CloudProviderT>::CloudProvider
    : public coro::cloudstorage::CloudProvider<TimingOutCloudProvider,
                                               CloudProvider> {
 private:
  coro::util::EventLoop* event_loop_;
  int timeout_ms_;
  CloudProviderT* provider_;

 public:
  CloudProvider(coro::util::EventLoop* event_loop, int timeout_ms,
                CloudProviderT* provider)
      : event_loop_(event_loop), timeout_ms_(timeout_ms), provider_(provider) {}

  CloudProviderT* GetProvider() { return provider_; }
  const CloudProviderT* GetProvider() const { return provider_; }

  template <typename DirectoryT>
  bool IsFileContentSizeRequired(const DirectoryT& d) const {
    return provider_->IsFileContentSizeRequired(d);
  }

  auto GetRoot(stdx::stop_token stop_token) const
      -> decltype(provider_->GetRoot(stop_token)) {
    auto context_token = CreateStopToken("GetRoot", std::move(stop_token));
    co_return co_await provider_->GetRoot(context_token.GetToken());
  }

  template <IsDirectory<CloudProviderT> Directory>
  auto ListDirectoryPage(Directory directory,
                         std::optional<std::string> page_token,
                         stdx::stop_token stop_token)
      -> decltype(provider_->ListDirectoryPage(directory, page_token,
                                               stop_token)) {
    auto context_token =
        CreateStopToken("ListDirectoryPage", std::move(stop_token));
    co_return co_await provider_->ListDirectoryPage(
        std::move(directory), std::move(page_token), context_token.GetToken());
  }

  auto GetGeneralData(stdx::stop_token stop_token)
      -> decltype(provider_->GetGeneralData(stop_token)) {
    auto context_token =
        CreateStopToken("GetGeneralData", std::move(stop_token));
    co_return co_await provider_->GetGeneralData(context_token.GetToken());
  }

  template <IsFile<CloudProviderT> File>
  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) {
    stdx::stop_source stop_source;
    stdx::stop_callback cb(std::move(stop_token),
                           [&] { stop_source.request_stop(); });
    auto scope_guard =
        coro::util::AtScopeExit([&] { stop_source.request_stop(); });
    auto generator = provider_->GetFileContent(std::move(file), range,
                                               stop_source.get_token());
    auto do_await = [&]<typename F>(F task) -> Task<typename F::type> {
      TimingOutStopToken timeout_source(*event_loop_, "GetFileContent",
                                        timeout_ms_);
      stdx::stop_callback cb(timeout_source.GetToken(),
                             [&] { stop_source.request_stop(); });
      co_return co_await task;
    };
    auto it = co_await do_await(generator.begin());
    while (it != generator.end()) {
      co_yield std::move(*it);
      co_await do_await(++it);
    }
  }

  template <typename ItemT>
  Task<ItemT> RenameItem(ItemT item, std::string new_name,
                         stdx::stop_token stop_token) {
    auto context_token = CreateStopToken("RenameItem", std::move(stop_token));
    co_return co_await provider_->RenameItem(
        std::move(item), std::move(new_name), context_token.GetToken());
  }

  template <IsDirectory<CloudProviderT> DirectoryT>
  auto CreateDirectory(DirectoryT parent, std::string name,
                       stdx::stop_token stop_token)
      -> decltype(provider_->CreateDirectory(parent, name, stop_token)) {
    auto context_token =
        CreateStopToken("CreateDirectory", std::move(stop_token));
    co_return co_await provider_->CreateDirectory(
        std::move(parent), std::move(name), context_token.GetToken());
  }

  template <typename ItemT>
  Task<> RemoveItem(ItemT item, stdx::stop_token stop_token) {
    auto context_token = CreateStopToken("RemoveItem", std::move(stop_token));
    co_return co_await provider_->RemoveItem(std::move(item),
                                             context_token.GetToken());
  }

  template <typename ItemT, typename DirectoryT>
  Task<ItemT> MoveItem(ItemT source, DirectoryT destination,
                       stdx::stop_token stop_token) {
    auto context_token = CreateStopToken("MoveItem", std::move(stop_token));
    co_return co_await provider_->MoveItem(
        std::move(source), std::move(destination), context_token.GetToken());
  }

  template <IsDirectory<CloudProviderT> DirectoryT>
  auto CreateFile(DirectoryT parent, std::string_view name,
                  typename CloudProviderT::FileContent content,
                  stdx::stop_token stop_token)
      -> decltype(provider_->CreateFile(parent, name, std::move(content),
                                        stop_token)) {
    stdx::stop_source stop_source;
    stdx::stop_callback cb(std::move(stop_token),
                           [&] { stop_source.request_stop(); });
    auto scope_guard =
        coro::util::AtScopeExit([&] { stop_source.request_stop(); });

    int64_t chunk_index = 0;
    content.data =
        ContentStream(std::move(content.data), &chunk_index, &stop_source);

    RunTask(InstallTimer(&chunk_index, &stop_source));

    co_return co_await provider_->CreateFile(parent, name, std::move(content),
                                             stop_source.get_token());
  }

 private:
  Task<> InstallTimer(int64_t* chunk_index,
                      stdx::stop_source* stop_source) const {
    int64_t current_chunk_index = *chunk_index;
    co_await event_loop_->Wait(timeout_ms_, stop_source->get_token());
    if (current_chunk_index == *chunk_index) {
      stop_source->request_stop();
    }
  }

  Generator<std::string> ContentStream(Generator<std::string> generator,
                                       int64_t* chunk_index,
                                       stdx::stop_source* stop_source) const {
    auto it = co_await generator.begin();
    while (it != generator.end()) {
      (*chunk_index)++;
      RunTask(InstallTimer(chunk_index, stop_source));
      co_yield std::move(*it);
      co_await ++it;
    }
  }

  class ContextStopToken {
   public:
    ContextStopToken(coro::util::EventLoop* event_loop, std::string action,
                     int timeout_ms, stdx::stop_token stop_token)
        : timing_out_stop_token_(*event_loop, std::move(action), timeout_ms),
          token_or_(timing_out_stop_token_.GetToken(), std::move(stop_token)) {}

    auto GetToken() const { return token_or_.GetToken(); }

   private:
    TimingOutStopToken timing_out_stop_token_;
    coro::util::StopTokenOr token_or_;
  };

  ContextStopToken CreateStopToken(std::string action,
                                   stdx::stop_token stop_token) const {
    return ContextStopToken(event_loop_, std::move(action), timeout_ms_,
                            std::move(stop_token));
  }
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_TIMING_OUT_CLOUD_PROVIDER_H
