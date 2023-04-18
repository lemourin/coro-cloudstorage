#include "coro/cloudstorage/util/mux_handler.h"

#include <string>

#include "coro/cloudstorage/cloud_exception.h"
#include "coro/cloudstorage/util/cloud_provider_utils.h"
#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http_parse.h"
#include "coro/stdx/stop_source.h"
#include "coro/util/stop_token_or.h"
#include "coro/when_all.h"

namespace coro::cloudstorage::util {

namespace {

using ::coro::util::MakeUniqueStopTokenOr;

constexpr const char* kMp4Sample =
    R"(/mux/?video_account_type=mega&audio_account_type=mega&video_account_name=lemourin%40gmail.com&audio_account_name=lemourin%40gmail.com&video_path=/I%E2%80%99m%20Your%20Treasure%20Box%20%EF%BC%8A%E3%81%82%E3%81%AA%E3%81%9F%E3%81%AF%20%E3%83%9E%E3%83%AA%E3%83%B3%E3%81%9B%E3%82%93%E3%81%A1%E3%82%87%E3%81%86%E3%82%92%20%E3%81%9F%E3%81%8B%E3%82%89%E3%81%B0%E3%81%93%E3%81%8B%E3%82%89%E3%81%BF%E3%81%A4%E3%81%91%E3%81%9F/%E3%80%90original%20anime%20MV%E3%80%91I%E2%80%99m%20Your%20Treasure%20Box%20%EF%BC%8A%E3%81%82%E3%81%AA%E3%81%9F%E3%81%AF%20%E3%83%9E%E3%83%AA%E3%83%B3%E3%81%9B%E3%82%93%E3%81%A1%E3%82%87%E3%81%86%E3%82%92%20%E3%81%9F%E3%81%8B%E3%82%89%E3%81%B0%E3%81%93%E3%81%8B%E3%82%89%E3%81%BF%E3%81%A4%E3%81%91%E3%81%9F%E3%80%82%E3%80%90hololive%E2%A7%B8%E5%AE%9D%E9%90%98%E3%83%9E%E3%83%AA%E3%83%B3%E3%80%91%20%5BvV-5W7SFHDc%5D.mp4&audio_path=/I%E2%80%99m%20Your%20Treasure%20Box%20%EF%BC%8A%E3%81%82%E3%81%AA%E3%81%9F%E3%81%AF%20%E3%83%9E%E3%83%AA%E3%83%B3%E3%81%9B%E3%82%93%E3%81%A1%E3%82%87%E3%81%86%E3%82%92%20%E3%81%9F%E3%81%8B%E3%82%89%E3%81%B0%E3%81%93%E3%81%8B%E3%82%89%E3%81%BF%E3%81%A4%E3%81%91%E3%81%9F/%E3%80%90original%20anime%20MV%E3%80%91I%E2%80%99m%20Your%20Treasure%20Box%20%EF%BC%8A%E3%81%82%E3%81%AA%E3%81%9F%E3%81%AF%20%E3%83%9E%E3%83%AA%E3%83%B3%E3%81%9B%E3%82%93%E3%81%A1%E3%82%87%E3%81%86%E3%82%92%20%E3%81%9F%E3%81%8B%E3%82%89%E3%81%B0%E3%81%93%E3%81%8B%E3%82%89%E3%81%BF%E3%81%A4%E3%81%91%E3%81%9F%E3%80%82%E3%80%90hololive%E2%A7%B8%E5%AE%9D%E9%90%98%E3%83%9E%E3%83%AA%E3%83%B3%E3%80%91%20%5BvV-5W7SFHDc%5D.m4a&format=mp4)";
constexpr const char* kWebmSample =
    R"(/mux/?video_account_type=mega&audio_account_type=mega&video_account_name=lemourin%40gmail.com&audio_account_name=lemourin%40gmail.com&video_path=/Imagine%20Dragons%20%26%20JID%20-%20Enemy%20%28from%20the%20series%20Arcane%EF%BC%9A%20League%20of%20Legends%29%20%EF%BD%9C%20Official%20Music%20Video/Imagine%20Dragons%20%26%20JID%20-%20Enemy%20%28from%20the%20series%20Arcane%EF%BC%9A%20League%20of%20Legends%29%20%EF%BD%9C%20Official%20Music%20Video%20%5BF5tSoaJ93ac%5D.webm&audio_path=/Imagine%20Dragons%20%26%20JID%20-%20Enemy%20%28from%20the%20series%20Arcane%EF%BC%9A%20League%20of%20Legends%29%20%EF%BD%9C%20Official%20Music%20Video/Imagine%20Dragons%20%26%20JID%20-%20Enemy%20%28from%20the%20series%20Arcane%EF%BC%9A%20League%20of%20Legends%29%20%EF%BD%9C%20Official%20Music%20Video%20%5BF5tSoaJ93ac%5D.audio.webm&format=webm)";

std::vector<std::string> GetPathComponents(const std::string& encoded_path) {
  std::vector<std::string> components = SplitString(encoded_path, '/');
  for (auto& component : components) {
    component = http::DecodeUri(component);
  }
  return components;
}

std::shared_ptr<CloudProviderAccount> FindAccount(
    std::span<const std::shared_ptr<CloudProviderAccount>> accounts,
    const CloudProviderAccount::Id& account_id) {
  for (const auto& account : accounts) {
    if (account->id() == account_id) {
      return account;
    }
  }
  throw CloudException(CloudException::Type::kNotFound);
}

}  // namespace

Task<http::Response<>> MuxHandler::operator()(
    http::Request<> request, stdx::stop_token stop_token) const {
  http::Uri uri = http::ParseUri(request.url);
  if (uri.path == "/mux/sample.mp4") {
    co_return http::Response<>{
        .status = 302,
        .headers = {
            {"Location", StrCat(kMp4Sample, '&', uri.query.value_or(""))}}};
  }
  if (uri.path == "/mux/sample.webm") {
    co_return http::Response<>{
        .status = 302,
        .headers = {
            {"Location", StrCat(kWebmSample, '&', uri.query.value_or(""))}}};
  }
  if (!uri.query) {
    co_return http::Response<>{.status = 400};
  }
  auto query = http::ParseQuery(uri.query.value());
  auto video_account_type = query.find("video_account_type");
  auto video_account_name = query.find("video_account_name");
  auto video_path = query.find("video_path");
  auto audio_account_type = query.find("audio_account_type");
  auto audio_account_name = query.find("audio_account_name");
  auto audio_path = query.find("audio_path");
  auto format = query.find("format");
  auto seekable = query.find("seekable");
  if (video_account_type == query.end() || video_account_name == query.end() ||
      video_path == query.end() || audio_account_type == query.end() ||
      audio_account_name == query.end() || audio_path == query.end() ||
      format == query.end()) {
    co_return http::Response<>{.status = 400};
  }
  if (format->second != "webm" && format->second != "mp4") {
    co_return http::Response<>{.status = 400};
  }

  auto video_account =
      FindAccount(accounts_, {http::DecodeUri(video_account_type->second),
                              http::DecodeUri(video_account_name->second)});
  auto audio_account =
      FindAccount(accounts_, {http::DecodeUri(audio_account_type->second),
                              http::DecodeUri(audio_account_name->second)});

  auto video_path_components = GetPathComponents(video_path->second);
  auto audio_path_components = GetPathComponents(audio_path->second);

  auto stop_token_or =
      MakeUniqueStopTokenOr(video_account->stop_token(),
                            audio_account->stop_token(), std::move(stop_token));

  auto [video_item, audio_item] = co_await WhenAll(
      GetItemByPathComponents(video_account->provider().get(),
                              video_path_components, stop_token_or->GetToken()),
      GetItemByPathComponents(audio_account->provider().get(),
                              audio_path_components,
                              stop_token_or->GetToken()));

  auto video_file = std::get<AbstractCloudProvider::File>(video_item);
  auto audio_file = std::get<AbstractCloudProvider::File>(audio_item);

  bool is_seekable =
      seekable != query.end() ? seekable->second == "true" : false;
  Generator<std::string> content =
      (*muxer_)(video_account->provider().get(), video_file,
                audio_account->provider().get(), audio_file,
                {.container = format->second == "mp4" ? MediaContainer::kMp4
                                                      : MediaContainer::kWebm,
                 .buffered = is_seekable},
                stop_token_or->GetToken());
  co_return http::Response<>{
      .status = 200,
      .headers =
          {
              {"Content-Type", is_seekable ? "application/octet-stream"
                                           : "video/" + format->second},
              {"Content-Disposition",
               "inline; filename=\"" + video_file.name + "\""},
          },
      .body = Forward(std::move(content), video_account, audio_account,
                      std::move(stop_token_or))};
}

}  // namespace coro::cloudstorage::util
