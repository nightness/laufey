// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The platform-independent half of auth sessions: see
// laufey_auth_session.h.

#include "laufey_auth_session.h"
#include "laufey_single_instance.h"
#include "laufey_ui_tasks.h"

#include <cstring>
#include <utility>

namespace laufey_common {

namespace {

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

// Printable ASCII without spaces: what a URL handed to the OS may contain
// (anything else must be percent-encoded by the caller).
bool IsUrlChar(char c) {
  return c > 0x20 && c < 0x7f;
}

bool StartsWithNoCase(const std::string& s, const char* prefix) {
  size_t n = std::strlen(prefix);
  if (s.size() < n)
    return false;
  for (size_t i = 0; i < n; i++) {
    if (Lower(s[i]) != prefix[i])
      return false;
  }
  return true;
}

// A lowercase LDH host name: labels of 1..63 letters, digits and hyphens,
// not starting or ending with a hyphen, at most 253 characters.
bool IsLdhHost(const std::string& host) {
  if (host.empty() || host.size() > 253)
    return false;
  size_t label = 0;
  char prev = '.';
  for (char c : host) {
    if (c == '.') {
      if (label == 0 || prev == '-')
        return false;
      label = 0;
    } else if ((c >= 'a' && c <= 'z') || IsDigit(c) || c == '-') {
      if (label == 0 && c == '-')
        return false;
      if (++label > 63)
        return false;
    } else {
      return false;
    }
    prev = c;
  }
  return label != 0 && prev != '-';
}

const char* const kReservedSchemes[] = {
    "http", "https", "file", "about", "data", "javascript", "blob", "ws", "wss",
};

std::mutex& SlotMutex() {
  static std::mutex* m = new std::mutex();
  return *m;
}

// The session holding the slot (a weak reference: the OS side owns it).
std::weak_ptr<AuthSession>& SlotSession() {
  static std::weak_ptr<AuthSession>* s = new std::weak_ptr<AuthSession>();
  return *s;
}
const AuthSession* g_slot_owner = nullptr;

void ReleaseSlot(const AuthSession* owner) {
  std::lock_guard<std::mutex> lock(SlotMutex());
  if (g_slot_owner == owner) {
    g_slot_owner = nullptr;
    SlotSession().reset();
  }
}

std::shared_ptr<AuthSession> CurrentSession() {
  std::lock_guard<std::mutex> lock(SlotMutex());
  return SlotSession().lock();
}

}  // namespace

bool ValidateAuthSessionUrl(const char* url, std::string* error) {
  if (!url || !*url) {
    *error = "url is required";
    return false;
  }
  size_t len = std::strlen(url);
  if (len > LAUFEY_AUTH_SESSION_MAX_URL_BYTES) {
    *error = "url is too long";
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    if (!IsUrlChar(url[i])) {
      *error = "url must be printable ASCII without spaces (percent-encode it)";
      return false;
    }
  }
  std::string s(url, len);
  size_t rest;
  if (StartsWithNoCase(s, "https://")) {
    rest = 8;
  } else if (StartsWithNoCase(s, "http://")) {
    rest = 7;
  } else {
    *error = "url must be an http or https URL";
    return false;
  }
  // An authority with a host: something before the first '/', '?' or '#'
  // that is not just userinfo or a port.
  size_t end = s.find_first_of("/?#", rest);
  std::string authority =
      s.substr(rest, end == std::string::npos ? std::string::npos : end - rest);
  size_t at = authority.rfind('@');
  std::string hostport =
      at == std::string::npos ? authority : authority.substr(at + 1);
  if (hostport.empty() || hostport[0] == ':') {
    *error = "url has no host";
    return false;
  }
  return true;
}

bool ParseAuthSessionCallback(const char* callback, AuthSessionCallback* out,
                              std::string* error) {
  *out = AuthSessionCallback();
  if (!callback || !*callback) {
    *error = "callback is required";
    return false;
  }
  std::string s(callback);
  if (StartsWithNoCase(s, "https://")) {
    if (s.size() > LAUFEY_AUTH_SESSION_MAX_URL_BYTES) {
      *error = "callback is too long";
      return false;
    }
    std::string rest = s.substr(8);
    for (char c : rest) {
      if (!IsUrlChar(c)) {
        *error = "callback URL must be printable ASCII without spaces";
        return false;
      }
    }
    if (rest.find_first_of("?#") != std::string::npos) {
      *error = "an https callback has no query or fragment";
      return false;
    }
    size_t slash = rest.find('/');
    std::string host =
        slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    for (char& c : host)
      c = Lower(c);
    if (!IsLdhHost(host)) {
      *error =
          "an https callback needs a host name (letters, digits, '-', '.'; "
          "no port or userinfo)";
      return false;
    }
    out->https = true;
    out->host = std::move(host);
    out->path = std::move(path);
    return true;
  }
  if (s.size() > 64) {
    *error = "callback scheme is too long";
    return false;
  }
  if (!IsAlpha(s[0])) {
    *error =
        "callback must be a URI scheme (letters, digits, '+', '-', '.') "
        "or an https URL";
    return false;
  }
  std::string scheme;
  for (char c : s) {
    if (!(IsAlpha(c) || IsDigit(c) || c == '+' || c == '-' || c == '.')) {
      *error =
          "callback must be a URI scheme (letters, digits, '+', '-', "
          "'.') or an https URL";
      return false;
    }
    scheme.push_back(Lower(c));
  }
  for (const char* reserved : kReservedSchemes) {
    if (scheme == reserved) {
      *error = "callback scheme '" + scheme +
               "' is not allowed (use an https URL or the app's own scheme)";
      return false;
    }
  }
  out->scheme = std::move(scheme);
  return true;
}

// --- AuthSession -------------------------------------------------------------

AuthSession::AuthSession(std::string url, AuthSessionCallback callback,
                         uint32_t flags,
                         laufey_auth_session_result_fn on_result,
                         void* user_data)
    : url_(std::move(url)),
      callback_(std::move(callback)),
      flags_(flags),
      on_result_(on_result),
      user_data_(user_data) {}

AuthSession::~AuthSession() {
  // A session dropped without a result (a bug on the OS side) still answers
  // and frees the slot, so the app is never wedged.
  if (TakeDelivery()) {
    on_result_(user_data_, LAUFEY_AUTH_SESSION_FAILED,
               "the auth session ended without a result");
  }
  ReleaseSlot(this);
}

bool AuthSession::TakeDelivery() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (delivered_)
    return false;
  delivered_ = true;
  return true;
}

