// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the single-instance lock (src/single_instance*.cc). No test
// framework: compile this file with src/single_instance.cc,
// src/single_instance_posix.cc (or src/single_instance_win.cc plus
// src/strings_win.cc and advapi32.lib shell32.lib on Windows),
// src/launch_config.cc and src/data_dir.cc, with backend-common/include and
// capi/include on the include path, and run it. CI does this in the `test`
// job and through ctest. Exits non-zero if any expectation fails.
//
// The transport tests take real locks under app ids made unique per run
// (dev.laufey.test.si-<pid>-<n>) and stop their servers again.

#include "laufey_single_instance.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "laufey_backend_common.h"
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace laufey_common;

static int g_failures = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

static int Pid() {
#ifdef _WIN32
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(getpid());
#endif
}

static std::string UniqueAppId() {
  static int n = 0;
  return "dev.laufey.test.si-" + std::to_string(Pid()) + "-" +
         std::to_string(++n);
}

static std::string Payload(const std::string& encoded) {
  return encoded.substr(kSingleInstanceHeaderBytes);
}

static void PutU32(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// --- Framing
// --------------------------------------------------------------------

static void TestRoundTrip() {
  SecondInstanceMessage in;
  in.cwd = "/home/u/some dir";
  in.args = {"acme://open/doc?id=42&x=y", "--flag", "",
             "caf\xC3\xA9 \xF0\x9F\x98\x80", "with space"};
  std::string encoded;
  EXPECT(EncodeSecondInstanceMessage(in, &encoded));
  uint32_t len = 0;
  EXPECT(ParseSecondInstanceHeader(
      reinterpret_cast<const unsigned char*>(encoded.data()), &len));
  EXPECT(len == encoded.size() - kSingleInstanceHeaderBytes);
  SecondInstanceMessage out;
  std::string error;
  EXPECT(DecodeSecondInstancePayload(Payload(encoded), &out, &error));
  EXPECT(out.cwd == in.cwd);
  EXPECT(out.args == in.args);

  // No arguments, empty working directory.
  SecondInstanceMessage empty;
  EXPECT(EncodeSecondInstanceMessage(empty, &encoded));
  EXPECT(encoded.size() == kSingleInstanceHeaderBytes + 8);
  EXPECT(DecodeSecondInstancePayload(Payload(encoded), &out, &error));
  EXPECT(out.args.empty() && out.cwd.empty());
}

static void TestHeader() {
  SecondInstanceMessage m;
  m.args = {"a"};
  std::string encoded;
  EXPECT(EncodeSecondInstanceMessage(m, &encoded));
  uint32_t len = 0;
  std::string bad = encoded;
  bad[0] = 'X';
  EXPECT(!ParseSecondInstanceHeader(
      reinterpret_cast<const unsigned char*>(bad.data()), &len));
  // A length above the cap is refused before any payload is read.
  std::string big = "LFSI";
  PutU32(&big, kSingleInstanceMaxMessageBytes + 1);
  EXPECT(!ParseSecondInstanceHeader(
      reinterpret_cast<const unsigned char*>(big.data()), &len));
  std::string at_cap = "LFSI";
  PutU32(&at_cap, kSingleInstanceMaxMessageBytes);
  EXPECT(ParseSecondInstanceHeader(
      reinterpret_cast<const unsigned char*>(at_cap.data()), &len));
  EXPECT(len == kSingleInstanceMaxMessageBytes);
}

static void TestTruncationAndTrailing() {
  SecondInstanceMessage m;
  m.cwd = "/w";
  m.args = {"one", "two"};
  std::string encoded;
  EXPECT(EncodeSecondInstanceMessage(m, &encoded));
  std::string payload = Payload(encoded);
  SecondInstanceMessage out;
  std::string error;
  // Every proper prefix is rejected.
  for (size_t cut = 0; cut < payload.size(); ++cut) {
    if (DecodeSecondInstancePayload(payload.substr(0, cut), &out, &error)) {
      std::fprintf(stderr, "FAIL: prefix of %zu bytes accepted\n", cut);
      ++g_failures;
    }
  }
  EXPECT(!DecodeSecondInstancePayload(payload + "x", &out, &error));
  EXPECT(error.find("trailing") != std::string::npos);
  // A string length pointing past the end.
  std::string lying;
  PutU32(&lying, 100);
  lying += "/w";
  EXPECT(!DecodeSecondInstancePayload(lying, &out, &error));
  // An argument count far larger than the bytes could hold.
  std::string many;
  PutU32(&many, 0);
  PutU32(&many, 1000);
  EXPECT(!DecodeSecondInstancePayload(many, &out, &error));
  // More than the argument cap, even if the bytes were there.
  std::string over;
  PutU32(&over, 0);
  PutU32(&over, kSingleInstanceMaxArgs + 1);
  for (uint32_t i = 0; i <= kSingleInstanceMaxArgs; ++i)
    PutU32(&over, 0);
  EXPECT(!DecodeSecondInstancePayload(over, &out, &error));
  EXPECT(error == "too many arguments");
}

static void TestUtf8() {
  EXPECT(IsValidUtf8(""));
  EXPECT(IsValidUtf8("plain"));
  EXPECT(IsValidUtf8("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"));
  EXPECT(IsValidUtf8("\xF4\x8F\xBF\xBF"));   // U+10FFFF
  EXPECT(!IsValidUtf8("\xC0\x80"));          // overlong NUL
  EXPECT(!IsValidUtf8("\xE0\x80\xAF"));      // overlong '/'
  EXPECT(!IsValidUtf8("\xED\xA0\x80"));      // surrogate U+D800
  EXPECT(!IsValidUtf8("\xF4\x90\x80\x80"));  // above U+10FFFF
  EXPECT(!IsValidUtf8("\xF5\x80\x80\x80"));
  EXPECT(!IsValidUtf8("\x80"));       // lone continuation
  EXPECT(!IsValidUtf8("\xC3"));       // truncated
  EXPECT(!IsValidUtf8("a\xE2\x82"));  // truncated at end
  EXPECT(!IsValidUtf8("\xFF\xFE"));

  EXPECT(SanitizeUtf8("ok \xC3\xA9") == "ok \xC3\xA9");
  EXPECT(SanitizeUtf8("a\xFF"
                      "b") ==
         "a\xEF\xBF\xBD"
         "b");
  EXPECT(SanitizeUtf8(std::string("a\0b", 3)) == "ab");
  EXPECT(IsValidUtf8(SanitizeUtf8("\xC0\x80\xED\xA0\x80\xC3")));

  // The decoder refuses bad UTF-8 and NUL in arguments and the directory.
  auto payload_with_arg = [](const std::string& arg) {
    std::string p;
    PutU32(&p, 0);
    PutU32(&p, 1);
    PutU32(&p, static_cast<uint32_t>(arg.size()));
    p += arg;
    return p;
  };
  SecondInstanceMessage out;
  std::string error;
  EXPECT(DecodeSecondInstancePayload(payload_with_arg("fine"), &out, &error));
  EXPECT(
      !DecodeSecondInstancePayload(payload_with_arg("bad\xFF"), &out, &error));
  EXPECT(!DecodeSecondInstancePayload(payload_with_arg("\xED\xA0\x80"), &out,
                                      &error));
  EXPECT(!DecodeSecondInstancePayload(payload_with_arg(std::string("a\0", 2)),
                                      &out, &error));
  std::string bad_cwd;
  PutU32(&bad_cwd, 1);
  bad_cwd += "\x80";
  PutU32(&bad_cwd, 0);
  EXPECT(!DecodeSecondInstancePayload(bad_cwd, &out, &error));

  // The encoder refuses them too (the sender sanitizes first).
  SecondInstanceMessage m;
  std::string encoded;
  m.args = {"x\xFF"};
  EXPECT(!EncodeSecondInstanceMessage(m, &encoded));
  EXPECT(encoded.empty());
  m.args = {std::string("x\0y", 3)};
  EXPECT(!EncodeSecondInstanceMessage(m, &encoded));
  m.args.clear();
  m.cwd = "\xC0\x80";
  EXPECT(!EncodeSecondInstanceMessage(m, &encoded));
}

static void TestSizeCaps() {
  SecondInstanceMessage m;
  std::string encoded;
  // Exactly at the cap: cwd_len + argc + one arg length = 12 bytes.
  m.args = {std::string(kSingleInstanceMaxMessageBytes - 12, 'a')};
  EXPECT(EncodeSecondInstanceMessage(m, &encoded));
  EXPECT(encoded.size() ==
         kSingleInstanceHeaderBytes + kSingleInstanceMaxMessageBytes);
  SecondInstanceMessage out;
  std::string error;
  EXPECT(DecodeSecondInstancePayload(Payload(encoded), &out, &error));
  EXPECT(out.args == m.args);
  // One byte over.
  m.args[0].push_back('a');
  EXPECT(!EncodeSecondInstanceMessage(m, &encoded));
  // Too many (empty) arguments.
  m.args.assign(kSingleInstanceMaxArgs + 1, std::string());
  EXPECT(!EncodeSecondInstanceMessage(m, &encoded));
  m.args.assign(kSingleInstanceMaxArgs, std::string());
  EXPECT(EncodeSecondInstanceMessage(m, &encoded));
  EXPECT(DecodeSecondInstancePayload(Payload(encoded), &out, &error));
  EXPECT(out.args.size() == kSingleInstanceMaxArgs);
}

// --- Naming
// ---------------------------------------------------------------------

static void TestNaming() {
  // FNV-1a 64 reference values.
  EXPECT(SingleInstanceIdHash("") == "cbf29ce484222325");
  EXPECT(SingleInstanceIdHash("a") == "af63dc4c8601ec8c");
  std::string h = SingleInstanceIdHash("com.example.notes");
  EXPECT(h.size() == 16);
  EXPECT(h.find_first_not_of("0123456789abcdef") == std::string::npos);
  EXPECT(h != SingleInstanceIdHash("com.example.notes2"));
  EXPECT(SingleInstanceSocketName("com.example.notes") ==
         "laufey-si-" + h + ".sock");
  EXPECT(SingleInstanceLockName("com.example.notes") ==
         "laufey-si-" + h + ".lock");
  EXPECT(
      SingleInstancePipeName("com.example.notes", "S-1-5-21-1-2-3-1001", 2) ==
      "\\\\.\\pipe\\laufey-si-" + h + "-S-1-5-21-1-2-3-1001-2");

  SingleInstanceEndpoint endpoint;
  std::string error;
  EXPECT(ResolveSingleInstanceEndpoint("com.example.notes", &endpoint, &error));
#ifdef _WIN32
  EXPECT(endpoint.pipe_name.rfind("\\\\.\\pipe\\laufey-si-" + h + "-S-1-", 0) ==
         0);
#else
  EXPECT(endpoint.socket_path ==
         endpoint.dir + "/" + SingleInstanceSocketName("com.example.notes"));
  EXPECT(endpoint.lock_path ==
         endpoint.dir + "/" + SingleInstanceLockName("com.example.notes"));
  EXPECT(endpoint.socket_path.size() < sizeof(sockaddr_un{}.sun_path));
  EXPECT(IsPrivateDirectory(endpoint.dir, &error));
#endif
}

// --- Delivery
// -------------------------------------------------------------------

struct Received {
  std::vector<std::vector<std::string>> args;
  std::vector<std::string> cwds;
};

static void Record(void* user_data, const char* const* argv, size_t argc,
                   const char* cwd) {
  auto* r = static_cast<Received*>(user_data);
  std::vector<std::string> args;
  for (size_t i = 0; i < argc; ++i)
    args.push_back(argv[i]);
  // The array is NULL-terminated as well.
  if (argv[argc] != nullptr)
    ++g_failures;
  r->args.push_back(args);
  r->cwds.push_back(cwd);
}

static int g_activations = 0;

static SecondInstanceMessage Msg(const std::string& arg) {
  SecondInstanceMessage m;
  m.args = {arg};
  m.cwd = "/cwd-" + arg;
  return m;
}

static void TestDelivery() {
  // Before the UI hooks exist, messages wait.
  QueueSecondInstance(Msg("early"));
  SecondInstanceUiHooks hooks;
  // Synchronous "UI thread" for the test.
  hooks.post = [](void*, void (*task)(void*), void* data) { task(data); };
  hooks.activate = [](void*) { ++g_activations; };
  EXPECT(g_activations == 0);
  SetSecondInstanceUiHooks(hooks);
  EXPECT(g_activations == 1);  // posted on install, no handler yet

  // Delivered without a handler: buffered.
  QueueSecondInstance(Msg("m1"));
  EXPECT(g_activations == 2);

  Received r;
  SetSecondInstanceHandler(Record, &r);
  EXPECT(r.args.size() == 2);
  EXPECT(r.args.size() == 2 && r.args[0][0] == "early" && r.args[1][0] == "m1");
  EXPECT(r.cwds.size() == 2 && r.cwds[1] == "/cwd-m1");

  // Live delivery.
  QueueSecondInstance(Msg("live"));
  EXPECT(r.args.size() == 3 && r.args[2][0] == "live");
  EXPECT(g_activations == 3);

  // Clearing re-arms buffering; the cap drops the oldest.
  SetSecondInstanceHandler(nullptr, nullptr);
  for (int i = 0; i < LAUFEY_MAX_PENDING_SECOND_INSTANCES + 3; ++i)
    DeliverSecondInstance(Msg("b" + std::to_string(i)));
  Received r2;
  SetSecondInstanceHandler(Record, &r2);
  EXPECT(r2.args.size() == LAUFEY_MAX_PENDING_SECOND_INSTANCES);
  EXPECT(!r2.args.empty() && r2.args[0][0] == "b3");
  EXPECT(!r2.args.empty() &&
         r2.args.back()[0] ==
             "b" + std::to_string(LAUFEY_MAX_PENDING_SECOND_INSTANCES + 2));
  SetSecondInstanceHandler(nullptr, nullptr);
}

// --- Transport
// ------------------------------------------------------------------

struct Inbox {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<SecondInstanceMessage> messages;

  std::function<void(SecondInstanceMessage)> Sink() {
    return [this](SecondInstanceMessage m) {
      std::lock_guard<std::mutex> lock(mutex);
      messages.push_back(std::move(m));
      cv.notify_all();
    };
  }
};

#ifndef _WIN32
// A private scratch directory for endpoints (0700, owned by us).
static std::string ScratchDir() {
  const char* tmp = std::getenv("TMPDIR");
  std::string base = (tmp && *tmp) ? tmp : "/tmp";
  while (base.size() > 1 && base.back() == '/')
    base.pop_back();
  std::string templ = base + "/lfsi-XXXXXX";
  std::vector<char> buf(templ.begin(), templ.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data()))
    return std::string();
  chmod(buf.data(), 0700);
  return buf.data();
}

