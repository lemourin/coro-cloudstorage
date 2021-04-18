#include "mega.h"

#include <coro/cloudstorage/providers/mega/file_system_access.h>
#include <coro/cloudstorage/providers/mega/http_io.h>
#include <coro/http/http_parse.h>
#include <coro/util/raii_utils.h>
#include <mega.h>

#include <random>

#ifdef WIN32
#undef CreateDirectory
#undef CreateFile
#endif

namespace coro::cloudstorage {

namespace {

constexpr auto kLogin = static_cast<void (::mega::MegaClient::*)(
    const char*, const uint8_t*, const char*)>(&::mega::MegaClient::login);
constexpr auto kLoginWithSalt = static_cast<void (::mega::MegaClient::*)(
    const char*, const char*, std::string*, const char*)>(
    &::mega::MegaClient::login2);
constexpr auto kSessionLogin =
    static_cast<void (::mega::MegaClient::*)(const uint8_t*, int)>(
        &::mega::MegaClient::login);
constexpr auto kPutNodes = static_cast<void (::mega::MegaClient::*)(
    ::mega::handle, ::mega::NewNode*, int, const char*)>(
    &::mega::MegaClient::putnodes);

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

Generator<std::string> ToGenerator(std::string string) {
  co_yield std::move(string);
}

const char* GetErrorDescription(::mega::error e) {
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

void Check(::mega::error e) {
  if (e != ::mega::API_OK) {
    throw CloudException(GetErrorDescription(e));
  }
}

}  // namespace

struct Mega::CloudProvider::DoLogIn {
  Task<> operator()();
  Data* d;
  std::string session;
};

struct Mega::CloudProvider::ReadData {
  std::deque<std::string> buffer;
  std::exception_ptr exception;
  Promise<void> semaphore;
  bool paused = true;
  int size = 0;
};

struct Mega::CloudProvider::App : ::mega::MegaApp {
  static constexpr int kBufferSize = 1 << 20;

  void prelogin_result(int version, std::string* email, std::string* salt,
                       ::mega::error e) final {
    SetResult(std::make_tuple(version, *email, *salt, e));
  }

  void login_result(::mega::error e) final { SetResult(e); }

  void fetchnodes_result(::mega::error e) final { SetResult(::mega::error(e)); }

  void account_details(::mega::AccountDetails* details, bool, bool, bool, bool,
                       bool, bool) final {
    SetResult(std::move(*details));
    delete details;
  }

  void account_details(::mega::AccountDetails* details, ::mega::error e) final {
    delete details;
    SetResult(e);
  }

  void setattr_result(::mega::handle handle, ::mega::error e) final {
    SetResult(std::make_tuple(handle, e));
  }

  void putfa_result(::mega::handle, ::mega::fatype, ::mega::error e) final {
    SetResult(e);
  }

  void putfa_result(::mega::handle, ::mega::fatype, const char*) final {
    SetResult(std::monostate());
  }

  void putnodes_result(::mega::error e, ::mega::targettype_t,
                       ::mega::NewNode* nodes) final {
    if (e == ::mega::API_OK) {
      delete[] nodes;
      auto n = client->nodenotify.back();
      n->applykey();
      n->setattr();
      SetResult(n);
    } else {
      SetResult(e);
    }
  }

  void unlink_result(::mega::handle handle, ::mega::error e) final {
    SetResult(std::make_tuple(handle, e));
  }

  void rename_result(::mega::handle handle, ::mega::error e) final {
    SetResult(std::make_tuple(handle, e));
  }

  ::mega::dstime pread_failure(::mega::error e, int retry, void* user_data,
                               ::mega::dstime) final {
    const int kMaxRetryCount = 14;
    std::cerr << "[MEGA] PREAD FAILURE " << GetErrorDescription(e) << " "
              << retry << "\n";
    auto it = read_data.find(reinterpret_cast<intptr_t>(user_data));
    if (it == std::end(read_data)) {
      return ~static_cast<::mega::dstime>(0);
    }
    if (retry >= kMaxRetryCount) {
      it->second->exception =
          std::make_exception_ptr(CloudException(GetErrorDescription(e)));
      it->second->semaphore.SetValue();
      return ~static_cast<::mega::dstime>(0);
    } else {
      ::coro::Invoke(Retry(1 << (retry / 2)));
      return 1 << (retry / 2);
    }
  }

  bool pread_data(uint8_t* data, m_off_t length, m_off_t, m_off_t, m_off_t,
                  void* user_data) final {
    auto it = read_data.find(reinterpret_cast<intptr_t>(user_data));
    if (it == std::end(read_data)) {
      return false;
    }
    it->second->buffer.emplace_back(reinterpret_cast<const char*>(data),
                                    static_cast<size_t>(length));
    it->second->size += static_cast<int>(length);
    if (it->second->size >= kBufferSize) {
      it->second->paused = true;
      it->second->semaphore.SetValue();
      return false;
    }
    it->second->semaphore.SetValue();
    return true;
  }

