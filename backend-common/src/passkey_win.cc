// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Windows passkeys: the Windows WebAuthn API (webauthn.dll, Windows 10 1903+,
// API version >= 1), loaded at runtime so the host still starts where it is
// missing. Shared by the WebView2 and CEF hosts. See laufey_passkey.h and
// docs/passkeys.md.
//
// WebAuthNAuthenticatorMakeCredential / GetAssertion block while the OS
// dialog is up, so each ceremony runs on its own worker thread; the HWND is
// only the dialog's owner. The caller builds clientDataJSON on Windows: we
// write {"type","challenge","origin":"https://<rpId>","crossOrigin":false}
// (what @clerk/electron-passkeys does too) and the OS hashes it. Windows
// accepts any RP ID without an entitlement.

#include "laufey_passkey.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h.
#include <webauthn.h>

#include <cstdio>
#include <string>
#include <thread>
#include <utility>

#include "laufey_backend_common.h"

namespace laufey_common {
namespace {

// HRESULTs the mapping names (winerror.h spells some only in newer SDKs).
constexpr HRESULT kUserCancelled = static_cast<HRESULT>(0x80090036L);  // NTE_
constexpr HRESULT kNotSupported = static_cast<HRESULT>(0x80090029L);   // NTE_
constexpr HRESULT kCancelled = static_cast<HRESULT>(0x800704C7L);      // ERROR_
constexpr HRESULT kTimeout = static_cast<HRESULT>(0x800705B4L);        // ERROR_

// Version 1 of WEBAUTHN_CLIENT_DATA / _COSE_CREDENTIAL_PARAMETER /
// _CREDENTIAL_EX (the SDK names only their CURRENT_VERSION, which a later SDK
// may raise).
constexpr DWORD kStructVersion1 = 1;

// Extra time the OS gets beyond our own deadline, so the timeout is always
// ours (reported as `timeout`) and the OS limit is only a backstop. It is
// not one where nobody answers the dialog (a CI runner): the call then runs
// past it until cancelled.
constexpr DWORD kOsTimeoutSlackMs = 10000;

struct WebAuthnApi {
  DWORD version = 0;
  decltype(&WebAuthNGetApiVersionNumber) get_api_version = nullptr;
  decltype(&WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable)
      is_uvpaa_available = nullptr;
  decltype(&WebAuthNAuthenticatorMakeCredential) make_credential = nullptr;
  decltype(&WebAuthNAuthenticatorGetAssertion) get_assertion = nullptr;
  decltype(&WebAuthNFreeCredentialAttestation) free_attestation = nullptr;
  decltype(&WebAuthNFreeAssertion) free_assertion = nullptr;
  decltype(&WebAuthNGetCancellationId) get_cancellation_id = nullptr;
  decltype(&WebAuthNCancelCurrentOperation) cancel_current_operation = nullptr;
  decltype(&WebAuthNGetErrorName) get_error_name = nullptr;

