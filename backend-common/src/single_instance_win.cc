// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Single-instance lock, Windows transport: a named pipe whose first instance
// is the lock (FILE_FLAG_FIRST_PIPE_INSTANCE), readable only by the current
// user and only locally. See laufey_single_instance.h.

#include "laufey_single_instance.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include "laufey_backend_common.h"

namespace laufey_common {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kServerReadTimeoutMs = 5000;
constexpr DWORD kPipeBufferBytes = 64 * 1024;

DWORD RemainingMs(Clock::time_point deadline) {
  auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - Clock::now())
                  .count();
  return left > 0 ? static_cast<DWORD>(left) : 0;
}

class Handle {
 public:
  explicit Handle(HANDLE h = nullptr) : h_(h) {}
  ~Handle() {
    Reset();
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const {
    return h_;
  }
  bool valid() const {
    return h_ && h_ != INVALID_HANDLE_VALUE;
  }
  void Reset(HANDLE h = nullptr) {
    if (valid())
      CloseHandle(h_);
    h_ = h;
  }

 private:
  HANDLE h_;
};

// The TOKEN_USER of `token`, in a buffer that owns the SID it points to.
bool TokenUserSid(HANDLE token, std::vector<BYTE>* buf) {
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (size == 0)
    return false;
  buf->resize(size);
  return GetTokenInformation(token, TokenUser, buf->data(), size, &size) != 0;
}

PSID SidOf(std::vector<BYTE>& buf) {
  return reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
}

bool CurrentUserSid(std::vector<BYTE>* buf) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return false;
  bool ok = TokenUserSid(token, buf);
  CloseHandle(token);
  return ok;
}

bool SidString(PSID sid, std::wstring* out) {
  LPWSTR str = nullptr;
  if (!ConvertSidToStringSidW(sid, &str))
    return false;
  *out = str;
  LocalFree(str);
  return true;
}

// Whether the process serving `pipe` runs as the current user.
bool ServerIsCurrentUser(HANDLE pipe) {
  ULONG pid = 0;
  if (!GetNamedPipeServerProcessId(pipe, &pid))
    return false;
  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process.valid())
    return false;
  HANDLE raw = nullptr;
  if (!OpenProcessToken(process.get(), TOKEN_QUERY, &raw))
    return false;
  Handle token(raw);
  std::vector<BYTE> theirs, mine;
  return TokenUserSid(token.get(), &theirs) && CurrentUserSid(&mine) &&
         EqualSid(SidOf(theirs), SidOf(mine));
}

// Runs one overlapped read or write of up to `len` bytes on `h`, waiting
// until `deadline` (or `stop`, if given, is signaled). Returns the number of
// bytes transferred, or -1.
long OverlappedIo(HANDLE h, bool write, void* buf, DWORD len,
                  Clock::time_point deadline, HANDLE stop) {
  OVERLAPPED ov{};
  Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.valid())
    return -1;
  ov.hEvent = event.get();
  DWORD done = 0;
  BOOL ok = write ? WriteFile(h, buf, len, nullptr, &ov)
                  : ReadFile(h, buf, len, nullptr, &ov);
  if (!ok && GetLastError() != ERROR_IO_PENDING)
    return -1;
  HANDLE waits[2] = {event.get(), stop};
  DWORD w =
      WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, RemainingMs(deadline));
  if (w != WAIT_OBJECT_0) {
    CancelIoEx(h, &ov);
    GetOverlappedResult(h, &ov, &done, TRUE);
    return -1;
  }
  if (!GetOverlappedResult(h, &ov, &done, FALSE))
    return -1;
  return static_cast<long>(done);
}