  void transfer_failed(::mega::Transfer* d, ::mega::error e,
                       ::mega::dstime time) final {
    client->restag = d->tag;
    SetResult(e);
  }

  void notify_retry(::mega::dstime time, ::mega::retryreason_t reason) final {
    ::coro::Invoke(Retry(time, /*abortbackoff=*/false));
  }

  void fa_complete(::mega::handle, ::mega::fatype, const char* data,
                   uint32_t size) final {
    SetResult(std::string(data, size));
  }

  int fa_failed(::mega::handle, ::mega::fatype, int retry_count,
                ::mega::error e) final {
    if (retry_count < 7) {
      SetResult(e);
      return 1;
    } else {
      return 0;
    }
  }

  Task<> Retry(::mega::dstime time, bool abortbackoff = true);

  template <typename T>
  void SetResult(T result) {
    auto it = semaphore.find(client->restag);
    if (it != std::end(semaphore)) {
      last_result = std::move(result);
      it->second->SetValue();
    }
  }

  auto GetSemaphore(int tag) {
    auto result = coro::util::MakePointer(new Promise<void>,
                                          [this, tag](Promise<void>* s) {
                                            semaphore.erase(tag);
                                            delete s;
                                          });
    semaphore.insert({tag, result.get()});
    return result;
  }

  explicit App(Data* d) : d(d) {}

  std::unordered_map<int, Promise<void>*> semaphore;
  std::any last_result;
  std::unordered_map<intptr_t, std::shared_ptr<ReadData>> read_data;
  Data* d;
};

struct Mega::CloudProvider::Data {
  stdx::stop_source stop_source;
  WaitT wait_;
  App mega_app;
  mega::HttpIO http_io;
  mega::FileSystemAccess fs;
  ::mega::MegaClient mega_client;
  bool exec_pending = false;
  bool recursive_exec = false;
  std::optional<SharedPromise<DoLogIn>> current_login;
  std::default_random_engine random_engine;

  void OnEvent();

  template <auto Method, typename... Args>
  Task<std::any> Do(stdx::stop_token stop_token, Args... args) {
    auto tag = mega_client.nextreqtag();
    if constexpr (std::is_same_v<bool,
                                 decltype((mega_client.*Method)(args...))>) {
      if (!(mega_client.*Method)(args...)) {
        throw CloudException("unknown mega error");
      }
    } else if constexpr (std::is_same_v<::mega::error,
                                        decltype(
                                            (mega_client.*Method)(args...))>) {
      if (auto error = (mega_client.*Method)(args...);
          error != ::mega::error::API_OK) {
        throw CloudException(GetErrorDescription(error));
      }
    } else {
      (mega_client.*Method)(args...);
    }
    OnEvent();
    auto semaphore = mega_app.GetSemaphore(tag);
    stdx::stop_callback callback(
        stop_token, [&] { semaphore->SetException(InterruptedException()); });
    auto& semaphore_ref = *semaphore;
    co_await semaphore_ref;
    auto result = std::move(mega_app.last_result);
    co_await wait_(0, std::move(stop_token));
    co_return result;
  }

  Task<std::string> GetSession(UserCredential credentials,
                               stdx::stop_token stop_token);
  Task<> EnsureLoggedIn(std::string session, stdx::stop_token);
  Task<> LogIn(std::string session);

  ::mega::Node* GetNode(::mega::handle handle) {
    auto* node = mega_client.nodebyhandle(handle);
    if (!node) {
      throw CloudException(CloudException::Type::kNotFound);
    } else {
      return node;
    }
  }

  Data(WaitT wait, FetchT fetch, const AuthData& auth_data)
      : stop_source(),
        wait_(std::move(wait)),
        mega_app(this),
        http_io(std::move(fetch), [this] { OnEvent(); }),
        mega_client(&mega_app, /*waiter=*/nullptr, /*http_io=*/&http_io,
                    /*fs=*/&fs, /*db_access=*/nullptr,
                    /*gfx_proc=*/nullptr, auth_data.api_key.c_str(),
                    auth_data.app_name.c_str()),
        random_engine(std::random_device()()) {}