  bool usable() const {
    return version >= 1 && make_credential && get_assertion &&
           free_attestation && free_assertion;
  }
};

template <typename T>
void Resolve(HMODULE module, const char* name, T* out) {
  *out = reinterpret_cast<T>(
      reinterpret_cast<void*>(GetProcAddress(module, name)));
}

WebAuthnApi LoadWebAuthn() {
  WebAuthnApi api;
  // System32 only: never pick up a webauthn.dll planted next to the app.
  // Never freed (process lifetime).
  HMODULE module =
      LoadLibraryExW(L"webauthn.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module)
    return api;
  Resolve(module, "WebAuthNGetApiVersionNumber", &api.get_api_version);
  Resolve(module, "WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable",
          &api.is_uvpaa_available);
  Resolve(module, "WebAuthNAuthenticatorMakeCredential", &api.make_credential);
  Resolve(module, "WebAuthNAuthenticatorGetAssertion", &api.get_assertion);
  Resolve(module, "WebAuthNFreeCredentialAttestation", &api.free_attestation);
  Resolve(module, "WebAuthNFreeAssertion", &api.free_assertion);
  Resolve(module, "WebAuthNGetCancellationId", &api.get_cancellation_id);
  Resolve(module, "WebAuthNCancelCurrentOperation",
          &api.cancel_current_operation);
  Resolve(module, "WebAuthNGetErrorName", &api.get_error_name);
  if (api.get_api_version)
    api.version = api.get_api_version();
  return api;
}

const WebAuthnApi& Api() {
  static const WebAuthnApi api = LoadWebAuthn();
  return api;
}

std::string SystemMessage(HRESULT hr) {
  wchar_t* buffer = nullptr;
  DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                 FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, static_cast<DWORD>(hr), 0,
                             reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string out;
  if (len && buffer) {
    std::wstring w(buffer, len);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' ||
                          w.back() == L' ' || w.back() == L'.'))
      w.pop_back();
    out = WideToUtf8(w);
  }
  if (buffer)
    LocalFree(buffer);
  char code[32];
  std::snprintf(code, sizeof(code), "0x%08lX",
                static_cast<unsigned long>(static_cast<DWORD>(hr)));
  return out.empty() ? std::string("WebAuthn error ") + code
                     : out + " (" + code + ")";
}

PasskeyError MapHResult(HRESULT hr) {
  PasskeyError e;
  e.message = SystemMessage(hr);
  if (hr == kUserCancelled || hr == kCancelled) {
    e.code = kPasskeyCancelled;
    return e;
  }
  if (hr == kTimeout) {
    e.code = kPasskeyTimeout;
    return e;
  }
  if (hr == kNotSupported) {
    e.code = kPasskeyNotSupported;
    return e;
  }
  // The DOMException name the OS gives the HRESULT (as
  // @clerk/electron-passkeys maps it).
  std::wstring name;
  if (Api().get_error_name) {
    PCWSTR n = Api().get_error_name(hr);
    if (n)
      name = n;
  }
  if (name == L"NotAllowedError")
    e.code = kPasskeyCancelled;
  else if (name == L"SecurityError")
    e.code = kPasskeyInvalidRp;
  else if (name == L"NotSupportedError" || name == L"ConstraintError")
    e.code = kPasskeyNotSupported;
  else
    e.code = kPasskeyUnknown;
  return e;
}

std::vector<uint8_t> Copy(const BYTE* data, DWORD len) {
  if (!data || len == 0)
    return {};
  return std::vector<uint8_t>(data, data + len);
}

// Transport bits -> WebAuthn transport strings / attachment.
std::vector<std::string> TransportsFromMask(DWORD mask) {
  std::vector<std::string> out;
  if (mask & WEBAUTHN_CTAP_TRANSPORT_USB)
    out.push_back("usb");
  if (mask & WEBAUTHN_CTAP_TRANSPORT_NFC)
    out.push_back("nfc");
  if (mask & WEBAUTHN_CTAP_TRANSPORT_BLE)
    out.push_back("ble");
  if (mask & WEBAUTHN_CTAP_TRANSPORT_INTERNAL)
    out.push_back("internal");
  if (mask & 0x20 /* WEBAUTHN_CTAP_TRANSPORT_HYBRID */)
    out.push_back("hybrid");
  return out;
}

const char* AttachmentFromMask(DWORD mask) {
  return (mask & WEBAUTHN_CTAP_TRANSPORT_INTERNAL) ? "platform"
                                                   : "cross-platform";
}

DWORD UserVerification(const std::string& uv) {
  if (uv == "required")
    return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;
  if (uv == "preferred")
    return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
  if (uv == "discouraged")
    return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED;
  return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_ANY;
}

// Keeps the buffers behind a WEBAUTHN_CREDENTIAL_LIST alive for the call.
struct CredentialList {
  std::vector<WEBAUTHN_CREDENTIAL_EX> credentials;
  std::vector<PWEBAUTHN_CREDENTIAL_EX> pointers;
  WEBAUTHN_CREDENTIAL_LIST list{};

