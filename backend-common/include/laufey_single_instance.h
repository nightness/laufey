// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Opt-in single-instance lock with argument forwarding. See
// docs/deep-links.md.
//
// With "singleInstance": true in laufey-launch.json (or
// LAUFEY_SINGLE_INSTANCE=1) and an app id, the first process of an app holds
// a per-user, per-app lock and listens on a local IPC endpoint. A later
// launch of the same app finds the lock taken, sends its arguments and
// working directory to the running process, waits for an acknowledgement,
// and exits 0 before it initializes any web engine. The running process
// brings itself to the front and hands the message to the runtime through
// set_second_instance_handler (laufey.h), like Electron's `second-instance`
// event.
//
//   Linux    Unix domain socket in $XDG_RUNTIME_DIR (else /tmp/laufey-<uid>,
//            created 0700 and checked), guarded by flock() on a lock file
//            next to it; the server checks SO_PEERCRED.
//   macOS    the same, in the per-user temporary directory
//            (confstr(_CS_DARWIN_USER_TEMP_DIR)); the server checks
//            getpeereid().
//   Windows  a named pipe \\.\pipe\laufey-si-<hash>-<user SID>-<session>
//            created with FILE_FLAG_FIRST_PIPE_INSTANCE,
//            PIPE_REJECT_REMOTE_CLIENTS and a DACL that grants only the
//            current user; the client checks that the server process runs as
//            the same user.
//
// The message is untrusted input from any process of the same user: it is
// length-prefixed, capped at kSingleInstanceMaxMessageBytes, and parsed
// strictly (exact length, valid UTF-8, no NUL).

#ifndef LAUFEY_SINGLE_INSTANCE_H_
#define LAUFEY_SINGLE_INSTANCE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "laufey.h"