  ~Data() { stop_source.request_stop(); }
};

Task<> Mega::CloudProvider::App::Retry(::mega::dstime time, bool abortbackoff) {
  std::cerr << "[MEGA] RETRYING IN " << time * 100 << "\n";
  co_await d->wait_(100 * time, d->stop_source.get_token());
  if (abortbackoff) {
    d->mega_client.abortbackoff();
  }
  std::cerr << "[MEGA] RETRYING NOW\n";
  d->OnEvent();
}

Task<> Mega::CloudProvider::DoLogIn::operator()() {
  co_await d->LogIn(std::move(session));
}

auto Mega::CloudProvider::CreateDataImpl(WaitT wait, FetchT fetch,
                                         const AuthData& auth_data)
    -> std::unique_ptr<Data, DataDeleter> {
  return std::unique_ptr<Data, DataDeleter>(
      new Data(std::move(wait), std::move(fetch), auth_data));
}

Task<std::string> Mega::CloudProvider::GetSession(Data* data,
                                                  UserCredential credential,
                                                  stdx::stop_token stop_token) {
  co_return co_await data->GetSession(std::move(credential),
                                      std::move(stop_token));
}

void Mega::CloudProvider::DataDeleter::operator()(Data* d) const { delete d; }

template <auto Method, typename... Args>
Task<std::any> Mega::CloudProvider::Do(stdx::stop_token stop_token,
                                       Args&&... args) {
  return d_->Do<Method>(std::move(stop_token), std::forward<Args>(args)...);
}

Task<std::string> Mega::CloudProvider::Data::GetSession(
    UserCredential credentials, stdx::stop_token stop_token) {
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

Task<> Mega::CloudProvider::Data::LogIn(std::string session) {
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

Task<> Mega::CloudProvider::Data::EnsureLoggedIn(std::string session,
                                                 stdx::stop_token stop_token) {
  if (!current_login) {
    current_login =
        SharedPromise(DoLogIn{.d = this, .session = std::move(session)});
  }
  co_await current_login->Get(std::move(stop_token));
}

void Mega::CloudProvider::Data::OnEvent() {
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

Task<> Mega::CloudProvider::SetThumbnail(const File& file,
                                         std::string thumbnail,
                                         stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);
  auto node = d_->GetNode(file.id);
  std::any result = co_await Do<&::mega::MegaClient::putfa>(
      stop_token, node->nodehandle, ::mega::GfxProc::THUMBNAIL,
      node->nodecipher(), new std::string(std::move(thumbnail)),
      /*checkAccess=*/false);
  if (result.type() == typeid(::mega::error)) {
    throw CloudException(
        GetErrorDescription(std::any_cast<::mega::error>(result)));
  }
}

Task<Mega::PageData> Mega::CloudProvider::ListDirectoryPage(
    Directory directory, std::optional<std::string>,
    coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, std::move(stop_token));
  PageData result;
  for (auto c : d_->GetNode(directory.id)->children) {
    result.items.emplace_back(ToItem(c));
  }
  co_return result;
}

Task<Mega::Directory> Mega::CloudProvider::GetRoot(
    coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, std::move(stop_token));
  co_return Directory{{.id = d_->mega_client.rootnodes[0]}};
}

Task<Mega::Item> Mega::CloudProvider::RenameItem(
    Item item, std::string new_name, coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);
  auto node = d_->GetNode(
      std::visit([](const auto& d) { return std::cref(d.id); }, item));
  node->attrs.map['n'] = std::move(new_name);
  std::any result = co_await Do<&::mega::MegaClient::setattr>(
      std::move(stop_token), node, nullptr);
  const auto& [handle, error] = std::move(
      std::any_cast<std::tuple<::mega::handle, ::mega::error>>(result));
  Check(error);
  co_return ToItem(d_->GetNode(handle));
}

Task<Mega::Item> Mega::CloudProvider::MoveItem(
    Item source, Directory destination, coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);
  auto source_node = d_->GetNode(
      std::visit([](const auto& d) { return std::cref(d.id); }, source));
  auto destination_node = d_->GetNode(destination.id);
  std::any result = co_await Do<&::mega::MegaClient::rename>(
      std::move(stop_token), source_node, destination_node,
      ::mega::SYNCDEL_NONE, ::mega::UNDEF);
  const auto& [handle, error] = std::move(
      std::any_cast<std::tuple<::mega::handle, ::mega::error>>(result));
  Check(error);
  co_return ToItem(d_->GetNode(handle));
}

Task<Mega::Directory> Mega::CloudProvider::CreateDirectory(
    Directory parent, std::string name, coro::stdx::stop_token stop_token) {
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
    buf[i] = static_cast<uint8_t>(dist(d_->random_engine));
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
      ToItem(d_->GetNode(std::any_cast<::mega::handle>(result))));
}

Task<> Mega::CloudProvider::RemoveItem(Item item,
                                       coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);

  std::any result = co_await Do<&::mega::MegaClient::unlink>(
      stop_token,
      d_->GetNode(std::visit([](const auto& d) { return d.id; }, item)), false);
  auto [handle, error] = std::move(
      std::any_cast<std::tuple<::mega::handle, ::mega::error>>(result));
  Check(error);
}