  explicit CredentialList(const std::vector<PasskeyCredentialDescriptor>& in) {
    credentials.reserve(in.size());
    for (const auto& d : in) {
      WEBAUTHN_CREDENTIAL_EX c{};
      c.dwVersion = kStructVersion1;
      c.cbId = static_cast<DWORD>(d.id.size());
      c.pbId = const_cast<BYTE*>(d.id.data());
      c.pwszCredentialType = WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY;
      c.dwTransports = 0;  // no transport restriction
      credentials.push_back(c);
    }
    for (auto& c : credentials)
      pointers.push_back(&c);
    list.cCredentials = static_cast<DWORD>(pointers.size());
    list.ppCredentials = pointers.data();
  }
};

std::string MakeCredential(HWND hwnd, const PasskeyCreationOptions& o,
                           DWORD timeout_ms, GUID* cancellation_id) {
  const WebAuthnApi& api = Api();
  std::wstring rp_id = Utf8ToWide(o.rp_id);
  std::wstring rp_name = Utf8ToWide(o.rp_name.empty() ? o.rp_id : o.rp_name);
  // As @clerk/electron-passkeys: name and display name fall back to each
  // other.
  std::wstring user_name =
      Utf8ToWide(!o.user_name.empty() ? o.user_name : o.user_display_name);
  std::wstring display_name = Utf8ToWide(
      !o.user_display_name.empty() ? o.user_display_name : o.user_name);

  WEBAUTHN_RP_ENTITY_INFORMATION rp{};
  rp.dwVersion = WEBAUTHN_RP_ENTITY_INFORMATION_VERSION_1;
  rp.pwszId = rp_id.c_str();
  rp.pwszName = rp_name.c_str();

  WEBAUTHN_USER_ENTITY_INFORMATION user{};
  user.dwVersion = WEBAUTHN_USER_ENTITY_INFORMATION_VERSION_1;
  user.cbId = static_cast<DWORD>(o.user_id.size());
  user.pbId = const_cast<BYTE*>(o.user_id.data());
  user.pwszName = user_name.c_str();
  user.pwszDisplayName = display_name.c_str();

  std::vector<int32_t> algs = o.algorithms;
  if (algs.empty())
    algs = {-7 /* ES256 */, -257 /* RS256 */};
  std::vector<WEBAUTHN_COSE_CREDENTIAL_PARAMETER> params;
  for (int32_t alg : algs) {
    WEBAUTHN_COSE_CREDENTIAL_PARAMETER p{};
    p.dwVersion = kStructVersion1;
    p.pwszCredentialType = WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY;
    p.lAlg = alg;
    params.push_back(p);
  }
  WEBAUTHN_COSE_CREDENTIAL_PARAMETERS cose{};
  cose.cCredentialParameters = static_cast<DWORD>(params.size());
  cose.pCredentialParameters = params.data();

  std::string client_data_json = BuildPasskeyClientDataJson(
      "webauthn.create", o.challenge_b64url, o.rp_id);
  WEBAUTHN_CLIENT_DATA client_data{};
  client_data.dwVersion = kStructVersion1;
  client_data.cbClientDataJSON = static_cast<DWORD>(client_data_json.size());
  client_data.pbClientDataJSON =
      reinterpret_cast<BYTE*>(const_cast<char*>(client_data_json.data()));
  client_data.pwszHashAlgId = WEBAUTHN_HASH_ALGORITHM_SHA_256;

  CredentialList exclude(o.exclude_credentials);

  // Version 3: the layout every API version >= 1 understands. Later members
  // stay zero.
  WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS options{};
  options.dwVersion = WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_3;
  options.dwTimeoutMilliseconds = timeout_ms;
  if (o.authenticator_attachment == "platform")
    options.dwAuthenticatorAttachment =
        WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM;
  else if (o.authenticator_attachment == "cross-platform")
    options.dwAuthenticatorAttachment =
        WEBAUTHN_AUTHENTICATOR_ATTACHMENT_CROSS_PLATFORM;
  else
    options.dwAuthenticatorAttachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
  options.bRequireResidentKey = o.resident_key == "required" ? TRUE : FALSE;
  options.dwUserVerificationRequirement = UserVerification(o.user_verification);
  if (o.attestation == "none")
    options.dwAttestationConveyancePreference =
        WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE;
  else if (o.attestation == "indirect")
    options.dwAttestationConveyancePreference =
        WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_INDIRECT;
  else if (o.attestation == "direct" || o.attestation == "enterprise")
    options.dwAttestationConveyancePreference =
        WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_DIRECT;
  else
    options.dwAttestationConveyancePreference =
        WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_ANY;
  options.pCancellationId = cancellation_id;
  if (!o.exclude_credentials.empty())
    options.pExcludeCredentialList = &exclude.list;

  PWEBAUTHN_CREDENTIAL_ATTESTATION attestation = nullptr;
  HRESULT hr = api.make_credential(hwnd, &rp, &user, &cose, &client_data,
                                   &options, &attestation);
  if (FAILED(hr)) {
    if (attestation)
      api.free_attestation(attestation);
    PasskeyError e = MapHResult(hr);
    return PasskeyErrorEnvelope(e.code, e.message);
  }
  if (!attestation) {
    return PasskeyErrorEnvelope(kPasskeyUnknown,
                                "WebAuthn returned no attestation");
  }
  PasskeyRegistrationResult r;
  r.credential_id =
      Copy(attestation->pbCredentialId, attestation->cbCredentialId);
  r.client_data_json.assign(client_data_json.begin(), client_data_json.end());
  r.attestation_object =
      Copy(attestation->pbAttestationObject, attestation->cbAttestationObject);
  // dwUsedTransport exists from WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_3.
  if (attestation->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_3) {
    r.attachment = AttachmentFromMask(attestation->dwUsedTransport);
    r.transports = TransportsFromMask(attestation->dwUsedTransport);
  }
  api.free_attestation(attestation);
  return PasskeyRegistrationEnvelope(r);
}

std::string GetAssertion(HWND hwnd, const PasskeyRequestOptions& o,
                         DWORD timeout_ms, GUID* cancellation_id) {
  const WebAuthnApi& api = Api();
  std::wstring rp_id = Utf8ToWide(o.rp_id);
  std::string client_data_json =
      BuildPasskeyClientDataJson("webauthn.get", o.challenge_b64url, o.rp_id);
  WEBAUTHN_CLIENT_DATA client_data{};
  client_data.dwVersion = kStructVersion1;
  client_data.cbClientDataJSON = static_cast<DWORD>(client_data_json.size());
  client_data.pbClientDataJSON =
      reinterpret_cast<BYTE*>(const_cast<char*>(client_data_json.data()));
  client_data.pwszHashAlgId = WEBAUTHN_HASH_ALGORITHM_SHA_256;

  CredentialList allow(o.allow_credentials);

  // Version 4: the layout every API version >= 1 understands.
  WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS options{};
  options.dwVersion = WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_4;
  options.dwTimeoutMilliseconds = timeout_ms;
  options.dwAuthenticatorAttachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
  options.dwUserVerificationRequirement = UserVerification(o.user_verification);
  options.pCancellationId = cancellation_id;
  if (!o.allow_credentials.empty())
    options.pAllowCredentialList = &allow.list;

  PWEBAUTHN_ASSERTION assertion = nullptr;
  HRESULT hr = api.get_assertion(hwnd, rp_id.c_str(), &client_data, &options,
                                 &assertion);
  if (FAILED(hr)) {
    if (assertion)
      api.free_assertion(assertion);
    PasskeyError e = MapHResult(hr);
    return PasskeyErrorEnvelope(e.code, e.message);
  }
  if (!assertion) {
    return PasskeyErrorEnvelope(kPasskeyUnknown,
                                "WebAuthn returned no assertion");
  }
  PasskeyAssertionResult r;
  r.credential_id =
      Copy(assertion->Credential.pbId, assertion->Credential.cbId);
  r.client_data_json.assign(client_data_json.begin(), client_data_json.end());
  r.authenticator_data =
      Copy(assertion->pbAuthenticatorData, assertion->cbAuthenticatorData);
  r.signature = Copy(assertion->pbSignature, assertion->cbSignature);
  r.user_handle = Copy(assertion->pbUserId, assertion->cbUserId);
  // dwUsedTransport exists from WEBAUTHN_ASSERTION_VERSION_4; older
  // assertions leave the attachment unknown (omitted).
  if (assertion->dwVersion >= WEBAUTHN_ASSERTION_VERSION_4)
    r.attachment = AttachmentFromMask(assertion->dwUsedTransport);
  api.free_assertion(assertion);
  return PasskeyAssertionEnvelope(r);
}

bool IsOurs(HWND hwnd) {
  DWORD pid = 0;
  if (hwnd)
    GetWindowThreadProcessId(hwnd, &pid);
  return pid == GetCurrentProcessId();
}

BOOL CALLBACK FindVisibleOwnWindow(HWND hwnd, LPARAM out) {
  if (IsOurs(hwnd) && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
    *reinterpret_cast<HWND*>(out) = hwnd;
    return FALSE;  // stop
  }
  return TRUE;
}

// The dialog owner when the caller named no window: the foreground window
// when it is ours, else the first visible top-level window of this process.
HWND DefaultOwner() {
  HWND foreground = GetForegroundWindow();
  if (foreground && IsOurs(foreground))
    return foreground;
  HWND found = nullptr;
  EnumWindows(FindVisibleOwnWindow, reinterpret_cast<LPARAM>(&found));
  return found;
}

}  // namespace

