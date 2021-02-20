#ifndef CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H
#define CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H

#include <coro/task.h>
#include <mega.h>

#include <string>

namespace coro::cloudstorage::mega {

class FileSystemAccess : public ::mega::FileSystemAccess {
 public:
  class FileContent {
   public:
    explicit FileContent(int64_t size) : size_(size) {}
    virtual Task<std::string> Read(int64_t offset, int64_t size,
                                   stdx::stop_token) = 0;

    int64_t size() const { return size_; }

   private:
    int64_t size_;
  };

  class NoopWaiter : public ::mega::Waiter {
   public:
    int wait() final { return 0; }
    void notify() final {}
  };

  class AsyncIOContext : public ::mega::AsyncIOContext {
   public:
    explicit AsyncIOContext(::mega::Waiter* waiter) { this->waiter = waiter; }
    ~AsyncIOContext() override { stop_source_.request_stop(); }
    void finish() final { stop_source_.request_stop(); }
    stdx::stop_token stop_token() const { return stop_source_.get_token(); }

   private:
    stdx::stop_source stop_source_;
  };

  class FileAccess : public ::mega::FileAccess {
   public:
    explicit FileAccess(::mega::Waiter* waiter) : ::mega::FileAccess(waiter) {}

    void asyncsysopen(::mega::AsyncIOContext* context) final {
      std::string path(reinterpret_cast<const char*>(context->buffer),
                       context->len);
      context->failed =
          !fopen(&path, context->access & ::mega::AsyncIOContext::ACCESS_READ,
                 context->access & ::mega::AsyncIOContext::ACCESS_WRITE);
      context->retry = retry;
      context->finished = true;
      if (context->userCallback) {
        context->userCallback(context->userData);
      }
    }

    void asyncsysread(::mega::AsyncIOContext* base_context) final {
      auto context = reinterpret_cast<AsyncIOContext*>(base_context);
      if (!context) {
        return;
      }
      Invoke([=]() -> Task<> {
        try {
          auto chunk = co_await content_->Read(context->pos, context->len,
                                               context->stop_token());
          context->failed = false;
          context->retry = false;
          context->finished = true;
          memcpy(base_context->buffer, chunk.data(), chunk.size());
          if (context->userCallback) {
            context->userCallback(context->userData);
          }
        } catch (const std::exception&) {
          context->failed = true;
          context->retry = false;
          context->finished = true;
          if (context->userCallback) {
            context->userCallback(context->userData);
          }
        }
      });
    }

    ::mega::AsyncIOContext* newasynccontext() final {
      return new AsyncIOContext(waiter);
    }

    bool asyncavailable() final { return true; }
    void updatelocalname(std::string* d) final {
      fopen(d, /*read=*/true, /*write=*/
            false);
    }
    bool fopen(std::string* name, bool read, bool write) final {
      localname = *name;
      type = ::mega::FILENODE;
      retry = false;
      content_ = reinterpret_cast<FileContent*>(std::stoll(*name));
      sysstat(&mtime, &size);
      return true;
    }
    bool fwrite(const uint8_t*, unsigned, m_off_t) final { return false; }
    bool sysread(uint8_t* data, unsigned length, m_off_t offset) final {
      // Used for generating fingerprints. I don't care.
      retry = false;
      memset(data, 0, length);
      return true;
    }
    bool sysstat(::mega::m_time_t* time, m_off_t* size) final {
      *time = 0;
      *size = content_->size();
      return true;
    }
    bool sysopen(bool) final { return fopen(&localname, true, false); }
    void sysclose() override {}

   private:
    FileContent* content_ = nullptr;
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
  ::mega::FileAccess* newfileaccess() final { return new FileAccess(&waiter_); }

 private:
  NoopWaiter waiter_;
};

}  // namespace coro::cloudstorage::mega

#endif  // CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H
