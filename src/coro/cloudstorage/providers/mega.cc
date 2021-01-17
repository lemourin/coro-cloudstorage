#include "mega.h"

#include <coro/http/http_parse.h>
#include <coro/util/raii_utils.h>

namespace coro::cloudstorage {

namespace {

template <typename Type>
Type ToItemImpl(::mega::Node* node) {
  Type type;
  type.name = node->displayname();
  type.id = node->nodehandle;
  type.timestamp = node->mtime ? node->mtime : node->ctime;
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

Task<std::string> Mega::Data::GetSession(UserCredential credentials,
                                         stdx::stop_token stop_token) {
  auto [version, email, salt, prelogin_error] =
      std::any_cast<std::tuple<int, std::string, std::string, ::mega::error>>(
          co_await Do<&::mega::MegaClient::prelogin>(
              stop_token, credentials.email.c_str()));
  Check(prelogin_error);
  auto twofactor_ptr =
      credentials.twofactor ? credentials.twofactor->c_str() : nullptr;
  if (version == 1) {
    const int kHashLength = 128;
    uint8_t hashed_password[kHashLength];
    Check(mega_client.pw_key(credentials.password.c_str(), hashed_password));
    auto login_error = std::any_cast<::mega::error>(
        co_await Do<kLogin>(std::move(stop_token), credentials.email.c_str(),
                            hashed_password, twofactor_ptr));
    if (login_error != ::mega::API_OK) {
      throw CloudException(CloudException::Type::kUnauthorized);
    }
  } else if (version == 2) {
    auto login_error = std::any_cast<::mega::error>(co_await Do<kLoginWithSalt>(
        std::move(stop_token), credentials.email.c_str(),
        credentials.password.c_str(), &salt, twofactor_ptr));
    if (login_error != ::mega::API_OK) {
      throw CloudException(CloudException::Type::kUnauthorized);
    }
  } else {
    throw CloudException("Unsupported MEGA login version.");
  }

  const int kHashBufferSize = 128;
  uint8_t buffer[kHashBufferSize];
  int length = mega_client.dumpsession(buffer, kHashBufferSize);
  co_return std::string(reinterpret_cast<const char*>(buffer), length);
}

Task<> Mega::Data::LogIn(std::string session) {
  auto stop_token = stop_source.get_token();
  auto login_error = std::any_cast<::mega::error>(co_await Do<kSessionLogin>(
      stop_token, reinterpret_cast<const uint8_t*>(session.c_str()),
      static_cast<int>(session.size())));
  if (login_error != ::mega::API_OK) {
    throw CloudException(CloudException::Type::kUnauthorized);
  }
  auto fetch_error =
      std::any_cast<::mega::error>(co_await Do<&::mega::MegaClient::fetchnodes>(
          stop_token, /*nocache=*/false));
  Check(fetch_error);
}

Task<> Mega::Data::EnsureLoggedIn(std::string session,
                                  stdx::stop_token stop_token) {
  if (!current_login) {
    current_login =
        SharedPromise(Mega::DoLogIn{.d = this, .session = std::move(session)});
  }
  co_await current_login->Get(std::move(stop_token));
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
  co_await d_->EnsureLoggedIn(auth_token_.session, std::move(stop_token));
  PageData result;
  for (auto c : GetNode(directory.id)->children) {
    result.items.emplace_back(ToItem(c));
  }
  co_return result;
}

Task<Mega::Directory> Mega::GetRoot(coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, std::move(stop_token));
  co_return Directory{{.id = d_->mega_client.rootnodes[0]}};
}

Task<Mega::Item> Mega::RenameItem(Item item, std::string new_name,
                                  coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);
  auto node =
      GetNode(std::visit([](const auto& d) { return std::cref(d.id); }, item));
  node->attrs.map['n'] = std::move(new_name);
  std::any result = co_await Do<&::mega::MegaClient::setattr>(
      std::move(stop_token), node, nullptr);
  const auto& [handle, error] = std::move(
      std::any_cast<std::tuple<::mega::handle, ::mega::error>>(result));
  Check(error);
  co_return ToItem(GetNode(handle));
}

Task<Mega::Directory> Mega::CreateDirectory(Directory parent, std::string name,
                                            coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);

  ::mega::NewNode folder;
  folder.source = ::mega::NEW_NODE;
  folder.type = ::mega::FOLDERNODE;
  folder.nodehandle = 0;
  folder.parenthandle = ::mega::UNDEF;

  ::mega::SymmCipher key;
  uint8_t buf[::mega::FOLDERNODEKEYLENGTH];
  std::uniform_int_distribution<uint32_t> dist(0, UINT8_MAX);
  for (int i = 0; i < ::mega::FOLDERNODEKEYLENGTH; i++)
    buf[i] = dist(d_->random_engine);
  folder.nodekey.assign(reinterpret_cast<char*>(buf),
                        ::mega::FOLDERNODEKEYLENGTH);
  key.setkey(buf);

  ::mega::AttrMap attrs;
  attrs.map['n'] = name;
  std::string attr_str;
  attrs.getjson(&attr_str);
  folder.attrstring = new std::string;
  d_->mega_client.makeattr(&key, folder.attrstring, attr_str.c_str());

  std::any result =
      co_await Do<kPutNodes>(stop_token, parent.id, &folder, 1, nullptr);
  if (result.type() == typeid(::mega::error)) {
    throw CloudException(
        GetErrorDescription(std::any_cast<::mega::error>(result)));
  }
  co_return std::get<Directory>(
      ToItem(GetNode(std::any_cast<::mega::handle>(result))));
}

Task<> Mega::RemoveItem(Item item, coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);

  std::any result = co_await Do<&::mega::MegaClient::unlink>(
      stop_token, GetNode(std::visit([](const auto& d) { return d.id; }, item)),
      false);
  auto [handle, error] = std::move(
      std::any_cast<std::tuple<::mega::handle, ::mega::error>>(result));
  Check(error);
}

Task<Mega::GeneralData> Mega::GetGeneralData(
    coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);
  std::any result = co_await Do<&::mega::MegaClient::getaccountdetails>(
      stop_token, new ::mega::AccountDetails, true, false, false, false, false,
      false);
  if (result.type() == typeid(::mega::error)) {
    throw CloudException(
        GetErrorDescription(std::any_cast<::mega::error>(result)));
  }
  const auto& account_details = std::any_cast<::mega::AccountDetails>(result);
  co_return GeneralData{.username = auth_token_.email,
                        .space_used = account_details.storage_used,
                        .space_total = account_details.storage_max};
}

Generator<std::string> Mega::GetFileContent(File file, http::Range range,
                                            coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);
  auto node = GetNode(file.id);
  intptr_t tag = d_->mega_client.nextreqtag();
  auto size = range.end.value_or(node->size - 1) - range.start + 1;
  auto data = std::make_shared<ReadData>();
  d_->mega_app.read_data.insert({tag, data});
  auto guard = coro::util::MakePointer(
      this, [tag](Mega* m) { m->d_->mega_app.read_data.erase(tag); });

  stdx::stop_callback callback(stop_token, [data] {
    data->semaphore.SetException(InterruptedException());
  });
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
      data->semaphore = Promise<void>();
      co_await d_->wait_(0, stop_token);
    }
  }
}

}  // namespace coro::cloudstorage