uint32_t PasskeyCapabilitiesWin() {
  const WebAuthnApi& api = Api();
  if (!api.usable())
    return 0;
  uint32_t flags = LAUFEY_PASSKEY_SECURITY_KEYS;
  BOOL available = FALSE;
  if (api.is_uvpaa_available && SUCCEEDED(api.is_uvpaa_available(&available)) &&
      available) {
    flags |= LAUFEY_PASSKEY_PLATFORM_AUTHENTICATOR;
  }
  return flags;
}

void PasskeyStartWin(std::shared_ptr<PasskeyCeremony> ceremony,
                     void* hwnd_ptr) {
  if (!ceremony)
    return;
  const WebAuthnApi& api = Api();
  if (!api.usable()) {
    ceremony->Finish(PasskeyErrorEnvelope(
        kPasskeyNotSupported,
        "the Windows WebAuthn API (webauthn.dll, Windows 10 1903 or newer) "
        "is not available"));
    return;
  }
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd)
    hwnd = DefaultOwner();
  if (!hwnd) {
    ceremony->Finish(PasskeyErrorEnvelope(
        kPasskeyUnknown, "no window to anchor the passkey request to"));
    return;
  }
  ceremony->SetWindowKey(hwnd);

  // A cancellation id lets a timeout or a closing window end the OS dialog.
  // A cancel that reaches the OS before the call below has registered the
  // id is dropped (S_OK, yet the call goes on), and the call doesn't honour
  // its own timeout when nobody answers the dialog, so the canceller repeats
  // until the call returns (kPasskeyCancelRepeatMs).
  auto cancellation_id = std::make_shared<GUID>();
  bool cancellable = api.get_cancellation_id && api.cancel_current_operation &&
                     SUCCEEDED(api.get_cancellation_id(cancellation_id.get()));
  if (cancellable) {
    ceremony->SetCanceller(
        [cancellation_id] {
          Api().cancel_current_operation(cancellation_id.get());
        },
        kPasskeyCancelRepeatMs);
  }

  DWORD timeout = ceremony->has_timeout() ? ceremony->timeout_ms()
                                          : kPasskeyDefaultTimeoutMs;
  ceremony->StartTimeout(timeout);

  std::thread([ceremony, hwnd, timeout, cancellation_id, cancellable] {
    if (ceremony->delivered()) {
      // Aborted before the OS call started.
      ceremony->Finish(
          PasskeyErrorEnvelope(kPasskeyCancelled, "the request was aborted"));
      return;
    }
    GUID* id = cancellable ? cancellation_id.get() : nullptr;
    DWORD os_timeout = timeout + kOsTimeoutSlackMs;
    std::string envelope =
        ceremony->is_create()
            ? MakeCredential(hwnd, ceremony->creation(), os_timeout, id)
            : GetAssertion(hwnd, ceremony->request(), os_timeout, id);
    ceremony->Finish(envelope);
  }).detach();
}

}  // namespace laufey_common
