#include "mega.h"

#include <coro/http/http_parse.h>
#include <coro/util/make_pointer.h>

namespace coro::cloudstorage {

namespace {

template <typename Type>
Type ToItemImpl(::mega::Node* node) {
  Type type;
  type.name = node->displayname();
  type.id = node->nodehandle;
  if constexpr (std::is_same_v<Type, Mega::File>) {
    type.size = node->size;
  }
  return type;
}

Mega::Item ToItem(::mega::Node* node) {
  if (node->type == ::mega::FILENODE) {
    return ToItemImpl<Mega::File>(node);
  } else {
    return ToItemImpl<Mega::Directory>(node);
  }
}

}  // namespace

Task<std::string> Mega::GetSession(Data& d, UserCredential credentials,
                                   stdx::stop_token stop_token) {
  auto [version, email, salt_ptr, prelogin_error] =
      co_await Do<&::mega::MegaClient::prelogin,
                  &::mega::MegaApp::prelogin_result>(d, stop_token,
                                                     credentials.email.c_str());
  std::string salt = *salt_ptr;
  co_await d.wait_(0, stop_token);
  Check(prelogin_error);
  auto twofactor_ptr =
      credentials.twofactor ? credentials.twofactor->c_str() : nullptr;
  if (version == 1) {
    const int kHashLength = 128;
    ::mega::byte hashed_password[kHashLength];
    Check(d.mega_client.pw_key(credentials.password.c_str(), hashed_password));
    auto [login_error] = co_await Do<kLogin, &::mega::MegaApp::login_result>(
        d, std::move(stop_token), credentials.email.c_str(), hashed_password,
        twofactor_ptr);
    co_await d.wait_(0, stop_token);
    if (login_error != ::mega::API_OK) {
      throw CloudException(CloudException::Type::kUnauthorized);
    }
  } else if (version == 2) {
    auto [login_error] =
        co_await Do<kLoginWithSalt, &::mega::MegaApp::login_result>(
            d, std::move(stop_token), credentials.email.c_str(),
            credentials.password.c_str(), &salt, twofactor_ptr);
    co_await d.wait_(0, stop_token);
    if (login_error != ::mega::API_OK) {
      throw CloudException(CloudException::Type::kUnauthorized);
    }
  } else {
    throw CloudException("Unsupported MEGA login version.");
  }

  const int kHashBufferSize = 128;
  ::mega::byte buffer[kHashBufferSize];
  int length = d.mega_client.dumpsession(buffer, kHashBufferSize);
  co_return std::string(reinterpret_cast<const char*>(buffer), length);
}

Task<> Mega::LogIn() {
  auto stop_token = d_->stop_source.get_token();
  auto [login_error] =
      co_await Do<kSessionLogin, &::mega::MegaApp::login_result>(
          stop_token,
          reinterpret_cast<const ::mega::byte*>(auth_token_.session.c_str()),
          static_cast<int>(auth_token_.session.size()));
  co_await d_->wait_(0, stop_token);
  if (login_error != ::mega::API_OK) {
    throw CloudException(CloudException::Type::kUnauthorized);
  }
  auto [fetch_error] = co_await Do<&::mega::MegaClient::fetchnodes,
                                   &::mega::MegaApp::fetchnodes_result>(
      stop_token, /*nocache=*/false);
  co_await CoCheck(fetch_error, stop_token);
}

Task<> Mega::EnsureLoggedIn(stdx::stop_token stop_token) {
  if (!d_->current_login) {
    d_->current_login = Promise<int>([this]() -> Task<int> {
      co_await LogIn();
      co_return 0;
    });
  }
  co_await d_->current_login->Get(std::move(stop_token));
}

const char* Mega::GetErrorDescription(::mega::error e) {
  if (e <= 0) {
    using namespace ::mega;
    switch (e) {
      case API_OK:
        return "No error";
      case API_EINTERNAL:
        return "Internal error";
      case API_EARGS:
        return "Invalid argument";
      case API_EAGAIN:
        return "Request failed, retrying";
      case API_ERATELIMIT:
        return "Rate limit exceeded";
      case API_EFAILED:
        return "Failed permanently";
      case API_ETOOMANY:
        return "Too many concurrent connections or transfers";
      case API_ERANGE:
        return "Out of range";
      case API_EEXPIRED:
        return "Expired";
      case API_ENOENT:
        return "Not found";
      case API_ECIRCULAR:
        return "Circular linkage detected";
      case API_EACCESS:
        return "Access denied";
      case API_EEXIST:
        return "Already exists";
      case API_EINCOMPLETE:
        return "Incomplete";
      case API_EKEY:
        return "Invalid key/Decryption error";
      case API_ESID:
        return "Bad session ID";
      case API_EBLOCKED:
        return "Blocked";
      case API_EOVERQUOTA:
        return "Over quota";
      case API_ETEMPUNAVAIL:
        return "Temporarily not available";
      case API_ETOOMANYCONNECTIONS:
        return "Connection overflow";
      case API_EWRITE:
        return "Write error";
      case API_EREAD:
        return "Read error";
      case API_EAPPKEY:
        return "Invalid application key";
      case API_ESSL:
        return "SSL verification failed";
      case API_EGOINGOVERQUOTA:
        return "Not enough quota";
      case API_EMFAREQUIRED:
        return "Multi-factor authentication required";
      default:
        return "Unknown error";
    }
  }
  return "HTTP Error";
}

void Mega::Data::OnEvent() {
  if (exec_pending) {
    recursive_exec = true;
    return;
  }
  exec_pending = true;
  mega_client.exec();
  exec_pending = false;
  if (recursive_exec) {
    recursive_exec = false;
    OnEvent();
  }
}

::mega::Node* Mega::GetNode(::mega::handle handle) const {
  auto node = d_->mega_client.nodebyhandle(handle);
  if (!node) {
    throw CloudException(CloudException::Type::kNotFound);
  } else {
    return node;
  }
}

Task<Mega::PageData> Mega::ListDirectoryPage(
    Directory directory, std::optional<std::string>,
    coro::stdx::stop_token stop_token) {
  co_await EnsureLoggedIn(std::move(stop_token));
  PageData result;
  for (auto c : GetNode(directory.id)->children) {
    result.items.emplace_back(ToItem(c));
  }
  co_return result;
}

Task<Mega::Directory> Mega::GetRoot(coro::stdx::stop_token stop_token) {
  co_await EnsureLoggedIn(std::move(stop_token));
  co_return Directory{.id = d_->mega_client.rootnodes[0]};
}

Task<Mega::GeneralData> Mega::GetGeneralData(
    coro::stdx::stop_token stop_token) {
  auto tag = d_->mega_client.nextreqtag();
  d_->mega_client.getaccountdetails(new ::mega::AccountDetails, true, false,
                                    false, false, false, false);
  d_->OnEvent();
  auto semaphore = d_->mega_app.GetSemaphore(tag);
  stdx::stop_callback callback(stop_token, [&] { semaphore->resume(); });
  auto& semaphore_ref = *semaphore;
  co_await semaphore_ref;
  if (stop_token.stop_requested()) {
    throw InterruptedException();
  }
  if (d_->mega_app.last_result.type() == typeid(::mega::error)) {
    co_await d_->wait_(0, stop_token);
    throw CloudException(GetErrorDescription(
        std::any_cast<::mega::error>(d_->mega_app.last_result)));
  }
  const auto& account_details =
      std::any_cast<::mega::AccountDetails>(d_->mega_app.last_result);
  co_return GeneralData{.username = auth_token_.email,
                        .space_used = account_details.storage_used,
                        .space_total = account_details.storage_max};
}

Generator<std::string> Mega::GetFileContent(File file, http::Range range,
                                            coro::stdx::stop_token stop_token) {
  co_await EnsureLoggedIn(stop_token);
  auto node = GetNode(file.id);
  intptr_t tag = d_->mega_client.nextreqtag();
  auto size = range.end.value_or(node->size - 1) - range.start + 1;
  auto data = std::make_shared<ReadData>();
  d_->mega_app.read_data.insert({tag, data});
  auto guard = coro::util::MakePointer(
      this, [tag](Mega* m) { m->d_->mega_app.read_data.erase(tag); });

  stdx::stop_callback callback(stop_token,
                               [data] { data->semaphore.resume(); });
  while (!stop_token.stop_requested() && size > 0) {
    if (data->paused && 2 * data->size < App::kBufferSize) {
      data->paused = false;
      d_->mega_client.pread(node, range.start + data->size, size - data->size,
                            reinterpret_cast<void*>(tag));
      d_->OnEvent();
    }
    if (!data->buffer.empty()) {
      auto chunk = std::move(*data->buffer.begin());
      data->buffer.pop_front();
      data->size -= static_cast<int>(chunk.size());
      size -= chunk.size();
      range.start += chunk.size();
      co_yield chunk;
    } else if (!data->paused) {
      co_await data->semaphore;
      if (data->exception) {
        co_await d_->wait_(0, stop_token);
        std::rethrow_exception(data->exception);
      }
      data->semaphore = Semaphore();
      co_await d_->wait_(0, stop_token);
    }
  }
  if (stop_token.stop_requested()) {
    throw InterruptedException();
  }
}

}  // namespace coro::cloudstorage