bool TransferFull(HANDLE h, bool write, void* buf, size_t len,
                  Clock::time_point deadline, HANDLE stop) {
  auto* p = static_cast<char*>(buf);
  while (len > 0) {
    DWORD chunk =
        len > kPipeBufferBytes ? kPipeBufferBytes : static_cast<DWORD>(len);
    long n = OverlappedIo(h, write, p, chunk, deadline, stop);
    if (n <= 0)
      return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// A pipe instance readable and writable by the current user only, never
// from the network. `first` makes creation fail if the pipe already exists,
// which is the lock.
HANDLE CreatePipeInstance(const std::wstring& name, bool first, DWORD* error) {
  std::vector<BYTE> sid_buf;
  std::wstring sid;
  if (!CurrentUserSid(&sid_buf) || !SidString(SidOf(sid_buf), &sid)) {
    *error = GetLastError();
    return INVALID_HANDLE_VALUE;
  }
  // Protected DACL with one entry: generic-all for this user.
  std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
    *error = GetLastError();
    return INVALID_HANDLE_VALUE;
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
  DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
  if (first)
    open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
  HANDLE h = CreateNamedPipeW(name.c_str(), open_mode,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                  PIPE_REJECT_REMOTE_CLIENTS,
                              PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes,
                              kPipeBufferBytes, 0, &sa);
  *error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
  LocalFree(sd);
  return h;
}

class PipeServer : public SingleInstanceServer {
 public:
  PipeServer(HANDLE first, std::wstring name,
             std::function<void(SecondInstanceMessage)> on_message)
      : current_(first),
        name_(std::move(name)),
        on_message_(std::move(on_message)),
        stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    thread_ = std::thread([this] { Serve(); });
  }

  ~PipeServer() override {
    Stop();
  }

  void Stop() override {
    if (stopped_)
      return;
    stopped_ = true;
    SetEvent(stop_.get());
    if (thread_.joinable())
      thread_.join();
    if (current_ != INVALID_HANDLE_VALUE)
      CloseHandle(current_);
    current_ = INVALID_HANDLE_VALUE;
  }

 private:
  void Serve() {
    for (;;) {
      bool connected = false;
      OVERLAPPED ov{};
      Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      ov.hEvent = event.get();
      if (ConnectNamedPipe(current_, &ov)) {
        connected = true;
      } else {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
          connected = true;
        } else if (err == ERROR_IO_PENDING) {
          HANDLE waits[2] = {event.get(), stop_.get()};
          DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
          DWORD n = 0;
          if (w != WAIT_OBJECT_0) {
            CancelIoEx(current_, &ov);
            GetOverlappedResult(current_, &ov, &n, TRUE);
            return;
          }
          connected = GetOverlappedResult(current_, &ov, &n, FALSE) != 0;
        }
      }
      if (WaitForSingleObject(stop_.get(), 0) == WAIT_OBJECT_0)
        return;
      // Open the next instance before serving this one, so the pipe name
      // (the lock) never disappears while we run.
      DWORD error = 0;
      HANDLE next = CreatePipeInstance(name_, false, &error);
      if (connected)
        HandleClient(current_);
      DisconnectNamedPipe(current_);
      CloseHandle(current_);
      current_ = next;
      if (current_ == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr,
                     "laufey: single-instance pipe lost (error %lu); later "
                     "launches will start their own instance\n",
                     static_cast<unsigned long>(error));
        return;
      }
    }
  }

  // One message per connection. The DACL admits only this user; everything
  // the client sends is still validated before use.
  void HandleClient(HANDLE pipe) {
    auto deadline =
        Clock::now() + std::chrono::milliseconds(kServerReadTimeoutMs);
    unsigned char header[kSingleInstanceHeaderBytes];
    uint32_t length = 0;
    SecondInstanceMessage message;
    bool ok = TransferFull(pipe, false, header, sizeof(header), deadline,
                           stop_.get()) &&
              ParseSecondInstanceHeader(header, &length);
    if (ok) {
      std::string payload(length, '\0');
      ok = (length == 0 || TransferFull(pipe, false, &payload[0], length,
                                        deadline, stop_.get())) &&
           DecodeSecondInstancePayload(payload, &message, nullptr);
    }
    // An ending primary would never deliver it: refuse, so the launch
    // becomes the primary instead of being acknowledged and lost.
    bool ending = ok && SingleInstanceEnding();
    if (ok && !ending)
      on_message_(std::move(message));
    unsigned char answer = ending ? kSingleInstanceEnding
                           : ok   ? kSingleInstanceAck
                                  : kSingleInstanceNak;
    TransferFull(pipe, true, &answer, 1, Clock::now() + std::chrono::seconds(1),
                 stop_.get());
    FlushFileBuffers(pipe);
  }

  HANDLE current_;
  std::wstring name_;
  std::function<void(SecondInstanceMessage)> on_message_;
  Handle stop_;
  std::thread thread_;
  bool stopped_ = false;
};

}  // namespace

