// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Single-instance lock, POSIX transport (Linux, macOS): flock() on a lock
// file plus a Unix domain socket, both in a private per-user directory. See
// laufey_single_instance.h.

#include "laufey_single_instance.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

namespace laufey_common {

namespace {

using Clock = std::chrono::steady_clock;

// How long the server gives one client to send its whole message.
constexpr int kServerReadTimeoutMs = 5000;

void SetCloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags >= 0)
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// A stream socket that doesn't leak into child processes (the runtime may
// spawn some) and never raises SIGPIPE.
int NewSocket() {
#ifdef SOCK_CLOEXEC
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
  if (fd < 0)
    return -1;
  SetCloexec(fd);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  return fd;
}

int SendFlags() {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

bool FillAddress(const std::string& path, sockaddr_un* addr) {
  std::memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr->sun_path))
    return false;
  std::memcpy(addr->sun_path, path.c_str(), path.size() + 1);
  return true;
}

int RemainingMs(Clock::time_point deadline) {
  auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - Clock::now())
                  .count();
  return left > 0 ? static_cast<int>(left) : 0;
}

// Reads exactly `len` bytes before `deadline`.
bool ReadFull(int fd, void* buf, size_t len, Clock::time_point deadline) {
  auto* p = static_cast<char*>(buf);
  while (len > 0) {
    pollfd pfd{fd, POLLIN, 0};
    int ms = RemainingMs(deadline);
    if (ms == 0)
      return false;
    int r = poll(&pfd, 1, ms);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      return false;
    ssize_t n = recv(fd, p, len, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// Writes all of `data` before `deadline`.
bool WriteFull(int fd, const void* data, size_t len,
               Clock::time_point deadline) {
  const auto* p = static_cast<const char*>(data);
  while (len > 0) {
    pollfd pfd{fd, POLLOUT, 0};
    int ms = RemainingMs(deadline);
    if (ms == 0)
      return false;
    int r = poll(&pfd, 1, ms);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      return false;
    ssize_t n = send(fd, p, len, SendFlags());
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// The effective uid of the process at the other end of `fd`.
bool PeerUid(int fd, uid_t* uid) {
#if defined(__linux__)
  ucred cred{};
  socklen_t len = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
      len != sizeof(cred))
    return false;
  *uid = cred.uid;
  return true;
#else
  gid_t gid;
  return getpeereid(fd, uid, &gid) == 0;
#endif
}

class PosixServer : public SingleInstanceServer {
 public:
  PosixServer(int lock_fd, int listen_fd, std::string socket_path,
              std::function<void(SecondInstanceMessage)> on_message)
      : lock_fd_(lock_fd),
        listen_fd_(listen_fd),
        socket_path_(std::move(socket_path)),
        on_message_(std::move(on_message)) {
    if (pipe(wake_) == 0) {
      SetCloexec(wake_[0]);
      SetCloexec(wake_[1]);
    } else {
      wake_[0] = wake_[1] = -1;
    }
    thread_ = std::thread([this] { Serve(); });
  }

  ~PosixServer() override {
    Stop();
  }

  void Stop() override {
    if (stopped_)
      return;
    stopped_ = true;
    if (wake_[1] >= 0) {
      char c = 0;
      ssize_t ignored = write(wake_[1], &c, 1);
      (void)ignored;
    }
    if (wake_[1] < 0) {
      // No way to wake the thread: leave it (and its descriptors) behind.
      thread_.detach();
      return;
    }
    if (thread_.joinable())
      thread_.join();
    close(listen_fd_);
    // Still holding the lock, so the socket file is ours to remove.
    unlink(socket_path_.c_str());
    close(lock_fd_);
    for (int fd : wake_) {
      if (fd >= 0)
        close(fd);
    }
  }

 private:
  void Serve() {
    for (;;) {
      pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
      int r = poll(fds, wake_[0] >= 0 ? 2 : 1, -1);
      if (r < 0 && errno == EINTR)
        continue;
      if (r < 0 || (fds[1].revents & POLLIN))
        return;
      if (!(fds[0].revents & POLLIN))
        continue;
      int client = accept(listen_fd_, nullptr, nullptr);
      if (client < 0)
        continue;
      SetCloexec(client);
#ifdef SO_NOSIGPIPE
      int one = 1;
      setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
      HandleClient(client);
      close(client);
    }
  }

  // One message per connection. Only a process running as this user is
  // heard; everything it sends is validated before use.
  void HandleClient(int fd) {
    uid_t uid;
    if (!PeerUid(fd, &uid) || uid != geteuid())
      return;
    auto deadline =
        Clock::now() + std::chrono::milliseconds(kServerReadTimeoutMs);
    unsigned char header[kSingleInstanceHeaderBytes];
    uint32_t length = 0;
    SecondInstanceMessage message;
    bool ok = ReadFull(fd, header, sizeof(header), deadline) &&
              ParseSecondInstanceHeader(header, &length);
    if (ok) {
      std::string payload(length, '\0');
      ok = (length == 0 || ReadFull(fd, &payload[0], length, deadline)) &&
           DecodeSecondInstancePayload(payload, &message, nullptr);
    }
    if (ok)
      on_message_(std::move(message));
    unsigned char answer = ok ? kSingleInstanceAck : kSingleInstanceNak;
    WriteFull(fd, &answer, 1, Clock::now() + std::chrono::seconds(1));
  }

  int lock_fd_;
  int listen_fd_;
  std::string socket_path_;
  std::function<void(SecondInstanceMessage)> on_message_;
  int wake_[2];
  std::thread thread_;
  bool stopped_ = false;
};

// Whether another process holds the lock at `lock_path` right now.
bool LockIsHeld(const std::string& lock_path) {
  int fd = open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return false;
  bool held = flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
  close(fd);  // releases the lock if we took it
  return held;
}

bool TryDirectory(const std::string& dir, const std::string& app_id,
                  SingleInstanceEndpoint* out, std::string* error) {
  if (dir.empty() || dir[0] != '/') {
    *error = "not an absolute directory";
    return false;
  }
  if (!IsPrivateDirectory(dir, error))
    return false;
  std::string base = dir;
  while (base.size() > 1 && base.back() == '/')
    base.pop_back();
  std::string socket_path = base + "/" + SingleInstanceSocketName(app_id);
  sockaddr_un addr;
  if (!FillAddress(socket_path, &addr)) {
    *error = "socket path too long: " + socket_path;
    return false;
  }
  out->dir = base;
  out->socket_path = socket_path;
  out->lock_path = base + "/" + SingleInstanceLockName(app_id);
  return true;
}

}  // namespace

bool IsPrivateDirectory(const std::string& dir, std::string* error) {
  struct stat st;
  if (lstat(dir.c_str(), &st) != 0) {
    *error = dir + ": " + std::strerror(errno);
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    *error = dir + " is not a directory";
    return false;
  }
  if (st.st_uid != geteuid()) {
    *error = dir + " is not owned by this user";
    return false;
  }
  if ((st.st_mode & 077) != 0) {
    *error = dir + " is accessible to other users";
    return false;
  }
  return true;
}

bool ResolveSingleInstanceEndpoint(const std::string& app_id,
                                   SingleInstanceEndpoint* out,
                                   std::string* error) {
  std::string why;
#if defined(__APPLE__)
  // The per-user temporary directory (what $TMPDIR normally points at),
  // asked from the system so a scrubbed environment gets the same answer.
  char buf[PATH_MAX];
  size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof(buf));
  if (n > 0 && n <= sizeof(buf) && TryDirectory(buf, app_id, out, &why))
    return true;
#else
  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  if (runtime_dir && *runtime_dir &&
      TryDirectory(runtime_dir, app_id, out, &why))
    return true;
#endif
  // Fallback: a directory of our own under /tmp, created owner-only. If it
  // already exists it must pass the same checks (someone else may have
  // created it first).
  std::string fallback = "/tmp/laufey-" + std::to_string(geteuid());
  if (mkdir(fallback.c_str(), 0700) != 0 && errno != EEXIST) {
    *error = fallback + ": " + std::strerror(errno);
    return false;
  }
  if (TryDirectory(fallback, app_id, out, &why))
    return true;
  *error = why;
  return false;
}

SingleInstanceAcquire AcquireSingleInstance(
    const SingleInstanceEndpoint& endpoint,
    std::function<void(SecondInstanceMessage)> on_message,
    std::unique_ptr<SingleInstanceServer>* server, std::string* error) {
  int lock_fd = open(endpoint.lock_path.c_str(),
                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_fd < 0) {
    *error = endpoint.lock_path + ": " + std::strerror(errno);
    return SingleInstanceAcquire::kError;
  }
  struct stat st;
  if (fstat(lock_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid()) {
    close(lock_fd);
    *error = endpoint.lock_path + " is not a file owned by this user";
    return SingleInstanceAcquire::kError;
  }
  if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    int err = errno;
    close(lock_fd);
    if (err == EWOULDBLOCK)
      return SingleInstanceAcquire::kSecondary;
    *error = endpoint.lock_path + ": " + std::strerror(err);
    return SingleInstanceAcquire::kError;
  }
  // We hold the lock, so a socket file already there was left by a process
  // that died: replace it.
  unlink(endpoint.socket_path.c_str());
  sockaddr_un addr;
  int fd = NewSocket();
  if (fd < 0 || !FillAddress(endpoint.socket_path, &addr) ||
      bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      chmod(endpoint.socket_path.c_str(), 0600) != 0 || listen(fd, 16) != 0) {
    *error = endpoint.socket_path + ": " + std::strerror(errno);
    if (fd >= 0)
      close(fd);
    unlink(endpoint.socket_path.c_str());
    close(lock_fd);
    return SingleInstanceAcquire::kError;
  }
  server->reset(new PosixServer(lock_fd, fd, endpoint.socket_path,
                                std::move(on_message)));
  return SingleInstanceAcquire::kPrimary;
}

SingleInstanceForward ForwardToPrimaryInstance(
    const SingleInstanceEndpoint& endpoint,
    const SecondInstanceMessage& message, int timeout_ms, std::string* error) {
  std::string encoded;
  if (!EncodeSecondInstanceMessage(message, &encoded)) {
    *error = "the arguments are too large or not valid UTF-8";
    return SingleInstanceForward::kFailed;
  }
  sockaddr_un addr;
  if (!FillAddress(endpoint.socket_path, &addr)) {
    *error = "socket path too long";
    return SingleInstanceForward::kFailed;
  }
  auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  int fd = -1;
  // The primary takes the lock a moment before it listens; retry until it
  // does, or until the lock is gone (it exited).
  for (;;) {
    fd = NewSocket();
    if (fd < 0) {
      *error = std::string("socket: ") + std::strerror(errno);
      return SingleInstanceForward::kFailed;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
      break;
    int err = errno;
    close(fd);
    fd = -1;
    if (err != ENOENT && err != ECONNREFUSED && err != EAGAIN && err != EINTR) {
      *error = endpoint.socket_path + ": " + std::strerror(err);
      return SingleInstanceForward::kFailed;
    }
    if (!LockIsHeld(endpoint.lock_path))
      return SingleInstanceForward::kNoPrimary;
    if (RemainingMs(deadline) == 0) {
      *error = "timed out connecting";
      return SingleInstanceForward::kFailed;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  unsigned char answer = 0;
  bool sent = WriteFull(fd, encoded.data(), encoded.size(), deadline);
  bool answered = sent && ReadFull(fd, &answer, 1, deadline);
  close(fd);
  if (answered && answer == kSingleInstanceAck)
    return SingleInstanceForward::kAcknowledged;
  *error = !sent ? "could not send" : !answered ? "no answer" : "rejected";
  return SingleInstanceForward::kFailed;
}

SecondInstanceMessage CurrentProcessInvocation(int argc, char** argv) {
  SecondInstanceMessage message;
  for (int i = 1; i < argc; ++i)
    message.args.push_back(SanitizeUtf8(argv[i] ? argv[i] : ""));
  std::string cwd(PATH_MAX, '\0');
  while (!getcwd(&cwd[0], cwd.size())) {
    if (errno != ERANGE || cwd.size() > (1u << 20)) {
      cwd.clear();
      break;
    }
    cwd.resize(cwd.size() * 2);
  }
  message.cwd = SanitizeUtf8(cwd.c_str());
  return message;
}

}  // namespace laufey_common
