#ifndef CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H
#define CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H

#include <mega.h>

namespace coro::cloudstorage::mega {

class FileSystemAccess : public ::mega::FileSystemAccess {
 public:
  class FileAccess : public ::mega::FileAccess {
   public:
    explicit FileAccess(FileSystemAccess* fs)
        : ::mega::FileAccess(nullptr), fs_(fs) {}

    bool asyncavailable() final { return false; }
    void updatelocalname(std::string*) final {}
    bool fopen(std::string*, bool read, bool write) final { return false; }
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

  void tmpnamelocal(std::string*) const final {}
  bool getsname(std::string*, std::string*) const final { return false; }
  bool renamelocal(std::string*, std::string*, bool) final { return false; }
  bool copylocal(std::string*, std::string*, ::mega::m_time_t) final {
    return false;
  }
  bool unlinklocal(std::string*) final { return false; }
  bool rmdirlocal(std::string*) final { return false; }
  bool mkdirlocal(std::string*, bool) final { return false; }
  bool setmtimelocal(std::string*, ::mega::m_time_t) final { return false; }
  bool chdirlocal(std::string*) const final { return false; }
  bool getextension(std::string*, char*, int) const final { return false; }
  bool issyncsupported(std::string*, bool*) final { return false; }
  bool expanselocalpath(std::string*, std::string*) final { return false; }
  void addevents(::mega::Waiter*, int) final {}
  void local2path(std::string*, std::string*) const final {}
  void path2local(std::string*, std::string*) const final {}
  size_t lastpartlocal(std::string*) const final { return 0; }
  ::mega::DirAccess* newdiraccess() final { return nullptr; }
  ::mega::FileAccess* newfileaccess() final { return new FileAccess(this); }
};

}  // namespace coro::cloudstorage::mega

#endif  // CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H