static SingleInstanceEndpoint EndpointIn(const std::string& dir,
                                         const std::string& app_id) {
  SingleInstanceEndpoint e;
  e.dir = dir;
  e.socket_path = dir + "/" + SingleInstanceSocketName(app_id);
  e.lock_path = dir + "/" + SingleInstanceLockName(app_id);
  return e;
}

// Sends raw bytes to the socket and returns the one-byte answer (or -1).
static int RawExchange(const std::string& path, const std::string& bytes) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  ssize_t sent = write(fd, bytes.data(), bytes.size());
  (void)sent;
  shutdown(fd, SHUT_WR);
  unsigned char c = 0;
  ssize_t n = read(fd, &c, 1);
  close(fd);
  return n == 1 ? c : -1;
}
#else
static SingleInstanceEndpoint EndpointFor(const std::string& app_id) {
  SingleInstanceEndpoint e;
  std::string error;
  ResolveSingleInstanceEndpoint(app_id, &e, &error);
  return e;
}

static int RawExchange(const std::string& pipe_name, const std::string& bytes) {
  std::wstring name = Utf8ToWide(pipe_name);
  HANDLE h = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 50 && h == INVALID_HANDLE_VALUE; ++i) {
    h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      WaitNamedPipeW(name.c_str(), 100);
  }
  if (h == INVALID_HANDLE_VALUE)
    return -1;
  DWORD n = 0;
  WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr);
  unsigned char c = 0;
  BOOL ok = ReadFile(h, &c, 1, &n, nullptr);
  CloseHandle(h);
  return ok && n == 1 ? c : -1;
}
#endif

