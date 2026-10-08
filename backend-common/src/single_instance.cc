// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Single-instance lock: message framing, naming, startup and delivery. The
// transports are in single_instance_posix.cc / single_instance_win.cc. See
// laufey_single_instance.h and docs/deep-links.md.

#include "laufey_single_instance.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <iostream>
#include <mutex>
#include <utility>

#include "laufey_backend_common.h"
#include "laufey_launch_config.h"

namespace laufey_common {

namespace {

const char kMagic[4] = {'L', 'F', 'S', 'I'};

// How long a second launch waits for the primary to answer before giving up.
constexpr int kForwardTimeoutMs = 10000;
// How long a launch waits for an ending primary to let go of the lock, and
// how often it tries to take it meanwhile.
constexpr int kEndingWaitMs = 10000;
constexpr int kEndingRetryMs = 50;

std::atomic<bool> g_ending{false};

void PutU32(std::string* out, uint32_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
}

uint32_t GetU32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// Length of the well-formed UTF-8 sequence starting at text[i], or 0 if the
// bytes there are not one.
size_t Utf8SequenceLength(const std::string& text, size_t i) {
  const auto byte = [&](size_t k) {
    return static_cast<unsigned char>(text[k]);
  };
  const size_t n = text.size();
  unsigned char c = byte(i);
  if (c < 0x80)
    return 1;
  auto cont = [&](size_t k) { return k < n && (byte(k) & 0xC0) == 0x80; };
  if (c >= 0xC2 && c <= 0xDF)
    return cont(i + 1) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) {
    if (!cont(i + 1) || !cont(i + 2))
      return 0;
    unsigned char c1 = byte(i + 1);
    if (c == 0xE0 && c1 < 0xA0)
      return 0;  // overlong
    if (c == 0xED && c1 > 0x9F)
      return 0;  // UTF-16 surrogate
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3))
      return 0;
    unsigned char c1 = byte(i + 1);
    if (c == 0xF0 && c1 < 0x90)
      return 0;  // overlong
    if (c == 0xF4 && c1 > 0x8F)
      return 0;  // above U+10FFFF
    return 4;
  }
  return 0;
}

bool IsCleanString(const std::string& s) {
  return s.find('\0') == std::string::npos && IsValidUtf8(s);
}

// --- Delivery state
// -----------------------------------------------------------

struct DeliveryState {
  // Held while a message is handed to `hooks.post`, so that once
  // SetSecondInstanceUiHooks returns no post into the old hooks is in flight
  // (a backend clears them before it is destroyed). Taken before `mutex`,
  // never under it.
  std::mutex post_mutex;
  std::mutex mutex;
  SecondInstanceUiHooks hooks;
  bool hooks_set = false;
  // Received before the UI hooks were installed.
  std::vector<SecondInstanceMessage> before_ui;
  laufey_second_instance_fn handler = nullptr;
  void* handler_data = nullptr;
  // Reached the UI thread while no handler was registered.
  std::deque<SecondInstanceMessage> unhandled;
};

// Never destroyed: the server thread may still be delivering while the
// process exits.
DeliveryState& Delivery() {
  static DeliveryState* state = new DeliveryState;
  return *state;
}

void CallHandler(laufey_second_instance_fn handler, void* data,
                 const SecondInstanceMessage& message) {
  std::vector<const char*> argv;
  argv.reserve(message.args.size() + 1);
  for (const std::string& arg : message.args)
    argv.push_back(arg.c_str());
  argv.push_back(nullptr);
  handler(data, argv.data(), message.args.size(), message.cwd.c_str());
}

void DeliverTask(void* data) {
  std::unique_ptr<SecondInstanceMessage> message(
      static_cast<SecondInstanceMessage*>(data));
  DeliverSecondInstance(*message);
}

