#ifndef CORO_CLOUDSTORAGE_UTIL_STALE_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_UTIL_STALE_CLOUD_PROVIDER_H

#include <nlohmann/json.hpp>

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/cache_manager.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"

namespace coro::cloudstorage::util {

class StaleCloudProvider : public AbstractCloudProvider {
 public:
  StaleCloudProvider(AbstractCloudProvider* provider,
                     CloudProviderCacheManager cache_manager,
                     const ThumbnailGenerator* thumbnail_generator)
      : provider_(provider),
        cache_manager_(std::move(cache_manager)),
        thumbnail_generator_(thumbnail_generator) {}

  std::string_view GetId() const override { return provider_->GetId(); }

  Task<Directory> GetRoot(stdx::stop_token stop_token) const override {
    return provider_->GetRoot(std::move(stop_token));
  }

  nlohmann::json ToJson(
      const AbstractCloudProvider::Item& item) const override {
    return provider_->ToJson(item);
  }

  AbstractCloudProvider::Item ToItem(
      const nlohmann::json& json) const override {
    return provider_->ToItem(json);
  }

  bool IsFileContentSizeRequired(const Directory& directory) const {
    return provider_->IsFileContentSizeRequired(directory);
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) const override;

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) const override {
    return provider_->GetGeneralData(std::move(stop_token));
  }

  Generator<std::string> GetFileContent(
      File file, http::Range range,
      stdx::stop_token stop_token) const override {
    return provider_->GetFileContent(std::move(file), range,
                                     std::move(stop_token));
  }

  Task<Directory> RenameItem(Directory item, std::string new_name,
                             stdx::stop_token stop_token) const override {
    return provider_->RenameItem(std::move(item), std::move(new_name),
                                 std::move(stop_token));
  }

  Task<File> RenameItem(File item, std::string new_name,
                        stdx::stop_token stop_token) const override {
    return provider_->RenameItem(std::move(item), std::move(new_name),
                                 std::move(stop_token));
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) const override {
    return provider_->CreateDirectory(std::move(parent), std::move(name),
                                      std::move(stop_token));
  }

  Task<> RemoveItem(Directory item,
                    stdx::stop_token stop_token) const override {
    return provider_->RemoveItem(std::move(item), std::move(stop_token));
  }

  Task<> RemoveItem(File item, stdx::stop_token stop_token) const override {
    return provider_->RemoveItem(std::move(item), std::move(stop_token));
  }

  Task<File> MoveItem(File source, Directory destination,
                      stdx::stop_token stop_token) const override {
    return provider_->MoveItem(std::move(source), std::move(destination),
                               std::move(stop_token));
  }

  Task<Directory> MoveItem(Directory source, Directory destination,
                           stdx::stop_token stop_token) const override {
    return provider_->MoveItem(std::move(source), std::move(destination),
                               std::move(stop_token));
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content,
                        stdx::stop_token stop_token) const override {
    co_return co_await provider_->CreateFile(
        std::move(parent), name, std::move(content), std::move(stop_token));
  }

  Task<Thumbnail> GetItemThumbnail(File item, http::Range range,
                                   stdx::stop_token stop_token) const override {
    return GetItemThumbnail(std::move(item), ThumbnailQuality::kLow, range,
                            std::move(stop_token));
  }

  Task<Thumbnail> GetItemThumbnail(Directory item, http::Range range,
                                   stdx::stop_token stop_token) const override {
    return GetItemThumbnail(std::move(item), ThumbnailQuality::kLow, range,
                            std::move(stop_token));
  }

  Task<Thumbnail> GetItemThumbnail(File item, ThumbnailQuality,
                                   http::Range range,
                                   stdx::stop_token stop_token) const override;

  Task<Thumbnail> GetItemThumbnail(Directory item, ThumbnailQuality,
                                   http::Range range,
                                   stdx::stop_token stop_token) const override;

 private:
  AbstractCloudProvider* provider_;
  CloudProviderCacheManager cache_manager_;
  const ThumbnailGenerator* thumbnail_generator_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_STALE_CLOUD_PROVIDER_H