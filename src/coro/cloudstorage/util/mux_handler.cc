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
    R"(/mux/?video_account_type=mega&audio_account_type=mega&video_account_name=lemourin%40gmail.com&audio_account_name=lemourin%40gmail.com&video_id=1009087793198683328&audio_id=11838556048967991440&format=mp4)";
constexpr const char* kWebmSample =
    R"(/mux/?video_account_type=mega&audio_account_type=mega&video_account_name=lemourin%40gmail.com&audio_account_name=lemourin%40gmail.com&video_id=12293138135299678493&audio_id=3034018774527793109&format=webm)";

CloudProviderAccount FindAccount(std::span<const CloudProviderAccount> accounts,
                                 const CloudProviderAccount::Id& account_id) {
  for (const auto& account : accounts) {
    if (account.id() == account_id) {
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
  auto video_id = query.find("video_id");
  auto audio_account_type = query.find("audio_account_type");
  auto audio_account_name = query.find("audio_account_name");
  auto audio_id = query.find("audio_id");
  auto format = query.find("format");
  auto seekable = query.find("seekable");
  if (video_account_type == query.end() || video_account_name == query.end() ||
      video_id == query.end() || audio_account_type == query.end() ||
      audio_account_name == query.end() || audio_id == query.end() ||
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

  auto stop_token_or =
      MakeUniqueStopTokenOr(video_account.stop_token(),
                            audio_account.stop_token(), std::move(stop_token));

  auto [video_item, audio_item] = co_await WhenAll(
      GetItemById(video_account.provider().get(), video_id->second,
                  stop_token_or->GetToken()),
      GetItemById(audio_account.provider().get(), audio_id->second,
                  stop_token_or->GetToken()));

  auto video_file = std::get<AbstractCloudProvider::File>(video_item);
  auto audio_file = std::get<AbstractCloudProvider::File>(audio_item);

  bool is_seekable =
      seekable != query.end() ? seekable->second == "true" : false;
  Generator<std::string> content =
      (*muxer_)(video_account.provider().get(), video_file,
                audio_account.provider().get(), audio_file,
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