Task<Mega::GeneralData> Mega::CloudProvider::GetGeneralData(
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

Generator<std::string> Mega::CloudProvider::GetFileContent(
    File file, http::Range range, coro::stdx::stop_token stop_token) {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);
  auto node = d_->GetNode(file.id);
  intptr_t tag = d_->mega_client.nextreqtag();
  auto size = range.end.value_or(node->size - 1) - range.start + 1;
  auto data = std::make_shared<ReadData>();
  d_->mega_app.read_data.insert({tag, data});
  auto guard = coro::util::MakePointer(this, [tag](Mega::CloudProvider* m) {
    m->d_->mega_app.read_data.erase(tag);
  });

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

auto Mega::CloudProvider::CreateFile(Directory parent, std::string_view name,
                                     FileContent content,
                                     stdx::stop_token stop_token)
    -> Task<File> {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);

  class FileUpload : public ::mega::File {
   public:
    explicit FileUpload(App* app) : app_(app), tag_(app_->client->restag + 1) {}

    void terminated() final {
      app_->client->restag = tag;
      app_->SetResult(::mega::error::API_EINTERNAL);
    }

   private:
    App* app_;
    int tag_;
  } file(&d_->mega_app);

  file.name = name;
  file.h = parent.id;
  file.localname = std::to_string(reinterpret_cast<intptr_t>(&content));
  file.size = content.size;
  std::any result = co_await Do<&::mega::MegaClient::startxfer>(
      stop_token, ::mega::PUT, &file, /*skipdupes=*/false,
      /*startfirst=*/false);
  if (result.type() == typeid(::mega::error)) {
    throw CloudException(
        GetErrorDescription(std::any_cast<::mega::error>(result)));
  }
  auto n = std::any_cast<::mega::Node*>(result);
  ::mega::handle h = n->nodehandle;
  ::mega::Node* ntmp;
  for (ntmp = n;
       ((ntmp->parent != nullptr) && (ntmp->parent->nodehandle != parent.id));
       ntmp = ntmp->parent)
    ;
  if ((ntmp->parent != nullptr) && (ntmp->parent->nodehandle == parent.id)) {
    h = ntmp->nodehandle;
  }
  auto node = d_->GetNode(h);
  auto new_file = ToItemImpl<Mega::File>(node);

  switch (GetFileType(new_file)) {
    case FileType::kImage:
    case FileType::kVideo: {
      try {
        auto thumbnail = co_await(*thumbnail_generator_)(
            this, new_file,
            util::ThumbnailOptions{
                .size = 120, .codec = util::ThumbnailOptions::Codec::JPEG},
            stop_token);
        co_await SetThumbnail(new_file, std::move(thumbnail), stop_token);
      } catch (...) {
      }
      break;
    }
    default:
      break;
  }

  co_return new_file;
}

auto Mega::CloudProvider::GetItemThumbnail(File item, http::Range range,
                                           stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  co_await d_->EnsureLoggedIn(auth_token_.session, stop_token);
  auto node = d_->GetNode(item.id);
  std::any result;
  try {
    result = co_await Do<&::mega::MegaClient::getfa>(
        stop_token, node->nodehandle, &node->fileattrstring, &node->nodekey,
        ::mega::GfxProc::THUMBNAIL, /*cancel=*/false);
  } catch (...) {
    result = ::mega::error::API_ENOENT;
  }
  std::string data;
  if (result.type() == typeid(::mega::error)) {
    switch (GetFileType(item)) {
      case FileType::kImage:
      case FileType::kVideo: {
        try {
          auto thumbnail = co_await(*thumbnail_generator_)(
              this, item,
              util::ThumbnailOptions{
                  .size = 120, .codec = util::ThumbnailOptions::Codec::JPEG},
              stop_token);
          co_await SetThumbnail(item, thumbnail, stop_token);
          data = std::move(thumbnail);
        } catch (...) {
        }
        break;
      }
      default:
        throw CloudException(
            GetErrorDescription(std::any_cast<::mega::error>(result)));
    }
  } else {
    data = std::move(std::any_cast<std::string>(result));
  }
  if (!range.end) {
    range.end = data.length() - 1;
  }
  if (*range.end >= static_cast<int64_t>(data.size())) {
    throw http::HttpException(http::HttpException::kRangeNotSatisfiable);
  }
  Thumbnail thumbnail;
  thumbnail.size = *range.end - range.start + 1;
  thumbnail.data = ToGenerator(std::move(data).substr(
      static_cast<size_t>(range.start),
      static_cast<size_t>(*range.end - range.start + 1)));
  co_return thumbnail;
}

}  // namespace coro::cloudstorage