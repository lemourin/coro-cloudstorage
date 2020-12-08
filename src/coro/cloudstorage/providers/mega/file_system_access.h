#ifndef CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H
#define CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H

#include <mega.h>

namespace coro::cloudstorage::mega {

class FileSystemAccess : public ::mega::FileSystemAccess {
 public:
  class FileAccess : public ::mega::FileAccess {
   public:
    FileAccess(FileSystemAccess* fs) : ::mega::FileAccess(nullptr), fs_(fs) {}

    bool asyncavailable() final { return false; }
    void updatelocalname(const ::mega::LocalPath&, bool force) final {}
    bool fopen(::mega::LocalPath&, bool read, bool write,
               ::mega::DirAccess* iterating_dir, bool ignoreAttributes) final {
      return false;
    }
    bool fwrite(const uint8_t*, unsigned, m_off_t) final { return false; }
    bool sysread(uint8_t* data, unsigned length, m_off_t offset) final {
      return false;
    }
    bool sysstat(::mega::m_time_t* time, m_off_t* size) final { return false; }
    bool sysopen(bool) final { return false; }
    void sysclose() override {}

   private:
    FileSystemAccess* fs_;
  };

  void tmpnamelocal(::mega::LocalPath&) const final {}
  bool getsname(const ::mega::LocalPath&, ::mega::LocalPath&) const final {
    return false;
  }
  bool renamelocal(::mega::LocalPath&, ::mega::LocalPath&, bool) final {
    return false;
  }
  bool copylocal(::mega::LocalPath&, ::mega::LocalPath&,
                 ::mega::m_time_t) final {
    return false;
  }
  bool unlinklocal(::mega::LocalPath&) final { return false; }
  bool rmdirlocal(::mega::LocalPath&) final { return false; }
  bool mkdirlocal(::mega::LocalPath&, bool) final { return false; }
  bool setmtimelocal(::mega::LocalPath&, ::mega::m_time_t) final {
    return false;
  }
  bool chdirlocal(::mega::LocalPath&) const final { return false; }
  bool getextension(const ::mega::LocalPath&, std::string&) const final {
    return false;
  }
  bool issyncsupported(::mega::LocalPath&, bool* = nullptr,
                       ::mega::SyncError* = nullptr) final {
    return false;
  }
  bool expanselocalpath(::mega::LocalPath&, ::mega::LocalPath&) final {
    return false;
  }
  void addevents(::mega::Waiter*, int) final {}
  void local2path(const std::string*, std::string*) const final {}
  void path2local(const std::string*, std::string*) const final {}
#if defined(_WIN32)
  void local2path(const std::wstring*, std::string*) const final {}
  void path2local(const std::string*, std::wstring*) const final {}
#endif
  ::mega::DirAccess* newdiraccess() final { return nullptr; }
  bool getlocalfstype(const ::mega::LocalPath& path,
                      ::mega::FileSystemType& type) const final {
    return false;
  }
  bool cwd(::mega::LocalPath& path) const final { return false; }
  std::unique_ptr<::mega::FileAccess> newfileaccess(
      bool follow_symlinks) final {
    return std::make_unique<FileAccess>(this);
  }
};

}  // namespace coro::cloudstorage::mega

#endif  // CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H
