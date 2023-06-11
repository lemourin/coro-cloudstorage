#include "coro/cloudstorage/util/cloud_provider_utils.h"

#include "coro/cloudstorage/util/generator_utils.h"

namespace coro::cloudstorage::util {

namespace {

using Item = AbstractCloudProvider::Item;

Task<Item> GetItemByPathComponents(
    const AbstractCloudProvider* p,
    AbstractCloudProvider::Directory current_directory,
    std::span<const std::string> components, stdx::stop_token stop_token) {
  if (components.empty()) {
    co_return current_directory;
  }
  FOR_CO_AWAIT(auto& page, ListDirectory(p, current_directory, stop_token)) {
    for (auto& item : page.items) {
      auto r = std::visit(
          [&]<typename T>(
              T& d) -> std::variant<std::monostate, Task<Item>, Item> {
            if constexpr (std::is_same_v<T, AbstractCloudProvider::Directory>) {
              if (d.name == components.front()) {
                return GetItemByPathComponents(
                    p, std::move(d), components.subspan(1), stop_token);
              }
            } else {
              if (d.name == components.front()) {
                return std::move(d);
              }
            }
            return std::monostate();
          },
          item);
      if (std::holds_alternative<Task<Item>>(r)) {
        co_return co_await std::get<Task<Item>>(r);
      } else if (std::holds_alternative<Item>(r)) {
        co_return std::move(std::get<Item>(r));
      }
    }
  }
  throw CloudException(CloudException::Type::kNotFound);
}

Task<Item> GetItemByPath(const AbstractCloudProvider* d,
                         AbstractCloudProvider::Directory current_directory,
                         std::string_view path, stdx::stop_token stop_token) {
  co_return co_await GetItemByPathComponents(
      d, std::move(current_directory), SplitString(std::string(path), '/'),
      std::move(stop_token));
}

Task<std::string> GenerateThumbnail(
    const ThumbnailGenerator* thumbnail_generator,
    const AbstractCloudProvider* provider, AbstractCloudProvider::File item,
    stdx::stop_token stop_token) {
  switch (GetFileType(item.mime_type)) {
    case FileType::kImage:
    case FileType::kVideo:
      return (*thumbnail_generator)(
          provider, std::move(item),
          ThumbnailOptions{.codec = ThumbnailOptions::Codec::PNG},
          std::move(stop_token));
    default:
      throw CloudException(CloudException::Type::kNotFound);
  }
}

Task<AbstractCloudProvider::Thumbnail> GetThumbnail(
    const ThumbnailGenerator* thumbnail_generator,
    const AbstractCloudProvider* provider, AbstractCloudProvider::File file,
    ThumbnailQuality quality, http::Range range, stdx::stop_token stop_token) {
  try {
    co_return co_await provider->GetItemThumbnail(file, quality, range,
                                                  stop_token);
  } catch (...) {
  }
  std::string image_bytes = co_await GenerateThumbnail(
      thumbnail_generator, provider, file, std::move(stop_token));
  int64_t size = image_bytes.size();
  co_return AbstractCloudProvider::Thumbnail{
      .data = ToGenerator(Trim(std::move(image_bytes), range)),
      .size = size,
      .mime_type = "image/png"};
}

}  // namespace

FileType GetFileType(std::string_view mime_type) {
  if (mime_type.find("audio") == 0) {
    return FileType::kAudio;
  } else if (mime_type.find("image") == 0) {
    return FileType::kImage;
  } else if (mime_type.find("video") == 0) {
    return FileType::kVideo;
  } else {
    return FileType::kUnknown;
  }
}

Task<Item> GetItemByPathComponents(const AbstractCloudProvider* d,
                                   std::vector<std::string> components,
                                   stdx::stop_token stop_token) {
  co_return co_await GetItemByPathComponents(d, co_await d->GetRoot(stop_token),
                                             std::move(components), stop_token);
}

Task<Item> GetItemByPath(const AbstractCloudProvider* d, std::string path,
                         stdx::stop_token stop_token) {
  co_return co_await GetItemByPath(d, co_await d->GetRoot(stop_token),
                                   std::move(path), stop_token);
}

template <>
Task<AbstractCloudProvider::Thumbnail>
GetItemThumbnailWithFallback<AbstractCloudProvider::File>(
    const ThumbnailGenerator* thumbnail_generator,
    const AbstractCloudProvider* provider, AbstractCloudProvider::File file,
    ThumbnailQuality quality, http::Range range, stdx::stop_token stop_token) {
  return GetThumbnail(thumbnail_generator, provider, std::move(file), quality,
                      range, std::move(stop_token));
}

template <>
Task<AbstractCloudProvider::Thumbnail>
GetItemThumbnailWithFallback<AbstractCloudProvider::Directory>(
    const ThumbnailGenerator*, const AbstractCloudProvider* provider,
    AbstractCloudProvider::Directory directory, ThumbnailQuality quality,
    http::Range range, stdx::stop_token stop_token) {
  return provider->GetItemThumbnail(std::move(directory), quality, range,
                                    std::move(stop_token));
}

Task<AbstractCloudProvider::Item> GetItemById(
    const AbstractCloudProvider* provider, std::string id,
    stdx::stop_token stop_token) {
  if (id == kRootId) {
    co_return co_await provider->GetRoot(std::move(stop_token));
  } else {
    co_return co_await provider->GetItem(std::move(id), std::move(stop_token));
  }
}

}  // namespace coro::cloudstorage::util