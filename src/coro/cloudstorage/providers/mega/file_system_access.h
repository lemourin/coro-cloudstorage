#ifndef CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H
#define CORO_CLOUDSTORAGE_FILE_SYSTEM_ACCESS_H

#include <coro/cloudstorage/providers/mega.h>
#include <coro/task.h>
#include <mega.h>

#include <string>

namespace coro::cloudstorage::mega {

class FileSystemAccess : public ::mega::FileSystemAccess {
 public:
  using FileContent = coro::cloudstorage::Mega::FileContent;

  class NoopWaiter : public ::mega::Waiter {
   public:
    int wait() final { return 0; }
    void notify() final {}
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

    Task<> DoAsyncRead(::mega::AsyncIOContext* context) {
      try {
        PendingRead read{.offset = context->pos};
        reads_.emplace_back(&read);
        if (reads_.size() > 1) {
          co_await read.semaphore;
        }
        auto guard = coro::util::AtScopeExit([&] {
          reads_.erase(std::find(reads_.begin(), reads_.end(), &read));
          auto it = std::min_element(
              reads_.begin(), reads_.end(),
              [](auto* a, auto* b) { return a->offset < b->offset; });
          if (it != reads_.end()) {
            (*it)->semaphore.SetValue();
          }
        });
        if (last_read_ != context->pos) {
          throw CloudException("out of order read");
        }
        if (!current_it_) {
          current_it_ = co_await content_->data.begin();
        }
        auto chunk =
            co_await http::GetBody(util::Take(*current_it_, context->len));
        last_read_ = context->pos + context->len;
        context->failed = false;
        context->retry = false;
        context->finished = true;
        memcpy(context->buffer, chunk.data(), chunk.size());
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
    }

    void asyncsysread(::mega::AsyncIOContext* context) final {
      Invoke(DoAsyncRead(context));
    }

    ::mega::AsyncIOContext* newasynccontext() final {
      auto context = new ::mega::AsyncIOContext;
      context->waiter = waiter;
      return context;
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
      *size = content_->size;
      return true;
    }
    bool sysopen(bool) final { return fopen(&localname, true, false); }
    void sysclose() override {}

   private:
    struct PendingRead {
      int64_t offset;
      Promise<void> semaphore;
    };

    FileContent* content_ = nullptr;
    int64_t last_read_ = 0;
    std::optional<Generator<std::string>::iterator> current_it_;
    std::vector<PendingRead*> reads_;
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