static std::string EndpointName(const SingleInstanceEndpoint& e) {
#ifdef _WIN32
  return e.pipe_name;
#else
  return e.socket_path;
#endif
}

static void TestTransport() {
  std::string app_id = UniqueAppId();
#ifdef _WIN32
  SingleInstanceEndpoint e = EndpointFor(app_id);
#else
  std::string dir = ScratchDir();
  EXPECT(!dir.empty());
  std::string error;
  EXPECT(IsPrivateDirectory(dir, &error));
  SingleInstanceEndpoint e = EndpointIn(dir, app_id);
#endif
  std::string err;
  SecondInstanceMessage m;
  m.args = {"acme://x?y=1", "caf\xC3\xA9", "", "two words"};
  m.cwd = "C:/some where";

  // Nobody holds the lock: nothing to forward to.
  EXPECT(ForwardToPrimaryInstance(e, m, 2000, &err) ==
         SingleInstanceForward::kNoPrimary);

  Inbox inbox;
  std::unique_ptr<SingleInstanceServer> primary;
  EXPECT(AcquireSingleInstance(e, inbox.Sink(), &primary, &err) ==
         SingleInstanceAcquire::kPrimary);
  EXPECT(primary != nullptr);
#ifndef _WIN32
  struct stat st;
  EXPECT(stat(e.socket_path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
#endif

  // A second taker is told someone else holds it.
  std::unique_ptr<SingleInstanceServer> other;
  EXPECT(AcquireSingleInstance(e, inbox.Sink(), &other, &err) ==
         SingleInstanceAcquire::kSecondary);
  EXPECT(other == nullptr);

  // Forwarding delivers the exact arguments and directory.
  EXPECT(ForwardToPrimaryInstance(e, m, 5000, &err) ==
         SingleInstanceForward::kAcknowledged);
  {
    std::unique_lock<std::mutex> lock(inbox.mutex);
    inbox.cv.wait_for(lock, std::chrono::seconds(5),
                      [&] { return !inbox.messages.empty(); });
    EXPECT(inbox.messages.size() == 1);
    if (!inbox.messages.empty()) {
      EXPECT(inbox.messages[0].args == m.args);
      EXPECT(inbox.messages[0].cwd == m.cwd);
    }
  }

  // Malformed input is answered with a NAK and never delivered; the server
  // keeps going.
  EXPECT(RawExchange(EndpointName(e), "garbage!") == kSingleInstanceNak);
  std::string bad_utf8 = "LFSI";
  std::string payload;
  PutU32(&payload, 0);
  PutU32(&payload, 1);
  PutU32(&payload, 1);
  payload += "\xFF";
  PutU32(&bad_utf8, static_cast<uint32_t>(payload.size()));
  bad_utf8 += payload;
  EXPECT(RawExchange(EndpointName(e), bad_utf8) == kSingleInstanceNak);
  std::string too_big = "LFSI";
  PutU32(&too_big, kSingleInstanceMaxMessageBytes + 1);
  EXPECT(RawExchange(EndpointName(e), too_big) == kSingleInstanceNak);
  EXPECT(ForwardToPrimaryInstance(e, m, 5000, &err) ==
         SingleInstanceForward::kAcknowledged);
  {
    std::unique_lock<std::mutex> lock(inbox.mutex);
    inbox.cv.wait_for(lock, std::chrono::seconds(5),
                      [&] { return inbox.messages.size() >= 2; });
    EXPECT(inbox.messages.size() == 2);
  }

  // Releasing the lock lets the next process become primary.
  primary->Stop();
  primary.reset();
  EXPECT(AcquireSingleInstance(e, inbox.Sink(), &primary, &err) ==
         SingleInstanceAcquire::kPrimary);
  primary.reset();

#ifndef _WIN32
  // A stale socket file (its process died without removing it): the next
  // taker replaces it and serves normally.
  {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, e.socket_path.c_str(), e.socket_path.size() + 1);
    EXPECT(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    close(fd);  // file stays behind, nobody listens
  }
  EXPECT(access(e.socket_path.c_str(), F_OK) == 0);
  // Connecting to it fails and the lock is free: no primary.
  EXPECT(ForwardToPrimaryInstance(e, m, 2000, &err) ==
         SingleInstanceForward::kNoPrimary);
  EXPECT(AcquireSingleInstance(e, inbox.Sink(), &primary, &err) ==
         SingleInstanceAcquire::kPrimary);
  EXPECT(ForwardToPrimaryInstance(e, m, 5000, &err) ==
         SingleInstanceForward::kAcknowledged);
  primary.reset();
  EXPECT(access(e.socket_path.c_str(), F_OK) != 0);  // removed on Stop

  // Directory checks.
  EXPECT(chmod(dir.c_str(), 0755) == 0);
  EXPECT(!IsPrivateDirectory(dir, &error));
  EXPECT(chmod(dir.c_str(), 0700) == 0);
  std::string link = dir + "-link";
  unlink(link.c_str());
  EXPECT(symlink(dir.c_str(), link.c_str()) == 0);
  EXPECT(!IsPrivateDirectory(link, &error));
  unlink(link.c_str());
  EXPECT(!IsPrivateDirectory(dir + "/missing", &error));
  unlink(e.lock_path.c_str());
  rmdir(dir.c_str());
#endif
}

// Two simultaneous first launches end with exactly one primary.
static void TestRace() {
  for (int round = 0; round < 20; ++round) {
    std::string app_id = UniqueAppId();
#ifdef _WIN32
    SingleInstanceEndpoint e = EndpointFor(app_id);
#else
    std::string dir = ScratchDir();
    SingleInstanceEndpoint e = EndpointIn(dir, app_id);
#endif
    std::atomic<int> primaries{0}, secondaries{0}, errors{0};
    std::atomic<bool> go{false};
    std::unique_ptr<SingleInstanceServer> servers[4];
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&, i] {
        while (!go)
          std::this_thread::yield();
        std::string err;
        switch (AcquireSingleInstance(
            e, [](SecondInstanceMessage) {}, &servers[i], &err)) {
          case SingleInstanceAcquire::kPrimary:
            ++primaries;
            break;
          case SingleInstanceAcquire::kSecondary:
            ++secondaries;
            break;
          case SingleInstanceAcquire::kError:
            ++errors;
            break;
        }
      });
    }
    go = true;
    for (auto& t : threads)
      t.join();
    EXPECT(primaries == 1);
    EXPECT(secondaries == 3);
    EXPECT(errors == 0);
    for (auto& s : servers)
      s.reset();
#ifndef _WIN32
    unlink(e.lock_path.c_str());
    rmdir(dir.c_str());
#endif
  }
}

int main() {
  TestRoundTrip();
  TestHeader();
  TestTruncationAndTrailing();
  TestUtf8();
  TestSizeCaps();
  TestNaming();
  TestDelivery();
  TestTransport();
  TestRace();
  if (g_failures) {
    std::fprintf(stderr, "%d expectation(s) failed\n", g_failures);
    return 1;
  }
  std::printf("single_instance_test: all passed\n");
  return 0;
}
