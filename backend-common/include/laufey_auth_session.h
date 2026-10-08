// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Auth sessions (an OS-run browser sign-in that ends at a callback URL), the
// shared half behind the C ABI's auth_session_capabilities /
// auth_session_start / test_cancel_auth_session (API >= 42). See
// docs/auth-session.md.
//
// This file holds everything that is not an OS call: argument validation,
// the app-wide "one session at a time" slot and the exactly-once result.
// The OS half is AuthSessionStartMac (auth_session_mac.mm,
// ASWebAuthenticationSession). Windows and Linux have no OS auth session
// (RFC 8252: use the system browser), so they report NOT_SUPPORTED.
//
// Nothing here logs: sign-in URLs and callback URLs carry codes and state.

#ifndef LAUFEY_AUTH_SESSION_H_
#define LAUFEY_AUTH_SESSION_H_

#include <laufey.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace laufey_common {

// A validated callback: a custom scheme, or an https host + path.
struct AuthSessionCallback {
  bool https = false;
  std::string scheme;  // lowercase; set when !https
  std::string host;    // lowercase; set when https
  std::string path;    // starts with '/'; set when https
};

// `url`: http or https, an authority, printable ASCII without spaces, at
// most LAUFEY_AUTH_SESSION_MAX_URL_BYTES. On failure `error` says what is
// wrong (never echoing the value).
bool ValidateAuthSessionUrl(const char* url, std::string* error);

// `callback`: a URI scheme (RFC 3986: a letter, then letters, digits, '+',
// '-', '.'; at most 64 characters) other than http, https, file, about,
// data, javascript, blob, ws and wss; or an https URL with a lowercase LDH
// host, no port, userinfo, query or fragment, and a path that starts with
// '/' (default "/").
bool ParseAuthSessionCallback(const char* callback, AuthSessionCallback* out,
                              std::string* error);

// One session in flight. Created by AuthSessionBegin, which also gives it
// the app-wide slot. Thread-safe.
class AuthSession : public std::enable_shared_from_this<AuthSession> {
 public:
  AuthSession(std::string url, AuthSessionCallback callback, uint32_t flags,
              laufey_auth_session_result_fn on_result, void* user_data);
  ~AuthSession();
  AuthSession(const AuthSession&) = delete;
  AuthSession& operator=(const AuthSession&) = delete;

  const std::string& url() const {
    return url_;
  }
  const AuthSessionCallback& callback() const {
    return callback_;
  }
  bool ephemeral() const {
    return (flags_ & LAUFEY_AUTH_SESSION_EPHEMERAL) != 0;
  }

  // The OS reported the outcome: release the slot, then deliver it unless a
  // result was already delivered (Cancel). Later calls are ignored.
  void Finish(int32_t status, const std::string& value);
  // End the session as cancelled now: run the canceller (so the OS sheet
  // goes away), release the slot and deliver CANCELLED with `message`. The
  // OS may or may not report the cancellation afterwards (macOS does not
  // while its consent prompt is up), which Finish then ignores. No-op once
  // a result was delivered.
  void Cancel(const std::string& message);
  // How to dismiss the OS sheet (called at most once, from Cancel, on the
  // thread that cancels). Installed after a Cancel, it runs at once.
  void SetCanceller(std::function<void()> cancel);
  // The native window the sheet is anchored to (NSWindow*), for
  // AuthSessionWindowClosing.
  void SetWindowKey(const void* key);
  const void* window_key() const;

  bool delivered() const;

 private:
  bool TakeDelivery();

  const std::string url_;
  const AuthSessionCallback callback_;
  const uint32_t flags_;
  const laufey_auth_session_result_fn on_result_;
  void* const user_data_;

  mutable std::mutex mutex_;
  bool delivered_ = false;
  std::function<void()> canceller_;
  const void* window_key_ = nullptr;
};

// The LAUFEY_AUTH_SESSION_CAP_* bits of this platform (0 off macOS).
uint32_t AuthSessionCapabilities();

// Validate a request against `capabilities` and take the app-wide slot. On
// any refusal (NULL arguments, invalid url / callback / flags, an https
// callback or the ephemeral flag the platform can't honor, a session in
// progress, capabilities 0) the result is delivered through `on_result`
// synchronously, on the calling thread, and nullptr is returned. A NULL
// `on_result` makes the call a no-op.
std::shared_ptr<AuthSession> AuthSessionBegin(
    uint32_t capabilities, const char* url, const char* callback,
    uint32_t flags, laufey_auth_session_result_fn on_result, void* user_data);

// Deliver a result without a session. No-op for a NULL `on_result`.
void AuthSessionReport(laufey_auth_session_result_fn on_result, void* user_data,
                       int32_t status, const std::string& value);

// The NOT_SUPPORTED answer of a platform without an OS auth session.
void AuthSessionReportNotSupported(laufey_auth_session_result_fn on_result,
                                   void* user_data);

// A native window is closing: the session anchored to it ends CANCELLED.
void AuthSessionWindowClosing(const void* window_key);

// Cancel the running session (test_cancel_auth_session; the backend's event
// loop ending). Returns false when none is running.
bool AuthSessionCancelCurrent(const std::string& message);

// Test-only: whether a session holds the app-wide slot.
bool AuthSessionBusyForTesting();

// The backend's event loop has ended (or the backend runs without one, as a
// headless worker): UiTaskDispatcher::Get().Close() (every UI task that has
// not run is answered with `ran` false) and the running auth session ends
// CANCELLED. Call it on the UI thread, before laufey_runtime_shutdown.
void UiLoopEnded();

#ifdef __APPLE__
// Run the session through ASWebAuthenticationSession. MAIN THREAD ONLY.
// `ns_window` is the NSWindow* (bridged, not retained) to anchor the sheet
// to, or nullptr for the key / main / first visible window. Always ends
// with session->Finish or session->Cancel.
void AuthSessionStartMac(std::shared_ptr<AuthSession> session, void* ns_window);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_AUTH_SESSION_H_