void AuthSession::Finish(int32_t status, const std::string& value) {
  ReleaseSlot(this);
  if (TakeDelivery())
    on_result_(user_data_, status, value.c_str());
}

void AuthSession::Cancel(const std::string& message) {
  std::function<void()> cancel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (delivered_)
      return;
    delivered_ = true;
    // A canceller installed after this runs at once (SetCanceller).
    cancel = std::move(canceller_);
    canceller_ = nullptr;
  }
  if (cancel)
    cancel();
  ReleaseSlot(this);
  on_result_(user_data_, LAUFEY_AUTH_SESSION_CANCELLED, message.c_str());
}

void AuthSession::SetCanceller(std::function<void()> cancel) {
  bool run_now = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (delivered_) {
      run_now = true;
    } else {
      canceller_ = cancel;
    }
  }
  if (run_now && cancel)
    cancel();
}

void AuthSession::SetWindowKey(const void* key) {
  std::lock_guard<std::mutex> lock(mutex_);
  window_key_ = key;
}

const void* AuthSession::window_key() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return window_key_;
}

bool AuthSession::delivered() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return delivered_;
}

// --- Entry points ------------------------------------------------------------

#ifndef __APPLE__
uint32_t AuthSessionCapabilities() {
  return 0;
}
#endif

void AuthSessionReport(laufey_auth_session_result_fn on_result, void* user_data,
                       int32_t status, const std::string& value) {
  if (on_result)
    on_result(user_data, status, value.c_str());
}

