#include "coro/cloudstorage/util/item_thumbnail_handler.h"

#include <fmt/format.h>

#include <iostream>

#include "coro/cloudstorage/util/cloud_provider_utils.h"
#include "coro/cloudstorage/util/handler_utils.h"
#include "coro/util/regex.h"

namespace coro::cloudstorage::util {

namespace {

namespace re = coro::util::re;

std::string_view GetIconName(AbstractCloudProvider::Directory) {
  return "folder";
}

std::string_view GetIconName(AbstractCloudProvider::File file) {
  switch (GetFileType(file.mime_type)) {
    case FileType::kUnknown:
      return "unknown";
    case FileType::kImage:
      return "image-x-generic";
    case FileType::kAudio:
      return "audio-x-generic";
    case FileType::kVideo:
      return "video-x-generic";
  }
  throw RuntimeError("invalid file type");
}

template <typename Item>
http::Response<> GetStaticIcon(const Item& item, int http_code) {
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Location", StrCat("/static/", GetIconName(item), ".svg")}};
  if (http_code == 301) {
    headers.emplace_back("Cache-Control", "private");
    headers.emplace_back("Cache-Control", "max-age=604800");
  }
  return http::Response<>{.status = http_code, .headers = std::move(headers)};
}

template <typename Item>
Task<http::Response<>> GetItemThumbnail(CloudProviderAccount account, Item d,
                                        ThumbnailQuality quality,
                                        std::optional<http::Range> range,
                                        stdx::stop_token stop_token) {
  try {
    auto data = co_await account.GetItemThumbnailWithFallback(
        d, quality, range.value_or(http::Range{}), stop_token);
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Cache-Control", "private"},
        {"Cache-Control", "max-age=604800"},
        {"Content-Type", std::string(data.thumbnail.mime_type)}};
    http::Range drange = range.value_or(http::Range{});
    if (!drange.end) {
      drange.end = data.thumbnail.size - 1;
    }
    headers.emplace_back("Accept-Ranges", "bytes");
    headers.emplace_back("Content-Length",
                         std::to_string(*drange.end - drange.start + 1));
    if (range) {
      headers.emplace_back("Content-Range",
                           fmt::format("bytes {}-{}/{}", drange.start,
                                       *drange.end, data.thumbnail.size));
    }
    co_return http::Response<>{.status = range ? 206 : 200,
                               .headers = std::move(headers),
                               .body = std::move(data.thumbnail.data)};
  } catch (const ThumbnailGeneratorException& e) {
    std::cerr << "FAILED TO GENERATE THUMBNAIL " << e.what() << '\n';
    co_return GetStaticIcon(d, 302);
  } catch (const CloudException& e) {
    co_return GetStaticIcon(
        d,
        /*http_code=*/e.type() == CloudException::Type::kNotFound ? 301 : 302);
  } catch (...) {
    co_return GetStaticIcon(d, /*http_code=*/302);
  }
}

}  // namespace

Task<http::Response<>> ItemThumbnailHandler::operator()(
    http::Request<> request, stdx::stop_token stop_token) const {
  auto uri = http::ParseUri(request.url);
  re::smatch results;
  if (!re::regex_match(uri.path.value(), results,
                       re::regex(R"(\/thumbnail\/[^\/]+\/[^\/]+\/(.*)$)"))) {
    co_return http::Response<>{.status = 400};
  }
  ThumbnailQuality quality = [&] {
    if (!uri.query) {
      return ThumbnailQuality::kLow;
    }
    auto query = http::ParseQuery(*uri.query);
    if (auto it = query.find("quality"); it != query.end()) {
      return it->second == "high" ? ThumbnailQuality::kHigh
                                  : ThumbnailQuality::kLow;
    }
    return ThumbnailQuality::kLow;
  }();
  std::string item_id =
      http::DecodeUri(ToStringView(results[1].begin(), results[1].end()));
  auto range = [&]() -> std::optional<http::Range> {
    if (auto header = http::GetHeader(request.headers, "Range")) {
      return http::ParseRange(std::move(*header));
    } else {
      return std::nullopt;
    }
  }();
  auto item = co_await account_.GetItemById(item_id, stop_token);
  co_return co_await std::visit(
      [account = account_, quality, range,
       stop_token = std::move(stop_token)](auto item) mutable {
        return GetItemThumbnail(account, std::move(item), quality, range,
                                std::move(stop_token));
      },
      std::move(item.item));
}

}  // namespace coro::cloudstorage::util