bool ResolveSingleInstanceEndpoint(const std::string& app_id,
                                   SingleInstanceEndpoint* out,
                                   std::string* error) {
  std::vector<BYTE> sid_buf;
  std::wstring sid;
  if (!CurrentUserSid(&sid_buf) || !SidString(SidOf(sid_buf), &sid)) {
    *error = "could not read the current user's SID (error " +
             std::to_string(GetLastError()) + ")";
    return false;
  }
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
    session = 0;
  out->pipe_name = SingleInstancePipeName(app_id, WideToUtf8(sid), session);
  return true;
}

SingleInstanceAcquire AcquireSingleInstance(
    const SingleInstanceEndpoint& endpoint,
    std::function<void(SecondInstanceMessage)> on_message,
    std::unique_ptr<SingleInstanceServer>* server, std::string* error) {
  std::wstring name = Utf8ToWide(endpoint.pipe_name);
  DWORD err = 0;
  HANDLE first = CreatePipeInstance(name, true, &err);
  if (first == INVALID_HANDLE_VALUE) {
    // FILE_FLAG_FIRST_PIPE_INSTANCE fails with access denied when the pipe
    // exists: another instance holds the lock (ForwardToPrimaryInstance
    // checks who).
    if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY)
      return SingleInstanceAcquire::kSecondary;
    *error = "CreateNamedPipe failed (error " + std::to_string(err) + ")";
    return SingleInstanceAcquire::kError;
  }
  server->reset(new PipeServer(first, std::move(name), std::move(on_message)));
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
  // Let the primary bring its window to the front: Windows only allows that
  // to a process the foreground process (usually this one, just launched)
  // permits.
  AllowSetForegroundWindow(ASFW_ANY);
  std::wstring name = Utf8ToWide(endpoint.pipe_name);
  auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  Handle pipe;
  for (;;) {
    // SECURITY_IDENTIFICATION: a server that isn't who we expect can't
    // impersonate us.
    pipe.Reset(CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr));
    if (pipe.valid())
      break;
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND)
      return SingleInstanceForward::kNoPrimary;
    if (err == ERROR_ACCESS_DENIED) {
      *error = "the pipe belongs to another user";
      return SingleInstanceForward::kUntrusted;
    }
    if (err != ERROR_PIPE_BUSY) {
      *error = "could not open the pipe (error " + std::to_string(err) + ")";
      return SingleInstanceForward::kFailed;
    }
    DWORD left = RemainingMs(deadline);
    if (left == 0) {
      *error = "timed out connecting";
      return SingleInstanceForward::kFailed;
    }
    WaitNamedPipeW(name.c_str(), left);
  }
  if (!ServerIsCurrentUser(pipe.get())) {
    *error = "the pipe is served by another user's process";
    return SingleInstanceForward::kUntrusted;
  }
  unsigned char answer = 0;
  bool sent = TransferFull(pipe.get(), true, &encoded[0], encoded.size(),
                           deadline, nullptr);
  bool answered =
      sent && TransferFull(pipe.get(), false, &answer, 1, deadline, nullptr);
  if (answered && answer == kSingleInstanceAck)
    return SingleInstanceForward::kAcknowledged;
  if (answered && answer == kSingleInstanceEnding) {
    *error = "the running instance is quitting";
    return SingleInstanceForward::kEnding;
  }
  *error = !sent ? "could not send" : !answered ? "no answer" : "rejected";
  return SingleInstanceForward::kFailed;
}

SecondInstanceMessage CurrentProcessInvocation(int /*argc*/, char** /*argv*/) {
  SecondInstanceMessage message;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv) {
    for (int i = 1; i < argc; ++i)
      message.args.push_back(SanitizeUtf8(WideToUtf8(argv[i])));
    LocalFree(argv);
  }
  DWORD n = GetCurrentDirectoryW(0, nullptr);
  if (n > 0) {
    std::wstring cwd(n, L'\0');
    DWORD got = GetCurrentDirectoryW(n, &cwd[0]);
    if (got > 0 && got < n) {
      cwd.resize(got);
      message.cwd = SanitizeUtf8(WideToUtf8(cwd));
    }
  }
  return message;
}

}  // namespace laufey_common
