#ifndef CORO_CLOUDSTORAGE_FUSE_TIMING_OUT_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_FUSE_TIMING_OUT_CLOUD_PROVIDER_H

#include <utility>

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/timing_out_stop_token.h"
#include "coro/util/event_loop.h"
#include "coro/util/stop_token_or.h"

namespace coro::cloudstorage::util {

class TimingOutCloudProvider : public AbstractCloudProvider {
 public:
  TimingOutCloudProvider(const coro::util::EventLoop* event_loop,
                         int timeout_ms, AbstractCloudProvider* provider)
      : event_loop_(event_loop), timeout_ms_(timeout_ms), provider_(provider) {}

  bool IsFileContentSizeRequired(
      const AbstractCloudProvider::Directory& d) const override;

  std::string_view GetId() const override;

  Task<AbstractCloudProvider::Directory> GetRoot(
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::PageData> ListDirectoryPage(
      AbstractCloudProvider::Directory directory,
      std::optional<std::string> page_token,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::GeneralData> GetGeneralData(
      stdx::stop_token stop_token) const override;

  Generator<std::string> GetFileContent(
      AbstractCloudProvider::File file, http::Range range,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::File> RenameItem(
      AbstractCloudProvider::File item, std::string new_name,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::Directory> RenameItem(
      AbstractCloudProvider::Directory item, std::string new_name,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::Directory> CreateDirectory(
      AbstractCloudProvider::Directory parent, std::string name,
      stdx::stop_token stop_token) const override;

  Task<> RemoveItem(AbstractCloudProvider::Directory item,
                    stdx::stop_token stop_token) const override;

  Task<> RemoveItem(AbstractCloudProvider::File item,
                    stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::File> MoveItem(
      AbstractCloudProvider::File source,
      AbstractCloudProvider::Directory destination,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::Directory> MoveItem(
      AbstractCloudProvider::Directory source,
      AbstractCloudProvider::Directory destination,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::File> CreateFile(
      AbstractCloudProvider::Directory parent, std::string name,
      AbstractCloudProvider::FileContent content,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::Thumbnail> GetItemThumbnail(
      AbstractCloudProvider::File item, http::Range range,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::Thumbnail> GetItemThumbnail(
      AbstractCloudProvider::Directory item, http::Range range,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::Thumbnail> GetItemThumbnail(
      AbstractCloudProvider::File item, ThumbnailQuality, http::Range range,
      stdx::stop_token stop_token) const override;

  Task<AbstractCloudProvider::Thumbnail> GetItemThumbnail(
      AbstractCloudProvider::Directory item, ThumbnailQuality,
      http::Range range, stdx::stop_token stop_token) const override;

 private:
  Task<> InstallTimer(int64_t* chunk_index,
                      stdx::stop_source* stop_source) const;

  Generator<std::string> ContentStream(Generator<std::string> generator,
                                       int64_t* chunk_index,
                                       stdx::stop_source* stop_source) const;

  template <typename ItemT>
  Task<ItemT> Rename(ItemT item, std::string new_name,
                     stdx::stop_token stop_token) const;

  template <typename ItemT>
  Task<> Remove(ItemT item, stdx::stop_token stop_token) const;

  template <typename ItemT>
  Task<ItemT> Move(ItemT source, AbstractCloudProvider::Directory destination,
                   stdx::stop_token stop_token) const;

  class ContextStopToken {
   public:
    ContextStopToken(const coro::util::EventLoop* event_loop,
                     std::string action, int timeout_ms,
                     stdx::stop_token stop_token)
        : timing_out_stop_token_(*event_loop, std::move(action), timeout_ms),
          token_or_(timing_out_stop_token_.GetToken(), std::move(stop_token)) {}

    stdx::stop_token GetToken() const { return token_or_.GetToken(); }

   private:
    TimingOutStopToken timing_out_stop_token_;
    coro::util::StopTokenOr<2> token_or_;
  };

  ContextStopToken CreateStopToken(std::string action,
                                   stdx::stop_token stop_token) const;

  const coro::util::EventLoop* event_loop_;
  int timeout_ms_;
  AbstractCloudProvider* provider_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_TIMING_OUT_CLOUD_PROVIDER_H