namespace laufey_common {

// --- Message ----------------------------------------------------------------
//
// Wire format (all integers little-endian uint32):
//
//   "LFSI"  magic
//   len     payload length in bytes (<= kSingleInstanceMaxMessageBytes)
//   payload:
//     cwd_len, cwd bytes
//     argc
//     argc x (arg_len, arg bytes)
//
// The payload must be consumed exactly. Every string must be valid UTF-8
// without NUL. The server answers with one byte: kSingleInstanceAck when the
// message was accepted, kSingleInstanceNak when it was rejected, and
// kSingleInstanceEnding when the primary is ending (MarkSingleInstanceEnding):
// it would never deliver the message, so it doesn't take it, and the launch
// becomes the primary once the ending one lets go of the lock.

// Largest payload accepted (1 MiB).
constexpr uint32_t kSingleInstanceMaxMessageBytes = 1024 * 1024;
// Most arguments accepted in one message.
constexpr uint32_t kSingleInstanceMaxArgs = 4096;
// Header size: magic + payload length.
constexpr size_t kSingleInstanceHeaderBytes = 8;
constexpr unsigned char kSingleInstanceAck = 0x06;
constexpr unsigned char kSingleInstanceNak = 0x15;
constexpr unsigned char kSingleInstanceEnding = 0x18;

struct SecondInstanceMessage {
  std::vector<std::string> args;  // argv after the executable, UTF-8
  std::string cwd;                // working directory, UTF-8
};

// Serializes `message` (header + payload). Returns false, leaving `out`
// empty, if the payload would exceed kSingleInstanceMaxMessageBytes, there
// are more than kSingleInstanceMaxArgs arguments, or a string is not valid
// UTF-8 or contains NUL.
bool EncodeSecondInstanceMessage(const SecondInstanceMessage& message,
                                 std::string* out);

// Validates a header and returns the payload length it announces. False on
// a wrong magic or a length above kSingleInstanceMaxMessageBytes.
bool ParseSecondInstanceHeader(const unsigned char* header,
                               uint32_t* payload_length);

// Parses a payload strictly. False (with `error` set) on truncation, trailing
// bytes, too many arguments, invalid UTF-8, or an embedded NUL.
bool DecodeSecondInstancePayload(const std::string& payload,
                                 SecondInstanceMessage* out,
                                 std::string* error);

// Well-formed UTF-8 (RFC 3629: no overlongs, surrogates or code points above
// U+10FFFF).
bool IsValidUtf8(const std::string& text);

// `text` with every invalid UTF-8 sequence replaced by U+FFFD and NUL
// removed. Used on the sending side, where POSIX argv is arbitrary bytes.
std::string SanitizeUtf8(const std::string& text);

// --- Naming -----------------------------------------------------------------

// 16 lowercase hex digits: FNV-1a (64-bit) of the app id. Not a secret; it
// keeps names short (macOS sun_path is 104 bytes) and free of odd characters.
std::string SingleInstanceIdHash(const std::string& app_id);

// "laufey-si-<hash>.sock" and "laufey-si-<hash>.lock" (POSIX).
std::string SingleInstanceSocketName(const std::string& app_id);
std::string SingleInstanceLockName(const std::string& app_id);

// "\\.\pipe\laufey-si-<hash>-<user_sid>-<session_id>" (Windows).
std::string SingleInstancePipeName(const std::string& app_id,
                                   const std::string& user_sid,
                                   uint32_t session_id);

// --- Transport --------------------------------------------------------------

// Where the lock lives. POSIX: the directory holding the socket and the lock
// file; Windows: the pipe name.
struct SingleInstanceEndpoint {
#ifdef _WIN32
  std::string pipe_name;
#else
  std::string dir;
  std::string socket_path;
  std::string lock_path;
#endif
};

// The endpoint for `app_id` for the current user. POSIX: picks and checks
// the directory ($XDG_RUNTIME_DIR / the Darwin user temp dir, else
// /tmp/laufey-<uid> created 0700), requiring it to be a real directory owned
// by this user and closed to group and others. False (with `error` set) if
// no safe directory is available or the socket path would not fit.
bool ResolveSingleInstanceEndpoint(const std::string& app_id,
                                   SingleInstanceEndpoint* out,
                                   std::string* error);

#ifndef _WIN32
// Whether `dir` is a directory (not a symlink) owned by the effective user
// with no group/other permission bits. Exposed for tests.
bool IsPrivateDirectory(const std::string& dir, std::string* error);
#endif

// The running server of the primary instance. Messages are handled one at a
// time on its own thread; `on_message` runs there.
class SingleInstanceServer {
 public:
  virtual ~SingleInstanceServer() = default;
  // Stops the server thread and releases the lock (tests; the backends keep
  // their server until the process exits).
  virtual void Stop() = 0;
};

enum class SingleInstanceAcquire {
  kPrimary,    // this process holds the lock; `server` is running
  kSecondary,  // another process holds it
  kError,      // no lock could be taken or checked; see `error`
};

// Tries to take the lock for `endpoint`. On kPrimary, `*server` is running
// and calls `on_message` for each valid message from a same-user client.
// A stale socket file (left by a process that died) is replaced.
SingleInstanceAcquire AcquireSingleInstance(
    const SingleInstanceEndpoint& endpoint,
    std::function<void(SecondInstanceMessage)> on_message,
    std::unique_ptr<SingleInstanceServer>* server, std::string* error);

enum class SingleInstanceForward {
  kAcknowledged,  // the primary accepted the message
  kNoPrimary,     // nobody holds the lock any more; try to become primary
  kFailed,        // the primary rejected it or did not answer
  kUntrusted,     // the endpoint belongs to another user; don't use the lock
  kEnding,        // the primary is ending and didn't take it; once it lets go
                  // of the lock, become the primary
};

// Sends `message` to the primary instance at `endpoint` and waits for its
// answer, for up to `timeout_ms` in total. Fails without connecting if the
// message can't be encoded (EncodeSecondInstanceMessage). Windows: first
// calls AllowSetForegroundWindow(ASFW_ANY) so the primary may take focus, and
// refuses a server that does not run as the current user.
SingleInstanceForward ForwardToPrimaryInstance(
    const SingleInstanceEndpoint& endpoint,
    const SecondInstanceMessage& message, int timeout_ms, std::string* error);

// Marks this process's primary as ending: from now on its server answers
// every launch kSingleInstanceEnding instead of accepting a message it would
// never deliver, and the launch waits for the lock and becomes the primary
// itself. Called by quit() (MarkQuitting), when the UI loop ends
// (UiLoopEnded), and when the process exits (exit() from any thread, such as
// the runtime's Deno.exit()). Irreversible; any thread.
void MarkSingleInstanceEnding();
bool SingleInstanceEnding();
void ResetSingleInstanceEndingForTesting();

// --- Startup ----------------------------------------------------------------

// This process's arguments after the executable, as UTF-8 (POSIX: `argv`,
// sanitized; Windows: CommandLineToArgvW(GetCommandLineW()), and `argc` /
// `argv` are ignored), and its working directory.
SecondInstanceMessage CurrentProcessInvocation(int argc, char** argv);

// Called first thing in each backend's main(), before any web engine or the
// runtime is initialized. Does nothing unless single-instance mode is on
// (LaunchSingleInstance()). Without an app id (LaunchAppId()) it warns and
// continues unlocked. If another instance holds the lock, forwards this
// invocation to it and returns false with `*exit_code` set (0 once the
// primary acknowledged, 1 otherwise): the caller must return that from
// main() right away. A primary that is ending refuses the message
// (kSingleInstanceEnding); this launch then waits (up to 10 s) for it to let
// go of the lock and becomes the primary. Otherwise returns true and, as the primary, keeps the
// server running for the life of the process; forwarded messages are queued
// until SetSecondInstanceUiHooks is called.
bool SingleInstanceStartup(int argc, char** argv, int* exit_code);

// --- Delivery ---------------------------------------------------------------

// How the primary hands a forwarded message to its UI thread and brings the
// app to the front. `post` must run `task(data)` on the UI thread (the
// backend's PostUiTask); `activate` runs on the UI thread before the handler
// is called and should show/raise the app's window (NULL to skip).
struct SecondInstanceUiHooks {
  void (*post)(void* ctx, void (*task)(void*), void* data) = nullptr;
  void (*activate)(void* ctx) = nullptr;
  void* ctx = nullptr;
};

// Installs the hooks once the backend's UI loop can take tasks, and posts
// every message queued before that. Passing empty hooks (post == NULL) goes
// back to queueing; once that call returns, the old hooks are no longer used,
// so a backend does it before destroying what `ctx` points to.
void SetSecondInstanceUiHooks(const SecondInstanceUiHooks& hooks);

// Backs the C ABI set_second_instance_handler. Messages that reached the UI
// thread while no handler was set (at most LAUFEY_MAX_PENDING_SECOND_INSTANCES,
// oldest dropped) are delivered, in order, on the calling thread. NULL clears
// the handler and re-arms buffering.
void SetSecondInstanceHandler(laufey_second_instance_fn handler,
                              void* user_data);

// Queues `message` for delivery: posts it to the UI thread if the hooks are
// installed, else holds it until they are. Thread-safe. The server calls
// this; exposed for tests.
void QueueSecondInstance(SecondInstanceMessage message);

// Delivers `message` on the current thread: runs the activate hook, then the
// handler, or buffers the message if there is none. Exposed for tests.
void DeliverSecondInstance(const SecondInstanceMessage& message);

#ifdef __APPLE__
// Brings the app to the front for a forwarded launch: unhides it, restores
// the main (else the first visible or minimized) window and activates the
// app. Windows the embedder hid stay hidden. Main thread only.
void ActivateAppMac();

// SetSecondInstanceUiHooks with the main dispatch queue and ActivateAppMac,
// for the backends that run an NSApplication.
void InstallSecondInstanceHooksMac();

// Stops AppKit from turning command-line arguments into open-document
// events (the NSTreatUnknownArgumentsAsOpen default), which would make a
// directly exec'd `app file` reach the runtime twice: in argv and through the
// open-url handler. A safeguard: macOS 15 was not seen doing it. Call before
// [NSApp run].
void DisableArgvOpenEventsMac();
#endif

}  // namespace laufey_common

#endif  // LAUFEY_SINGLE_INSTANCE_H_
