#include "coro/cloudstorage/util/timing_out_cloud_provider.h"

#include "coro/util/raii_utils.h"

namespace coro::cloudstorage::util {

bool TimingOutCloudProvider::IsFileContentSizeRequired(
    const AbstractCloudProvider::Directory& d) const {
  return provider_->IsFileContentSizeRequired(d);
}

std::string_view TimingOutCloudProvider::GetId() const {
  return provider_->GetId();
}

Task<AbstractCloudProvider::Directory> TimingOutCloudProvider::GetRoot(
    stdx::stop_token stop_token) const {
  auto context_token = CreateStopToken("GetRoot", std::move(stop_token));
  co_return co_await provider_->GetRoot(context_token.GetToken());
}

Task<AbstractCloudProvider::PageData> TimingOutCloudProvider::ListDirectoryPage(
    AbstractCloudProvider::Directory directory,
    std::optional<std::string> page_token, stdx::stop_token stop_token) const {
  auto context_token =
      CreateStopToken("ListDirectoryPage", std::move(stop_token));
  co_return co_await provider_->ListDirectoryPage(
      std::move(directory), std::move(page_token), context_token.GetToken());
}

Task<AbstractCloudProvider::GeneralData> TimingOutCloudProvider::GetGeneralData(
    stdx::stop_token stop_token) const {
  auto context_token = CreateStopToken("GetGeneralData", std::move(stop_token));
  co_return co_await provider_->GetGeneralData(context_token.GetToken());
}

Generator<std::string> TimingOutCloudProvider::GetFileContent(
    AbstractCloudProvider::File file, http::Range range,
    stdx::stop_token stop_token) const {
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

Task<AbstractCloudProvider::File> TimingOutCloudProvider::RenameItem(
    AbstractCloudProvider::File item, std::string new_name,
    stdx::stop_token stop_token) const {
  return Rename(std::move(item), std::move(new_name), std::move(stop_token));
}

Task<AbstractCloudProvider::Directory> TimingOutCloudProvider::RenameItem(
    AbstractCloudProvider::Directory item, std::string new_name,
    stdx::stop_token stop_token) const {
  return Rename(std::move(item), std::move(new_name), std::move(stop_token));
}

Task<AbstractCloudProvider::Directory> TimingOutCloudProvider::CreateDirectory(
    AbstractCloudProvider::Directory parent, std::string name,
    stdx::stop_token stop_token) const {
  auto context_token =
      CreateStopToken("CreateDirectory", std::move(stop_token));
  co_return co_await provider_->CreateDirectory(
      std::move(parent), std::move(name), context_token.GetToken());
}

Task<> TimingOutCloudProvider::RemoveItem(AbstractCloudProvider::Directory item,
                                          stdx::stop_token stop_token) const {
  return Remove(std::move(item), std::move(stop_token));
}

Task<> TimingOutCloudProvider::RemoveItem(AbstractCloudProvider::File item,
                                          stdx::stop_token stop_token) const {
  return Remove(std::move(item), std::move(stop_token));
}

Task<AbstractCloudProvider::File> TimingOutCloudProvider::MoveItem(
    AbstractCloudProvider::File source,
    AbstractCloudProvider::Directory destination,
    stdx::stop_token stop_token) const {
  return Move(std::move(source), std::move(destination), std::move(stop_token));
}

Task<AbstractCloudProvider::Directory> TimingOutCloudProvider::MoveItem(
    AbstractCloudProvider::Directory source,
    AbstractCloudProvider::Directory destination,
    stdx::stop_token stop_token) const {
  return Move(std::move(source), std::move(destination), std::move(stop_token));
}

Task<AbstractCloudProvider::File> TimingOutCloudProvider::CreateFile(
    AbstractCloudProvider::Directory parent, std::string name,
    AbstractCloudProvider::FileContent content,
    stdx::stop_token stop_token) const {
  stdx::stop_source stop_source;
  stdx::stop_callback cb(std::move(stop_token),
                         [&] { stop_source.request_stop(); });
  auto scope_guard =
      coro::util::AtScopeExit([&] { stop_source.request_stop(); });

  int64_t chunk_index = 0;
  content.data =
      ContentStream(std::move(content.data), &chunk_index, &stop_source);

  RunTask(InstallTimer(&chunk_index, &stop_source));

  co_return co_await provider_->CreateFile(
      parent, std::move(name), std::move(content), stop_source.get_token());
}

Task<AbstractCloudProvider::Thumbnail> TimingOutCloudProvider::GetItemThumbnail(
    AbstractCloudProvider::File item, http::Range range,
    stdx::stop_token stop_token) const {
  return provider_->GetItemThumbnail(std::move(item), range,
                                     std::move(stop_token));
}

Task<AbstractCloudProvider::Thumbnail> TimingOutCloudProvider::GetItemThumbnail(
    AbstractCloudProvider::Directory item, http::Range range,
    stdx::stop_token stop_token) const {
  return provider_->GetItemThumbnail(std::move(item), range,
                                     std::move(stop_token));
}

Task<AbstractCloudProvider::Thumbnail> TimingOutCloudProvider::GetItemThumbnail(
    AbstractCloudProvider::File item, ThumbnailQuality quality,
    http::Range range, stdx::stop_token stop_token) const {
  return provider_->GetItemThumbnail(std::move(item), quality, range,
                                     std::move(stop_token));
}

Task<AbstractCloudProvider::Thumbnail> TimingOutCloudProvider::GetItemThumbnail(
    AbstractCloudProvider::Directory item, ThumbnailQuality quality,
    http::Range range, stdx::stop_token stop_token) const {
  return provider_->GetItemThumbnail(std::move(item), quality, range,
                                     std::move(stop_token));
}

Task<> TimingOutCloudProvider::InstallTimer(
    int64_t* chunk_index, stdx::stop_source* stop_source) const {
  int64_t current_chunk_index = *chunk_index;
  co_await event_loop_->Wait(timeout_ms_, stop_source->get_token());
  if (current_chunk_index == *chunk_index) {
    stop_source->request_stop();
  }
}

Generator<std::string> TimingOutCloudProvider::ContentStream(
    Generator<std::string> generator, int64_t* chunk_index,
    stdx::stop_source* stop_source) const {
  auto it = co_await generator.begin();
  while (it != generator.end()) {
    (*chunk_index)++;
    RunTask(InstallTimer(chunk_index, stop_source));
    co_yield std::move(*it);
    co_await ++it;
  }
}

template <typename ItemT>
Task<ItemT> TimingOutCloudProvider::Rename(ItemT item, std::string new_name,
                                           stdx::stop_token stop_token) const {
  auto context_token = CreateStopToken("RenameItem", std::move(stop_token));
  co_return co_await provider_->RenameItem(std::move(item), std::move(new_name),
                                           context_token.GetToken());
}

template <typename ItemT>
Task<> TimingOutCloudProvider::Remove(ItemT item,
                                      stdx::stop_token stop_token) const {
  auto context_token = CreateStopToken("RemoveItem", std::move(stop_token));
  co_return co_await provider_->RemoveItem(std::move(item),
                                           context_token.GetToken());
}

template <typename ItemT>
Task<ItemT> TimingOutCloudProvider::Move(
    ItemT source, AbstractCloudProvider::Directory destination,
    stdx::stop_token stop_token) const {
  auto context_token = CreateStopToken("MoveItem", std::move(stop_token));
  co_return co_await provider_->MoveItem(
      std::move(source), std::move(destination), context_token.GetToken());
}

auto TimingOutCloudProvider::CreateStopToken(std::string action,
                                             stdx::stop_token stop_token) const
    -> ContextStopToken {
  return ContextStopToken(event_loop_, std::move(action), timeout_ms_,
                          std::move(stop_token));
}

}  // namespace coro::cloudstorage::util