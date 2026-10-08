// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The secure store (API 47): a small secret per (service, account) in the
// OS's secret store. On macOS (secret_store_mac.mm) the Keychain through
// Security.framework in this process: the data-protection keychain when the
// app is signed with a keychain access group, otherwise the login keychain
// with an access list that trusts only this app's host executable, so
// another program of the user gets macOS's prompt, never the secret silently
// (code injected into this app aside; unsigned or ad-hoc builds with
// byte-identical hosts are one program to macOS). Another program can write
// a login-keychain item without a prompt; see secret_store_mac.mm for what
// the store makes of that. Below: Linux.
//
// On Linux: a small secret per (service, account)
// in the Secret Service (org.freedesktop.secrets: gnome-keyring, KWallet's
// Secret Service, KeePassXC), through libsecret (dlopen()ed: no build or
// package dependency beyond libsecret-1.so.0, which every desktop ships)
// for the secret itself, and plain D-Bus reads that never prompt for the
// state around it:
//
//   - no provider: refused at once with what to do ("install gnome-keyring",
//     or, where KWallet runs without serving the Secret Service, "enable
//     KWallet's Secret Service"); never a plaintext fallback;
//   - locked: the items (or the default collection, for a write) are looked
//     up with SearchItems / the Locked property first. Where no one can
//     answer an unlock prompt (no graphical session, no prompter) the call
//     is refused at once; otherwise the Secret Service is asked to unlock
//     (its own prompt, on a thread of its own) and the call waits up to its
//     timeout: an unlock nobody answers is "unavailable", never a hang and
//     never "not found". The prompt is never dismissed from here (gnome-
//     keyring aborts when its unlock prompt is dismissed while it is up): it
//     stays up for the person to answer, and a later answer only unlocks;
//     nothing is written after the call gave up. One unlock is in flight at
//     a time;
//   - no default keyring (a write): refused; creating one is the desktop's
//     keyring manager's job;
//   - a lookup that finds nothing (and nothing locked) is "not found".
//
// Items are stored with the attributes `service` and `account` (the label is
// the caller's), the shape `secret-tool store … service S account A` makes,
// so either reads the other's items. A write replaces every item with those
// attributes. See docs/secure-store.md.

#ifndef LAUFEY_SECRET_STORE_H_
#define LAUFEY_SECRET_STORE_H_

#include <cstdint>
#include <string>

namespace laufey_common {

// The LAUFEY_SECRET_* statuses (laufey.h).
enum class SecretStatus {
  kOk = 0,
  kNotFound = 1,
  kUnavailable = 2,
  kFailed = 3,
};

// The default bound on a call (an unlock prompt nobody answers), when the
// caller passes 0.
constexpr uint32_t kSecretDefaultTimeoutMs = 20000;

// Each call blocks the calling thread (never the UI thread) for at most
// about `timeout_ms` (0: kSecretDefaultTimeoutMs). `reason` gets why, for
// kUnavailable / kFailed. Strings are UTF-8; empty service or account is
// kFailed.
SecretStatus SecretLookup(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* value, std::string* reason);
SecretStatus SecretStore(const std::string& service, const std::string& account,
                         const std::string& label, const std::string& value,
                         uint32_t timeout_ms, std::string* reason);
// Deleting nothing is kOk.
SecretStatus SecretDelete(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* reason);

// The C ABI entry points (secret_lookup / secret_store / secret_delete):
// the calls above with C strings, the out strings malloc'd for string_free
// (NULL out pointers allowed), the LAUFEY_SECRET_* status returned.
int SecretLookupForAbi(const char* service, const char* account,
                       uint32_t timeout_ms, char** value, char** reason);
int SecretStoreForAbi(const char* service, const char* account,
                      const char* label, const char* value, uint32_t timeout_ms,
                      char** reason);
int SecretDeleteForAbi(const char* service, const char* account,
                       uint32_t timeout_ms, char** reason);

#if defined(__APPLE__)
// Which keychain the macOS store uses in this process: "data-protection" (the
// app is signed with a keychain access group) or "login".
const char* SecretKeychainKind();

// This process's keychain partition ID, as macOS's securityd ascribes it
// (securityd/src/clientid.cpp, partitionIdForProcess): "apple:",
// "teamid:<team>" (Developer ID, development, Mac App Store or TestFlight
// signed), "cdhash:<hex>" (any other signature: ad-hoc), "unsigned:"; empty
// when the process's code signature can't be read.
std::string SecretOwnPartitionId();

// Tests only: lookups and stores in the login keychain take an item whose
// partition list lacks SecretOwnPartitionId() for another program's, as
// they do in an Apple- or team-signed app, also in this (ad-hoc signed or
// unsigned) process.
void SecretRequireOwnPartitionForTesting(bool on);
#endif

// --- Pieces with no bus (tested on their own) --------------------------------

// The provider's name from the owner process's comm ("gnome-keyring-d" ->
// "gnome-keyring", "ksecretd" / "kwalletd6" -> "KWallet", "keepassxc" ->
// "KeePassXC"); the comm itself otherwise, "" for none.
std::string SecretProviderFromComm(const std::string& comm);

// Why there is no Secret Service: `kwallet_present` when kwalletd /
// ksecretd runs (or can be started) without serving
// org.freedesktop.secrets.
std::string NoSecretProviderReason(bool kwallet_present);

// Why a locked keyring refused: `provider` as SecretProviderFromComm names
// it ("" when unknown); `prompter`: someone could answer the unlock prompt;
// then `timed_out`: it went unanswered for `waited_ms`, else it was dismissed
// (or couldn't be shown).
std::string LockedSecretReason(const std::string& provider, bool prompter,
                               bool timed_out, uint32_t waited_ms);

// Test-only: load libsecret from `path` instead of libsecret-1.so.0 (a
// missing file tests the "not installed" reason). Before the first call.
void SetLibsecretPathForTesting(const char* path);

}  // namespace laufey_common

#endif  // LAUFEY_SECRET_STORE_H_