// Called without the state lock held: `post` may run the task right away.
void Post(const SecondInstanceUiHooks& hooks, SecondInstanceMessage message) {
  hooks.post(hooks.ctx, DeliverTask,
             new SecondInstanceMessage(std::move(message)));
}

// The primary's server. Deliberately leaked: it runs until the process
// exits, and the OS releases the lock then (also after a crash).
SingleInstanceServer* g_process_server = nullptr;

}  // namespace

// --- Message
// -------------------------------------------------------------------

bool IsValidUtf8(const std::string& text) {
  size_t i = 0;
  while (i < text.size()) {
    size_t len = Utf8SequenceLength(text, i);
    if (len == 0)
      return false;
    i += len;
  }
  return true;
}

std::string SanitizeUtf8(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    size_t len = Utf8SequenceLength(text, i);
    if (len == 0) {
      out += "\xEF\xBF\xBD";  // U+FFFD
      ++i;
      continue;
    }
    if (!(len == 1 && text[i] == '\0'))
      out.append(text, i, len);
    i += len;
  }
  return out;
}

bool EncodeSecondInstanceMessage(const SecondInstanceMessage& message,
                                 std::string* out) {
  out->clear();
  if (message.args.size() > kSingleInstanceMaxArgs)
    return false;
  // Sizes are checked before each append, so the payload never grows past
  // the cap (and no length overflows uint32).
  size_t total = 4 + 4;
  if (!IsCleanString(message.cwd))
    return false;
  total += message.cwd.size();
  for (const std::string& arg : message.args) {
    if (!IsCleanString(arg))
      return false;
    total += 4 + arg.size();
    if (total > kSingleInstanceMaxMessageBytes)
      return false;
  }
  if (total > kSingleInstanceMaxMessageBytes)
    return false;
  std::string payload;
  payload.reserve(total);
  PutU32(&payload, static_cast<uint32_t>(message.cwd.size()));
  payload += message.cwd;
  PutU32(&payload, static_cast<uint32_t>(message.args.size()));
  for (const std::string& arg : message.args) {
    PutU32(&payload, static_cast<uint32_t>(arg.size()));
    payload += arg;
  }
  out->assign(kMagic, sizeof(kMagic));
  PutU32(out, static_cast<uint32_t>(payload.size()));
  *out += payload;
  return true;
}

bool ParseSecondInstanceHeader(const unsigned char* header,
                               uint32_t* payload_length) {
  for (size_t i = 0; i < sizeof(kMagic); ++i) {
    if (header[i] != static_cast<unsigned char>(kMagic[i]))
      return false;
  }
  uint32_t len = GetU32(header + 4);
  if (len > kSingleInstanceMaxMessageBytes)
    return false;
  *payload_length = len;
  return true;
}

bool DecodeSecondInstancePayload(const std::string& payload,
                                 SecondInstanceMessage* out,
                                 std::string* error) {
  const auto* p = reinterpret_cast<const unsigned char*>(payload.data());
  const size_t n = payload.size();
  size_t pos = 0;
  auto fail = [&](const char* what) {
    if (error)
      *error = what;
    return false;
  };
  auto read_u32 = [&](uint32_t* v) {
    if (n - pos < 4)
      return false;
    *v = GetU32(p + pos);
    pos += 4;
    return true;
  };
  auto read_string = [&](std::string* s) {
    uint32_t len = 0;
    if (!read_u32(&len) || n - pos < len)
      return false;
    s->assign(payload, pos, len);
    pos += len;
    return true;
  };
  if (n > kSingleInstanceMaxMessageBytes)
    return fail("message too large");
  SecondInstanceMessage message;
  if (!read_string(&message.cwd))
    return fail("truncated working directory");
  if (!IsCleanString(message.cwd))
    return fail("working directory is not valid UTF-8 or contains NUL");
  uint32_t argc = 0;
  if (!read_u32(&argc))
    return fail("truncated argument count");
  if (argc > kSingleInstanceMaxArgs)
    return fail("too many arguments");
  // Each argument needs at least its 4-byte length.
  if (argc > (n - pos) / 4)
    return fail("truncated arguments");
  message.args.reserve(argc);
  for (uint32_t i = 0; i < argc; ++i) {
    std::string arg;
    if (!read_string(&arg))
      return fail("truncated argument");
    if (!IsCleanString(arg))
      return fail("argument is not valid UTF-8 or contains NUL");
    message.args.push_back(std::move(arg));
  }
  if (pos != n)
    return fail("trailing bytes after the last argument");
  *out = std::move(message);
  return true;
}

