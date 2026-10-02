// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Passkeys (WebAuthn ceremonies through the OS platform authenticator), the
// shared half behind the C ABI's passkey_capabilities / passkey_request
// (API >= 37). See docs/passkeys.md.
//
// The wire contract is deliberately the one of `@clerk/electron-passkeys`:
// options go in as WebAuthn JSON with base64url binary fields, and every
// request resolves with one JSON envelope,
//
//   {"ok":true,"credential":{...}}
//   {"ok":false,"error":{"code":"cancelled"|"invalid_rp"|"not_supported"|
//                                "timeout"|"unknown","message":"..."}}
//
// This file holds everything that is not an OS call: the strict options
// parser, base64url, the envelope / clientDataJSON writers, the app-wide
// "one ceremony at a time" slot and the exactly-once completion. The OS
// halves are PasskeyStartMac (passkey_mac.mm) and PasskeyStartWin
// (passkey_win.cc); Linux has no platform API and reports not_supported.
//
// Nothing here logs: challenges, user handles and credentials never reach
// stderr, and error messages name fields, never their values.

#ifndef LAUFEY_PASSKEY_H_
#define LAUFEY_PASSKEY_H_

#include <laufey.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Must live outside any namespace (see laufey_backend_common.h).
#if defined(__APPLE__) && defined(__OBJC__)
@class NSError;
#endif

