#include "coro/cloudstorage/util/mux_handler.h"

#include <string>

#include "coro/cloudstorage/cloud_exception.h"
#include "coro/cloudstorage/util/cloud_provider_utils.h"
#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http_parse.h"
#include "coro/stdx/stop_source.h"
#include "coro/when_all.h"

namespace coro::cloudstorage::util {

namespace {

struct OnCancel {
  void operator()() const { stop_source->request_stop(); }
  stdx::stop_source* stop_source;
};

template <int N>
class StopTokenWrapper {
 public:
  template <typename... Args>
  explicit StopTokenWrapper(Args&&... args)
      : stop_callbacks_{
            {{std::forward<Args>(args), OnCancel{&stop_source_}}...}} {}

  template <typename... Args>
  explicit StopTokenWrapper(stdx::stop_source stop_source, Args&&... args)
      : stop_source_(std::move(stop_source)),
        stop_callbacks_{
            {{std::forward<Args>(args), OnCancel{&stop_source_}}...}} {}

  stdx::stop_token stop_token() const { return stop_source_.get_token(); }

 private:
  stdx::stop_source stop_source_;
  std::array<stdx::stop_callback<OnCancel>, N> stop_callbacks_;
};

template <typename... Args>
auto MakeStopTokenOr(Args&&... stop_token) {
  return std::make_unique<StopTokenWrapper<sizeof...(Args)>>(
      std::forward<Args>(stop_token)...);
}

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
  if (video_account_type == query.end() || video_account_name == query.end() ||
      video_path == query.end() || audio_account_type == query.end() ||
      audio_account_name == query.end() || audio_path == query.end()) {
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
      MakeStopTokenOr(video_account->stop_token(), audio_account->stop_token(),
                      std::move(stop_token));

  auto [video_item, audio_item] =
      co_await WhenAll(GetItemByPathComponents(video_account->provider().get(),
                                               video_path_components,
                                               stop_token_or->stop_token()),
                       GetItemByPathComponents(audio_account->provider().get(),
                                               audio_path_components,
                                               stop_token_or->stop_token()));

  Generator<std::string> content =
      (*muxer_)(video_account->provider().get(),
                std::get<AbstractCloudProvider::File>(video_item),
                audio_account->provider().get(),
                std::get<AbstractCloudProvider::File>(audio_item),
                {.container = MediaContainer::kMp4, .buffered = false},
                stop_token_or->stop_token());
  co_return http::Response<>{
      .status = 200,
      .headers = {{"Content-Type", "video/mp4"}},
      .body = Forward(std::move(content), video_account, audio_account,
                      std::move(stop_token_or))};
}

}  // namespace coro::cloudstorage::util