// --- Naming
// --------------------------------------------------------------------

std::string SingleInstanceIdHash(const std::string& app_id) {
  uint64_t h = 14695981039346656037ULL;  // FNV-1a 64 offset basis
  for (unsigned char c : app_id) {
    h ^= c;
    h *= 1099511628211ULL;  // FNV-1a 64 prime
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(h));
  return buf;
}

std::string SingleInstanceSocketName(const std::string& app_id) {
  return "laufey-si-" + SingleInstanceIdHash(app_id) + ".sock";
}

std::string SingleInstanceLockName(const std::string& app_id) {
  return "laufey-si-" + SingleInstanceIdHash(app_id) + ".lock";
}

std::string SingleInstancePipeName(const std::string& app_id,
                                   const std::string& user_sid,
                                   uint32_t session_id) {
  return "\\\\.\\pipe\\laufey-si-" + SingleInstanceIdHash(app_id) + "-" +
         user_sid + "-" + std::to_string(session_id);
}

// --- Ending
// --------------------------------------------------------------------

void MarkSingleInstanceEnding() {
  g_ending.store(true);
}

bool SingleInstanceEnding() {
  return g_ending.load();
}

void ResetSingleInstanceEndingForTesting() {
  g_ending.store(false);
}

// --- Startup
// -------------------------------------------------------------------

bool SingleInstanceStartup(int argc, char** argv, int* exit_code) {
  if (!LaunchSingleInstance())
    return true;
  std::string app_id = LaunchAppId();
  if (app_id.empty() || !IsSafeAppId(app_id)) {
    std::cerr << "laufey: single-instance mode needs an app id (LAUFEY_APP_ID "
                 "or \"appId\" in laufey-launch.json); running without the "
                 "lock"
              << std::endl;
    return true;
  }
  SingleInstanceEndpoint endpoint;
  std::string error;
  if (!ResolveSingleInstanceEndpoint(app_id, &endpoint, &error)) {
    std::cerr << "laufey: single-instance lock unavailable (" << error
              << "); running without it" << std::endl;
    return true;
  }
  // A primary can disappear between our failed lock attempt and the connect
  // (it exited); then try to become the primary again, a few times. One
  // that is ending refuses the message: keep trying to take the lock until
  // it lets go (bounded by kEndingWaitMs).
  auto ending_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kEndingWaitMs);
  // The primary said it is ending: from then on an exchange that fails (it
  // exited with this connection still unanswered) means "try again" too.
  bool primary_ending = false;
  for (int attempt = 0; attempt < 3;) {
    std::unique_ptr<SingleInstanceServer> server;
    SingleInstanceAcquire result = AcquireSingleInstance(
        endpoint, [](SecondInstanceMessage m) { QueueSecondInstance(m); },
        &server, &error);
    if (result == SingleInstanceAcquire::kPrimary) {
      g_process_server = server.release();
      // exit() from any thread (the runtime's Deno.exit()) ends the process
      // without quit() or the loop's end: refuse launches from then on.
      static std::once_flag at_exit;
      std::call_once(at_exit, [] { std::atexit(MarkSingleInstanceEnding); });
      return true;
    }
    if (result == SingleInstanceAcquire::kError) {
      std::cerr << "laufey: single-instance lock unavailable (" << error
                << "); running without it" << std::endl;
      return true;
    }
    SingleInstanceForward forwarded =
        ForwardToPrimaryInstance(endpoint, CurrentProcessInvocation(argc, argv),
                                 kForwardTimeoutMs, &error);
    if (forwarded == SingleInstanceForward::kAcknowledged) {
      *exit_code = 0;
      return false;
    }
    if (forwarded == SingleInstanceForward::kEnding ||
        (primary_ending && forwarded == SingleInstanceForward::kFailed)) {
      primary_ending = true;
      if (std::chrono::steady_clock::now() >= ending_deadline) {
        std::cerr << "laufey: another instance of " << app_id
                  << " is quitting but still holds the single-instance lock"
                  << std::endl;
        *exit_code = 1;
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kEndingRetryMs));
      continue;  // not an attempt: the primary is on its way out
    }
    if (forwarded == SingleInstanceForward::kUntrusted) {
      std::cerr << "laufey: single-instance lock unavailable (" << error
                << "); running without it" << std::endl;
      return true;
    }
    if (forwarded == SingleInstanceForward::kFailed) {
      std::cerr << "laufey: another instance of " << app_id
                << " is running but did not accept this launch (" << error
                << ")" << std::endl;
      *exit_code = 1;
      return false;
    }
    ++attempt;  // kNoPrimary: it went away; try to take the lock again
  }
  std::cerr << "laufey: could not take or reach the single-instance lock for "
            << app_id << "; running without it" << std::endl;
  return true;
}