void AuthSessionReportNotSupported(laufey_auth_session_result_fn on_result,
                                   void* user_data) {
  AuthSessionReport(
      on_result, user_data, LAUFEY_AUTH_SESSION_NOT_SUPPORTED,
      "this platform has no OS auth session; open the system browser and "
      "receive the redirect through a loopback or custom-scheme listener "
      "(RFC 8252)");
}

std::shared_ptr<AuthSession> AuthSessionBegin(
    uint32_t capabilities, const char* url, const char* callback,
    uint32_t flags, laufey_auth_session_result_fn on_result, void* user_data) {
  if (!on_result)
    return nullptr;
  if ((capabilities & LAUFEY_AUTH_SESSION_CAP_SUPPORTED) == 0) {
    AuthSessionReportNotSupported(on_result, user_data);
    return nullptr;
  }
  std::string error;
  if (!ValidateAuthSessionUrl(url, &error)) {
    AuthSessionReport(on_result, user_data, LAUFEY_AUTH_SESSION_INVALID, error);
    return nullptr;
  }
  AuthSessionCallback cb;
  if (!ParseAuthSessionCallback(callback, &cb, &error)) {
    AuthSessionReport(on_result, user_data, LAUFEY_AUTH_SESSION_INVALID, error);
    return nullptr;
  }
  if ((flags & ~LAUFEY_AUTH_SESSION_EPHEMERAL) != 0) {
    AuthSessionReport(on_result, user_data, LAUFEY_AUTH_SESSION_INVALID,
                      "unknown auth session flags");
    return nullptr;
  }
  if (cb.https &&
      (capabilities & LAUFEY_AUTH_SESSION_CAP_HTTPS_CALLBACK) == 0) {
    AuthSessionReport(
        on_result, user_data, LAUFEY_AUTH_SESSION_NOT_SUPPORTED,
        "an https callback needs macOS 14.4 or later; use a custom scheme");
    return nullptr;
  }
  if ((flags & LAUFEY_AUTH_SESSION_EPHEMERAL) != 0 &&
      (capabilities & LAUFEY_AUTH_SESSION_CAP_EPHEMERAL) == 0) {
    AuthSessionReport(on_result, user_data, LAUFEY_AUTH_SESSION_NOT_SUPPORTED,
                      "ephemeral auth sessions are not supported here");
    return nullptr;
  }
  auto session = std::make_shared<AuthSession>(std::string(url), std::move(cb),
                                               flags, on_result, user_data);
  {
    std::lock_guard<std::mutex> lock(SlotMutex());
    if (!g_slot_owner) {
      g_slot_owner = session.get();
      SlotSession() = session;
      return session;
    }
  }
  // Busy. This session never held the slot, so its Finish leaves the slot
  // (and the running session) alone.
  session->Finish(LAUFEY_AUTH_SESSION_BUSY,
                  "an auth session is already in progress");
  return nullptr;
}

void AuthSessionWindowClosing(const void* window_key) {
  if (!window_key)
    return;
  std::shared_ptr<AuthSession> session = CurrentSession();
  if (session && session->window_key() == window_key)
    session->Cancel("the window the auth session was anchored to closed");
}

bool AuthSessionCancelCurrent(const std::string& message) {
  std::shared_ptr<AuthSession> session = CurrentSession();
  if (!session || session->delivered())
    return false;
  session->Cancel(message);
  return true;
}

void UiLoopEnded() {
  // The app is ending: a later launch becomes the primary instead.
  MarkSingleInstanceEnding();
  UiTaskDispatcher::Get().Close();
  AuthSessionCancelCurrent("the app is quitting");
}

bool AuthSessionBusyForTesting() {
  std::lock_guard<std::mutex> lock(SlotMutex());
  return g_slot_owner != nullptr;
}

}  // namespace laufey_common
