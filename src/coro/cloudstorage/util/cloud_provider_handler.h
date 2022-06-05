#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_HANDLER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_HANDLER_H

#include <fmt/format.h>

#include <iomanip>
#include <sstream>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/handler_utils.h"
#include "coro/cloudstorage/util/settings_manager.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/cloudstorage/util/thumbnail_options.h"
#include "coro/cloudstorage/util/thumbnail_quality.h"
#include "coro/cloudstorage/util/webdav_handler.h"
#include "coro/http/http_parse.h"
#include "coro/util/lru_cache.h"
#include "coro/util/regex.h"

namespace coro::cloudstorage::util {

class CloudProviderHandler {
 public:
  using CloudProvider = AbstractCloudProvider::CloudProvider;
  using Request = http::Request<>;
  using Response = http::Response<>;

  CloudProviderHandler(CloudProvider* provider,
                       const ThumbnailGenerator* thumbnail_generator,
                       const SettingsManager* settings_manager)
      : provider_(provider),
        thumbnail_generator_(thumbnail_generator),
        settings_manager_(settings_manager) {}

  Task<Response> operator()(Request request, stdx::stop_token stop_token);

 private:
  std::string GetItemPathPrefix(
      std::span<const std::pair<std::string, std::string>> headers) const;

  Task<std::string> GenerateThumbnail(const AbstractCloudProvider::File& item,
                                      stdx::stop_token stop_token) const;

  template <typename Item>
  Response GetStaticIcon(const Item& item) const;

  template <typename Item>
  Task<Response> GetIcon(const Item& item, stdx::stop_token stop_token) const;

  template <typename Item>
  Task<Response> GetItemThumbnail(Item d, ThumbnailQuality,
                                  stdx::stop_token stop_token) const;

  Task<Response> HandleExistingItem(Request request,
                                    AbstractCloudProvider::File d,
                                    stdx::stop_token stop_token);

  Task<Response> HandleExistingItem(Request request,
                                    AbstractCloudProvider::Directory d,
                                    stdx::stop_token stop_token);

  template <typename Item>
  std::string GetItemEntry(const Item& item, std::string_view path,
                           bool use_dash_player) const;

  Generator<std::string> GetDirectoryContent(
      std::string path_prefix,
      Generator<AbstractCloudProvider::PageData> page_data,
      std::string path) const;

  CloudProvider* provider_;
  const ThumbnailGenerator* thumbnail_generator_;
  const SettingsManager* settings_manager_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_HANDLER_H