// --- Delivery
// ------------------------------------------------------------------

void SetSecondInstanceUiHooks(const SecondInstanceUiHooks& hooks) {
  DeliveryState& state = Delivery();
  std::lock_guard<std::mutex> post_lock(state.post_mutex);
  std::vector<SecondInstanceMessage> queued;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.hooks = hooks;
    state.hooks_set = hooks.post != nullptr;
    if (!state.hooks_set)
      return;
    queued.swap(state.before_ui);
  }
  for (SecondInstanceMessage& message : queued)
    Post(hooks, std::move(message));
}

void QueueSecondInstance(SecondInstanceMessage message) {
  DeliveryState& state = Delivery();
  std::lock_guard<std::mutex> post_lock(state.post_mutex);
  SecondInstanceUiHooks hooks;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.hooks_set) {
      if (state.before_ui.size() >= LAUFEY_MAX_PENDING_SECOND_INSTANCES)
        state.before_ui.erase(state.before_ui.begin());
      state.before_ui.push_back(std::move(message));
      return;
    }
    hooks = state.hooks;
  }
  Post(hooks, std::move(message));
}

void DeliverSecondInstance(const SecondInstanceMessage& message) {
  DeliveryState& state = Delivery();
  laufey_second_instance_fn handler = nullptr;
  void* data = nullptr;
  void (*activate)(void*) = nullptr;
  void* ctx = nullptr;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    activate = state.hooks.activate;
    ctx = state.hooks.ctx;
    handler = state.handler;
    data = state.handler_data;
    if (!handler) {
      // Drop the oldest: the latest launch is the one the user waits on.
      if (state.unhandled.size() >= LAUFEY_MAX_PENDING_SECOND_INSTANCES)
        state.unhandled.pop_front();
      state.unhandled.push_back(message);
    }
  }
  if (activate)
    activate(ctx);
  if (handler)
    CallHandler(handler, data, message);
}

void SetSecondInstanceHandler(laufey_second_instance_fn handler,
                              void* user_data) {
  DeliveryState& state = Delivery();
  std::deque<SecondInstanceMessage> pending;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.handler = handler;
    state.handler_data = user_data;
    if (handler)
      pending.swap(state.unhandled);
  }
  // Outside the lock: the handler is embedder code and may re-enter. As
  // with set_open_url_handler, this runs on the registering thread.
  for (const SecondInstanceMessage& message : pending)
    CallHandler(handler, user_data, message);
}

}  // namespace laufey_common