namespace laufey_common {

// --- Limits -----------------------------------------------------------------

// Largest options document accepted (bytes, before parsing).
constexpr size_t kPasskeyMaxOptionsBytes = LAUFEY_PASSKEY_MAX_OPTIONS_BYTES;
// Entries in excludeCredentials / allowCredentials.
constexpr size_t kPasskeyMaxCredentials = 256;
// Entries in pubKeyCredParams.
constexpr size_t kPasskeyMaxAlgorithms = 64;
// Entries in one descriptor's transports.
constexpr size_t kPasskeyMaxTransports = 16;
// A user handle is 1..64 bytes (WebAuthn L3 5.4.3).
constexpr size_t kPasskeyMaxUserIdBytes = 64;
// A credential id is at most 1023 bytes (WebAuthn L3 6.1).
constexpr size_t kPasskeyMaxCredentialIdBytes = 1023;
// `timeout` is clamped into this range (milliseconds).
constexpr uint32_t kPasskeyMinTimeoutMs = 1000;
constexpr uint32_t kPasskeyMaxTimeoutMs = 600000;
// The timeout used where the OS needs one and the options carry none
// (Windows; matches @clerk/electron-passkeys).
constexpr uint32_t kPasskeyDefaultTimeoutMs = 60000;

// --- Error codes (the @clerk/electron-passkeys set) --------------------------

constexpr const char kPasskeyCancelled[] = "cancelled";
constexpr const char kPasskeyInvalidRp[] = "invalid_rp";
constexpr const char kPasskeyNotSupported[] = "not_supported";
constexpr const char kPasskeyTimeout[] = "timeout";
constexpr const char kPasskeyUnknown[] = "unknown";

// Message of the error a request gets while another one is in progress.
constexpr const char kPasskeyBusyMessage[] =
    "a passkey request is already in progress";

struct PasskeyError {
  std::string code;
  std::string message;
};

// --- Parsed options ---------------------------------------------------------

struct PasskeyCredentialDescriptor {
  std::vector<uint8_t> id;
  // Known WebAuthn transport strings only ("usb", "nfc", "ble", "internal",
  // "hybrid", "smart-card"); unknown ones are dropped.
  std::vector<std::string> transports;
};

// PublicKeyCredentialCreationOptions as @clerk/electron serializes them.
// Optional strings are empty when absent; enumerations are empty when absent
// or not a value this version knows (WebAuthn: unknown values are ignored).
struct PasskeyCreationOptions {
  std::string rp_id;
  std::string rp_name;
  std::vector<uint8_t> user_id;
  std::string user_name;
  std::string user_display_name;
  std::vector<uint8_t> challenge;
  // The challenge re-encoded canonically (for a clientDataJSON we build).
  std::string challenge_b64url;
  // COSE algorithms of the "public-key" pubKeyCredParams entries, in order.
  // Empty when the options list none (the platform picks its defaults).
  std::vector<int32_t> algorithms;
  bool has_timeout = false;
  uint32_t timeout_ms = 0;  // clamped to [kPasskeyMinTimeoutMs, Max]
  std::string authenticator_attachment;  // "platform" | "cross-platform"
  // "discouraged" | "preferred" | "required"; from residentKey, else from
  // requireResidentKey (WebAuthn L3 5.4.4).
  std::string resident_key;
  std::string user_verification;  // "required" | "preferred" | "discouraged"
  std::string attestation;  // "none" | "indirect" | "direct" | "enterprise"
  std::vector<PasskeyCredentialDescriptor> exclude_credentials;
};

// PublicKeyCredentialRequestOptions as @clerk/electron serializes them.
struct PasskeyRequestOptions {
  std::string rp_id;
  std::vector<uint8_t> challenge;
  std::string challenge_b64url;
  bool has_timeout = false;
  uint32_t timeout_ms = 0;
  std::string user_verification;
  std::vector<PasskeyCredentialDescriptor> allow_credentials;
};

// Parse `json` (already checked for size and UTF-8 by PasskeyBegin, but these
// check again so they can be used on their own). On failure fill `error`
// (code invalid_rp for a missing / malformed RP ID, unknown otherwise) and
// return false.
bool ParsePasskeyCreationOptions(const std::string& json,
                                 PasskeyCreationOptions* out,
                                 PasskeyError* error);
bool ParsePasskeyRequestOptions(const std::string& json,
                                PasskeyRequestOptions* out,
                                PasskeyError* error);

// A WebAuthn RP ID this layer passes to the OS: a lowercase ASCII domain
// name (LDH labels of 1..63 characters, at most 253 characters, no leading /
// trailing dot or hyphen) that is not an IPv4 address. Internationalized
// names must be given in their A-label (punycode) form. The OS still decides
// whether the app may use it (macOS: the associated domain).
bool IsValidPasskeyRpId(const std::string& rp_id);

// base64url without padding. Decoding accepts up to two trailing '=' and
// rejects any other character, a length of 1 mod 4, and non-zero trailing
// bits (non-canonical input).
std::string Base64UrlEncode(const uint8_t* data, size_t len);
std::string Base64UrlEncode(const std::vector<uint8_t>& data);
bool Base64UrlDecode(const std::string& in, std::vector<uint8_t>* out);

// --- Results ----------------------------------------------------------------

struct PasskeyRegistrationResult {
  std::vector<uint8_t> credential_id;
  std::vector<uint8_t> client_data_json;
  std::vector<uint8_t> attestation_object;
  // "platform" | "cross-platform"; empty leaves it out (the reader sees null).
  std::string attachment;
  std::vector<std::string> transports;
};

struct PasskeyAssertionResult {
  std::vector<uint8_t> credential_id;
  std::vector<uint8_t> client_data_json;
  std::vector<uint8_t> authenticator_data;
  std::vector<uint8_t> signature;
  // Empty leaves "userHandle" out (non-discoverable credentials).
  std::vector<uint8_t> user_handle;
  std::string attachment;
};

// The envelopes. Strings are made valid UTF-8 first (SanitizeUtf8 of
// laufey_single_instance.h: invalid bytes become U+FFFD, NULs are dropped).
std::string PasskeyRegistrationEnvelope(const PasskeyRegistrationResult& r);
std::string PasskeyAssertionEnvelope(const PasskeyAssertionResult& r);
std::string PasskeyErrorEnvelope(const std::string& code,
                                 const std::string& message);

// The clientDataJSON a client that is not a browser builds for the OS to
// hash (Windows): WebAuthn L3 5.8.1 member order (type, challenge, origin,
// crossOrigin), origin "https://<rp_id>". `type` is "webauthn.create" or
// "webauthn.get". `rp_id` must have passed IsValidPasskeyRpId.
std::string BuildPasskeyClientDataJson(const char* type,
                                       const std::string& challenge_b64url,
                                       const std::string& rp_id);

// --- Ceremonies -------------------------------------------------------------

// One in-flight request. Created by PasskeyBegin, which also gives it the
// app-wide slot: until Finish runs, every other request is refused with
// kPasskeyBusyMessage. Thread-safe.
class PasskeyCeremony : public std::enable_shared_from_this<PasskeyCeremony> {
 public:
  PasskeyCeremony(uint32_t kind, laufey_passkey_result_fn callback,
                  void* user_data);
  ~PasskeyCeremony();
  PasskeyCeremony(const PasskeyCeremony&) = delete;
  PasskeyCeremony& operator=(const PasskeyCeremony&) = delete;

  uint32_t kind() const {
    return kind_;
  }
  bool is_create() const {
    return kind_ == LAUFEY_PASSKEY_CREATE;
  }
  PasskeyCreationOptions& creation() {
    return creation_;
  }
  PasskeyRequestOptions& request() {
    return request_;
  }
  // The options' timeout, if any (clamped).
  bool has_timeout() const;
  uint32_t timeout_ms() const;

