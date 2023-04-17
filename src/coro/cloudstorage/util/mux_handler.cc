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

  Generator<std::string> content =
      (*muxer_)(video_account->provider().get(), video_file,
                audio_account->provider().get(), audio_file,
                {.container = format->second == "mp4" ? MediaContainer::kMp4
                                                      : MediaContainer::kWebm,
                 .buffered = false},
                stop_token_or->GetToken());
  co_return http::Response<>{
      .status = 200,
      .headers =
          {
              {"Content-Type", "video/" + format->second},
              {"Content-Disposition",
               "inline; filename=\"" + video_file.name + "\""},
          },
      .body = Forward(std::move(content), video_account, audio_account,
                      std::move(stop_token_or))};
}

}  // namespace coro::cloudstorage::util