  // The OS operation has ended (or never started): release the slot, then
  // deliver `envelope` unless a result was already delivered (Abort). Call
  // exactly once; later calls are ignored.
  void Finish(const std::string& envelope);
  // Deliver an error now, without waiting for the OS: run the canceller so
  // the OS operation ends, and keep the slot until Finish. No-op once a
  // result was delivered.
  void Abort(const char* code, const std::string& message);
  // How to cancel the OS operation (called at most once, from Abort, on
  // whatever thread aborts). Installed after an Abort, it runs at once.
  void SetCanceller(std::function<void()> cancel);
  // The native window the ceremony is anchored to (NSWindow* / HWND), for
  // PasskeyWindowClosing.
  void SetWindowKey(const void* key);
  const void* window_key() const;
  // Abort with `timeout` after `ms` unless a result was delivered first. One
  // short-lived thread per call; it holds a reference until it wakes.
  void StartTimeout(uint32_t ms);

  bool delivered() const;
  bool finished() const;

 private:
  void Deliver(const std::string& envelope);

  const uint32_t kind_;
  const laufey_passkey_result_fn callback_;
  void* const user_data_;
  PasskeyCreationOptions creation_;
  PasskeyRequestOptions request_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool delivered_ = false;  // the callback ran (or is running)
  bool finished_ = false;   // the OS operation ended; slot released
  bool aborted_ = false;
  bool cancel_ran_ = false;
  std::function<void()> canceller_;
  const void* window_key_ = nullptr;
};

// Validate a request and take the app-wide slot. On any failure (NULL or
// oversized options, not UTF-8, unknown kind, malformed options, another
// request in progress) the error envelope is delivered through `callback`
// synchronously, on the calling thread, and nullptr is returned. A NULL
// `callback` makes the call a no-op (returns nullptr).
std::shared_ptr<PasskeyCeremony> PasskeyBegin(uint32_t kind,
                                              const char* options_json,
                                              laufey_passkey_result_fn callback,
                                              void* user_data);

// Deliver an error envelope without a ceremony (e.g. not_supported on a
// platform without a passkey API). No-op for a NULL callback.
void PasskeyReportError(laufey_passkey_result_fn callback, void* user_data,
                        const char* code, const std::string& message);

// The not_supported answer of a backend / platform without passkeys.
void PasskeyReportNotSupported(laufey_passkey_result_fn callback,
                               void* user_data);

// A native window is closing: if the request in progress is anchored to it,
// abort it with `cancelled`. Backends call this from their window teardown.
void PasskeyWindowClosing(const void* window_key);

// Test-only: whether a ceremony holds the app-wide slot.
bool PasskeyBusyForTesting();

#ifdef __APPLE__
// LAUFEY_PASSKEY_* capability flags: both on macOS 12+ (the platform and
// security-key providers share the OS sheet), 0 before.
uint32_t PasskeyCapabilitiesMac();
// Run the ceremony through ASAuthorizationController. MAIN THREAD ONLY.
// `ns_window` is the NSWindow* (bridged, not retained) to anchor the sheet
// to, or nullptr for the app's key / main / first visible window. Always
// ends with ceremony->Finish.
void PasskeyStartMac(std::shared_ptr<PasskeyCeremony> ceremony,
                     void* ns_window);
#ifdef __OBJC__
// How an ASAuthorizationController error becomes an envelope error:
// Canceled -> cancelled; a failure whose text says the app is not associated
// with the domain -> invalid_rp; NotHandled / NotInteractive ->
// not_supported; a timeout -> timeout; anything else -> unknown. The message
// is the error's localizedDescription. Exposed for tests.
PasskeyError MapAuthorizationErrorMac(NSError* error);
#endif
#endif

#ifdef _WIN32
// LAUFEY_PASSKEY_* capability flags from webauthn.dll: SECURITY_KEYS when the
// API is present, PLATFORM_AUTHENTICATOR when Windows Hello is available.
uint32_t PasskeyCapabilitiesWin();
// Run the ceremony through webauthn.dll on a worker thread (the calls block
// while the OS dialog is up). Any thread. `hwnd` (HWND) owns the dialog; if
// NULL, the foreground window when it belongs to this process, else the
// first visible top-level window of the process. Always ends
// with ceremony->Finish.
void PasskeyStartWin(std::shared_ptr<PasskeyCeremony> ceremony, void* hwnd);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_PASSKEY_H_
