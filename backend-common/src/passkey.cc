// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The platform-independent half of passkeys. See laufey_passkey.h and
// docs/passkeys.md.

#include "laufey_passkey.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <utility>

#include "json_reader.h"
#include "laufey_launch_config.h"
#include "laufey_single_instance.h"  // IsValidUtf8, SanitizeUtf8

namespace laufey_common {

namespace {

using json::JsonReader;
using json::JsonValue;

// --- base64url ---------------------------------------------------------------

constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int B64Value(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '-')
    return 62;
  if (c == '_')
    return 63;
  return -1;
}

// --- JSON output -------------------------------------------------------------

// `s` as a JSON string literal. `s` must be valid UTF-8 (callers sanitize).
void AppendJsonString(std::string* out, const std::string& s) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out->append("\\\"");
        break;
      case '\\':
        out->append("\\\\");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      default:
        if (c < 0x20) {
          static const char kHex[] = "0123456789abcdef";
          out->append("\\u00");
          out->push_back(kHex[c >> 4]);
          out->push_back(kHex[c & 0xF]);
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

void AppendStringArray(std::string* out, const std::vector<std::string>& v) {
  out->push_back('[');
  for (size_t i = 0; i < v.size(); ++i) {
    if (i)
      out->push_back(',');
    AppendJsonString(out, SanitizeUtf8(v[i]));
  }
  out->push_back(']');
}

// The members every credential shares: id, rawId, type,
// authenticatorAttachment (left out when unknown).
void AppendCredentialHead(std::string* out, const std::vector<uint8_t>& id,
                          const std::string& attachment) {
  std::string id_b64 = Base64UrlEncode(id);
  out->append("{\"id\":");
  AppendJsonString(out, id_b64);
  out->append(",\"rawId\":");
  AppendJsonString(out, id_b64);
  out->append(",\"type\":\"public-key\"");
  if (!attachment.empty()) {
    out->append(",\"authenticatorAttachment\":");
    AppendJsonString(out, SanitizeUtf8(attachment));
  }
}

void AppendB64Member(std::string* out, const char* name,
                     const std::vector<uint8_t>& bytes) {
  out->push_back('"');
  out->append(name);
  out->append("\":");
  AppendJsonString(out, Base64UrlEncode(bytes));
}

// --- Options parsing ---------------------------------------------------------

constexpr const char* kKnownTransports[] = {"usb",      "nfc",    "ble",
                                            "internal", "hybrid", "smart-card"};

bool IsOneOf(const std::string& s, std::initializer_list<const char*> values) {
  for (const char* v : values) {
    if (s == v)
      return true;
  }
  return false;
}

class OptionsParser {
 public:
  explicit OptionsParser(PasskeyError* error) : error_(error) {}

  bool Fail(const char* code, const std::string& message) {
    error_->code = code;
    error_->message = "invalid passkey options: " + message;
    return false;
  }
  bool Fail(const std::string& message) {
    return Fail(kPasskeyUnknown, message);
  }

  // The document as an object.
  bool ParseRoot(const std::string& text, JsonValue* root) {
    if (text.size() > kPasskeyMaxOptionsBytes) {
      return Fail("larger than " + std::to_string(kPasskeyMaxOptionsBytes) +
                  " bytes");
    }
    if (!IsValidUtf8(text))
      return Fail("not valid UTF-8");
    std::string error;
    if (!JsonReader(text).ParseDocument(root, &error))
      return Fail("not valid JSON (" + error + ")");
    if (root->type != JsonValue::Type::kObject)
      return Fail("the document is not an object");
    return true;
  }

  // Member `key` of `object`, or nullptr when absent or null. A key given
  // twice is an error (JSON.stringify never writes one).
  bool Member(const JsonValue& object, const std::string& path, const char* key,
              const JsonValue** out) {
    *out = nullptr;
    for (const auto& member : object.object) {
      if (member.first != key)
        continue;
      if (*out)
        return Fail(path + key + " is given more than once");
      *out = &member.second;
    }
    if (*out && (*out)->type == JsonValue::Type::kNull)
      *out = nullptr;
    return true;
  }

  bool OptionalString(const JsonValue& object, const std::string& path,
                      const char* key, std::string* out, bool* present) {
    const JsonValue* v = nullptr;
    if (!Member(object, path, key, &v))
      return false;
    if (present)
      *present = v != nullptr;
    if (!v)
      return true;
    if (v->type != JsonValue::Type::kString)
      return Fail(path + key + " must be a string");
    *out = v->string;
    return true;
  }

  bool RequiredString(const JsonValue& object, const std::string& path,
                      const char* key, std::string* out, const char* code) {
    bool present = false;
    if (!OptionalString(object, path, key, out, &present))
      return false;
    if (!present)
      return Fail(code, path + key + " is required");
    return true;
  }

  bool RequiredObject(const JsonValue& object, const std::string& path,
                      const char* key, const JsonValue** out,
                      const char* code) {
    if (!Member(object, path, key, out))
      return false;
    if (!*out)
      return Fail(code, path + key + " is required");
    if ((*out)->type != JsonValue::Type::kObject)
      return Fail(code, path + key + " must be an object");
    return true;
  }

  // A required base64url member, decoded, of [min, max] bytes.
  bool RequiredBytes(const JsonValue& object, const std::string& path,
                     const char* key, size_t min, size_t max,
                     std::vector<uint8_t>* out) {
    std::string text;
    if (!RequiredString(object, path, key, &text, kPasskeyUnknown))
      return false;
    if (!Base64UrlDecode(text, out))
      return Fail(path + key + " is not valid base64url");
    if (out->size() < min)
      return Fail(path + key + " is empty");
    if (out->size() > max) {
      return Fail(path + key + " is longer than " + std::to_string(max) +
                  " bytes");
    }
    return true;
  }

  bool RpId(const JsonValue& object, const std::string& path, const char* key,
            std::string* out) {
    if (!RequiredString(object, path, key, out, kPasskeyInvalidRp))
      return false;
    if (out->empty())
      return Fail(kPasskeyInvalidRp, path + key + " is empty");
    if (!IsValidPasskeyRpId(*out)) {
      return Fail(kPasskeyInvalidRp,
                  path + key + " is not a valid domain (lowercase ASCII)");
    }
    return true;
  }

  bool Timeout(const JsonValue& object, bool* has, uint32_t* ms) {
    const JsonValue* v = nullptr;
    if (!Member(object, "", "timeout", &v))
      return false;
    *has = false;
    if (!v)
      return true;
    if (v->type != JsonValue::Type::kNumber || !v->is_integer ||
        v->integer < 0 || v->integer > 0xFFFFFFFFll) {
      return Fail("timeout must be a non-negative integer");
    }
    int64_t t = v->integer;
    if (t < kPasskeyMinTimeoutMs)
      t = kPasskeyMinTimeoutMs;
    if (t > kPasskeyMaxTimeoutMs)
      t = kPasskeyMaxTimeoutMs;
    *has = true;
    *ms = static_cast<uint32_t>(t);
    return true;
  }

  // An enumeration member: kept when it is one of `values`, dropped (left
  // empty) when it is another string (WebAuthn ignores unknown values).
  bool Enum(const JsonValue& object, const std::string& path, const char* key,
            std::initializer_list<const char*> values, std::string* out) {
    std::string value;
    bool present = false;
    if (!OptionalString(object, path, key, &value, &present))
      return false;
    if (present && IsOneOf(value, values))
      *out = value;
    return true;
  }

  bool Descriptors(const JsonValue& object, const char* key,
                   std::vector<PasskeyCredentialDescriptor>* out) {
    const JsonValue* list = nullptr;
    if (!Member(object, "", key, &list))
      return false;
    if (!list)
      return true;
    if (list->type != JsonValue::Type::kArray)
      return Fail(std::string(key) + " must be an array");
    if (list->array.size() > kPasskeyMaxCredentials) {
      return Fail(std::string(key) + " has more than " +
                  std::to_string(kPasskeyMaxCredentials) + " entries");
    }
    for (size_t i = 0; i < list->array.size(); ++i) {
      const JsonValue& entry = list->array[i];
      std::string path = std::string(key) + "[" + std::to_string(i) + "].";
      if (entry.type != JsonValue::Type::kObject)
        return Fail(std::string(key) + " entries must be objects");
      std::string type;
      bool has_type = false;
      if (!OptionalString(entry, path, "type", &type, &has_type))
        return false;
      PasskeyCredentialDescriptor d;
      if (!RequiredBytes(entry, path, "id", 1, kPasskeyMaxCredentialIdBytes,
                         &d.id)) {
        return false;
      }
      const JsonValue* transports = nullptr;
      if (!Member(entry, path, "transports", &transports))
        return false;
      if (transports) {
        if (transports->type != JsonValue::Type::kArray)
          return Fail(path + "transports must be an array");
        if (transports->array.size() > kPasskeyMaxTransports) {
          return Fail(path + "transports has more than " +
                      std::to_string(kPasskeyMaxTransports) + " entries");
        }
        for (const JsonValue& t : transports->array) {
          if (t.type != JsonValue::Type::kString)
            return Fail(path + "transports entries must be strings");
          for (const char* known : kKnownTransports) {
            if (t.string == known) {
              d.transports.push_back(t.string);
              break;
            }
          }
        }
      }
      // WebAuthn: descriptors of a type the client doesn't know are skipped.
      if (has_type && type != "public-key")
        continue;
      out->push_back(std::move(d));
    }
    return true;
  }

  bool Challenge(const JsonValue& root, std::vector<uint8_t>* bytes,
                 std::string* canonical) {
    // No upper bound beyond the document cap: browsers set none.
    if (!RequiredBytes(root, "", "challenge", 1, kPasskeyMaxOptionsBytes,
                       bytes)) {
      return false;
    }
    *canonical = Base64UrlEncode(*bytes);
    return true;
  }

 private:
  PasskeyError* error_;
};

// --- The app-wide slot -------------------------------------------------------

std::mutex& SlotMutex() {
  static std::mutex m;
  return m;
}
// The ceremony holding the slot (raw pointer for identity; the weak pointer
// for PasskeyWindowClosing). Guarded by SlotMutex().
const PasskeyCeremony* g_slot_owner = nullptr;
std::weak_ptr<PasskeyCeremony>& SlotWeak() {
  static std::weak_ptr<PasskeyCeremony> w;
  return w;
}

void ReleaseSlot(const PasskeyCeremony* owner) {
  std::lock_guard<std::mutex> lock(SlotMutex());
  if (g_slot_owner == owner) {
    g_slot_owner = nullptr;
    SlotWeak().reset();
  }
}

}  // namespace

// --- Public helpers
// ------------------------------------------------------------

std::string Base64UrlEncode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve((len * 4 + 2) / 3);
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    uint32_t v =
        (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out.push_back(kB64Alphabet[(v >> 18) & 63]);
    out.push_back(kB64Alphabet[(v >> 12) & 63]);
    out.push_back(kB64Alphabet[(v >> 6) & 63]);
    out.push_back(kB64Alphabet[v & 63]);
  }
  size_t rest = len - i;
  if (rest == 1) {
    uint32_t v = uint32_t(data[i]) << 16;
    out.push_back(kB64Alphabet[(v >> 18) & 63]);
    out.push_back(kB64Alphabet[(v >> 12) & 63]);
  } else if (rest == 2) {
    uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out.push_back(kB64Alphabet[(v >> 18) & 63]);
    out.push_back(kB64Alphabet[(v >> 12) & 63]);
    out.push_back(kB64Alphabet[(v >> 6) & 63]);
  }
  return out;
}

std::string Base64UrlEncode(const std::vector<uint8_t>& data) {
  return Base64UrlEncode(data.data(), data.size());
}

bool Base64UrlDecode(const std::string& in, std::vector<uint8_t>* out) {
  out->clear();
  size_t n = in.size();
  // Up to two '=' of padding.
  size_t pad = 0;
  while (pad < 2 && n > 0 && in[n - 1] == '=') {
    --n;
    ++pad;
  }
  if (pad > 0 && in.size() % 4 != 0)
    return false;  // padding, but not to a full quantum
  if (n % 4 == 1)
    return false;
  out->reserve(n * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; ++i) {
    int v = B64Value(in[i]);
    if (v < 0)
      return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
    }
  }
  // Leftover bits must be zero (canonical encoding).
  if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) {
    out->clear();
    return false;
  }
  return true;
}

bool ParsePasskeyCreationOptions(const std::string& text,
                                 PasskeyCreationOptions* out,
                                 PasskeyError* error) {
  *out = PasskeyCreationOptions();
  OptionsParser p(error);
  JsonValue root;
  if (!p.ParseRoot(text, &root))
    return false;

  const JsonValue* rp = nullptr;
  if (!p.RequiredObject(root, "", "rp", &rp, kPasskeyInvalidRp))
    return false;
  if (!p.RpId(*rp, "rp.", "id", &out->rp_id))
    return false;
  if (!p.OptionalString(*rp, "rp.", "name", &out->rp_name, nullptr))
    return false;

  const JsonValue* user = nullptr;
  if (!p.RequiredObject(root, "", "user", &user, kPasskeyUnknown))
    return false;
  if (!p.RequiredBytes(*user, "user.", "id", 1, kPasskeyMaxUserIdBytes,
                       &out->user_id)) {
    return false;
  }
  if (!p.OptionalString(*user, "user.", "name", &out->user_name, nullptr) ||
      !p.OptionalString(*user, "user.", "displayName", &out->user_display_name,
                        nullptr)) {
    return false;
  }

  if (!p.Challenge(root, &out->challenge, &out->challenge_b64url))
    return false;

  const JsonValue* params = nullptr;
  if (!p.Member(root, "", "pubKeyCredParams", &params))
    return false;
  if (params) {
    if (params->type != JsonValue::Type::kArray)
      return p.Fail("pubKeyCredParams must be an array");
    if (params->array.size() > kPasskeyMaxAlgorithms) {
      return p.Fail("pubKeyCredParams has more than " +
                    std::to_string(kPasskeyMaxAlgorithms) + " entries");
    }
    for (size_t i = 0; i < params->array.size(); ++i) {
      const JsonValue& entry = params->array[i];
      std::string path = "pubKeyCredParams[" + std::to_string(i) + "].";
      if (entry.type != JsonValue::Type::kObject)
        return p.Fail("pubKeyCredParams entries must be objects");
      std::string type;
      bool has_type = false;
      if (!p.OptionalString(entry, path, "type", &type, &has_type))
        return false;
      const JsonValue* alg = nullptr;
      if (!p.Member(entry, path, "alg", &alg))
        return false;
      if (!alg || alg->type != JsonValue::Type::kNumber || !alg->is_integer ||
          alg->integer < INT32_MIN || alg->integer > INT32_MAX) {
        return p.Fail(path + "alg must be an integer");
      }
      if (has_type && type != "public-key")
        continue;
      out->algorithms.push_back(static_cast<int32_t>(alg->integer));
    }
    // WebAuthn: a non-empty list with no supported entry is NotSupportedError.
    if (!params->array.empty() && out->algorithms.empty()) {
      return p.Fail(kPasskeyNotSupported,
                    "pubKeyCredParams has no \"public-key\" entry");
    }
  }

  if (!p.Timeout(root, &out->has_timeout, &out->timeout_ms))
    return false;

  const JsonValue* selection = nullptr;
  if (!p.Member(root, "", "authenticatorSelection", &selection))
    return false;
  if (selection) {
    if (selection->type != JsonValue::Type::kObject)
      return p.Fail("authenticatorSelection must be an object");
    const std::string path = "authenticatorSelection.";
    if (!p.Enum(*selection, path, "authenticatorAttachment",
                {"platform", "cross-platform"},
                &out->authenticator_attachment) ||
        !p.Enum(*selection, path, "residentKey",
                {"discouraged", "preferred", "required"}, &out->resident_key) ||
        !p.Enum(*selection, path, "userVerification",
                {"required", "preferred", "discouraged"},
                &out->user_verification)) {
      return false;
    }
    const JsonValue* require = nullptr;
    if (!p.Member(*selection, path, "requireResidentKey", &require))
      return false;
    if (require && require->type != JsonValue::Type::kBool)
      return p.Fail(path + "requireResidentKey must be a boolean");
    // WebAuthn L3 5.4.4: requireResidentKey only counts when residentKey is
    // absent (or a value this version doesn't know).
    if (out->resident_key.empty() && require)
      out->resident_key = require->boolean ? "required" : "discouraged";
  }

  if (!p.Enum(root, "", "attestation",
              {"none", "indirect", "direct", "enterprise"},
              &out->attestation)) {
    return false;
  }
  return p.Descriptors(root, "excludeCredentials", &out->exclude_credentials);
}

bool ParsePasskeyRequestOptions(const std::string& text,
                                PasskeyRequestOptions* out,
                                PasskeyError* error) {
  *out = PasskeyRequestOptions();
  OptionsParser p(error);
  JsonValue root;
  if (!p.ParseRoot(text, &root))
    return false;
  if (!p.RpId(root, "", "rpId", &out->rp_id))
    return false;
  if (!p.Challenge(root, &out->challenge, &out->challenge_b64url))
    return false;
  if (!p.Timeout(root, &out->has_timeout, &out->timeout_ms))
    return false;
  if (!p.Enum(root, "", "userVerification",
              {"required", "preferred", "discouraged"},
              &out->user_verification)) {
    return false;
  }
  return p.Descriptors(root, "allowCredentials", &out->allow_credentials);
}

std::string PasskeyErrorEnvelope(const std::string& code,
                                 const std::string& message) {
  std::string out = "{\"ok\":false,\"error\":{\"code\":";
  AppendJsonString(&out, SanitizeUtf8(code));
  out.append(",\"message\":");
  AppendJsonString(&out, SanitizeUtf8(message));
  out.append("}}");
  return out;
}

std::string PasskeyRegistrationEnvelope(const PasskeyRegistrationResult& r) {
  std::string out = "{\"ok\":true,\"credential\":";
  AppendCredentialHead(&out, r.credential_id, r.attachment);
  out.append(",\"response\":{");
  AppendB64Member(&out, "clientDataJSON", r.client_data_json);
  out.push_back(',');
  AppendB64Member(&out, "attestationObject", r.attestation_object);
  out.append(",\"transports\":");
  AppendStringArray(&out, r.transports);
  out.append("}}}");
  return out;
}

std::string PasskeyAssertionEnvelope(const PasskeyAssertionResult& r) {
  std::string out = "{\"ok\":true,\"credential\":";
  AppendCredentialHead(&out, r.credential_id, r.attachment);
  out.append(",\"response\":{");
  AppendB64Member(&out, "clientDataJSON", r.client_data_json);
  out.push_back(',');
  AppendB64Member(&out, "authenticatorData", r.authenticator_data);
  out.push_back(',');
  AppendB64Member(&out, "signature", r.signature);
  if (!r.user_handle.empty()) {
    out.push_back(',');
    AppendB64Member(&out, "userHandle", r.user_handle);
  }
  out.append("}}}");
  return out;
}

std::string BuildPasskeyClientDataJson(const char* type,
                                       const std::string& challenge_b64url,
                                       const std::string& rp_id) {
  std::string out = "{\"type\":";
  AppendJsonString(&out, type);
  out.append(",\"challenge\":");
  AppendJsonString(&out, challenge_b64url);
  out.append(",\"origin\":");
  AppendJsonString(&out, "https://" + rp_id);
  out.append(",\"crossOrigin\":false}");
  return out;
}

// --- PasskeyCeremony
// -----------------------------------------------------------

PasskeyCeremony::PasskeyCeremony(uint32_t kind,
                                 laufey_passkey_result_fn callback,
                                 void* user_data)
    : kind_(kind), callback_(callback), user_data_(user_data) {}

PasskeyCeremony::~PasskeyCeremony() {
  // Every path is supposed to end in Finish; if one didn't (a backend bug),
  // free the slot and still answer, so the caller never waits forever.
  if (!finished_)
    ReleaseSlot(this);
  if (!delivered_) {
    delivered_ = true;
    Deliver(PasskeyErrorEnvelope(kPasskeyUnknown,
                                 "the passkey request ended without a result"));
  }
}

bool PasskeyCeremony::has_timeout() const {
  return is_create() ? creation_.has_timeout : request_.has_timeout;
}

uint32_t PasskeyCeremony::timeout_ms() const {
  return is_create() ? creation_.timeout_ms : request_.timeout_ms;
}

void PasskeyCeremony::Deliver(const std::string& envelope) {
  if (callback_)
    callback_(user_data_, envelope.c_str());
}

void PasskeyCeremony::Finish(const std::string& envelope) {
  bool deliver = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_)
      return;
    finished_ = true;
    deliver = !delivered_;
    delivered_ = true;
    canceller_ = nullptr;
  }
  cv_.notify_all();
  // Free the slot BEFORE answering, so the caller may start the next request
  // from inside its callback.
  ReleaseSlot(this);
  if (deliver)
    Deliver(envelope);
}

void PasskeyCeremony::Abort(const char* code, const std::string& message) {
  std::function<void()> cancel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (delivered_ || finished_)
      return;
    delivered_ = true;
    aborted_ = true;
    if (canceller_ && !cancel_ran_) {
      cancel_ran_ = true;
      cancel = std::move(canceller_);
    }
    canceller_ = nullptr;
  }
  cv_.notify_all();
  // Ask the OS to stop first; the slot stays held until it reports back
  // (Finish), so a new request can't race the dying one.
  if (cancel)
    cancel();
  Deliver(PasskeyErrorEnvelope(code, message));
}

void PasskeyCeremony::SetCanceller(std::function<void()> cancel) {
  bool run_now = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_ || cancel_ran_)
      return;
    if (aborted_) {
      cancel_ran_ = true;
      run_now = true;
    } else {
      canceller_ = std::move(cancel);
    }
  }
  if (run_now && cancel)
    cancel();
}

void PasskeyCeremony::SetWindowKey(const void* key) {
  std::lock_guard<std::mutex> lock(mutex_);
  window_key_ = key;
}

const void* PasskeyCeremony::window_key() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return window_key_;
}

bool PasskeyCeremony::delivered() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return delivered_;
}

bool PasskeyCeremony::finished() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return finished_;
}

void PasskeyCeremony::StartTimeout(uint32_t ms) {
  std::shared_ptr<PasskeyCeremony> self = shared_from_this();
  std::thread([self, ms] {
    bool expired;
    {
      std::unique_lock<std::mutex> lock(self->mutex_);
      expired = !self->cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] {
        return self->delivered_ || self->finished_;
      });
    }
    if (expired)
      self->Abort(kPasskeyTimeout, "the passkey request timed out");
  }).detach();
}

// --- Entry points
// --------------------------------------------------------------

void PasskeyReportError(laufey_passkey_result_fn callback, void* user_data,
                        const char* code, const std::string& message) {
  if (!callback)
    return;
  std::string envelope = PasskeyErrorEnvelope(code, message);
  callback(user_data, envelope.c_str());
}

void PasskeyReportNotSupported(laufey_passkey_result_fn callback,
                               void* user_data) {
  PasskeyReportError(callback, user_data, kPasskeyNotSupported,
                     "Native passkeys are not supported on this platform.");
}

bool IsPasskeyRpIdAllowed(const std::string& rp_id,
                          const std::vector<std::string>& allowed) {
  auto lower = [](char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  };
  for (const std::string& candidate : allowed) {
    if (candidate.size() != rp_id.size())
      continue;
    bool same = true;
    for (size_t i = 0; same && i < rp_id.size(); ++i)
      same = lower(candidate[i]) == lower(rp_id[i]);
    if (same)
      return true;
  }
  return false;
}

std::shared_ptr<PasskeyCeremony> PasskeyBegin(uint32_t kind,
                                              const char* options_json,
                                              laufey_passkey_result_fn callback,
                                              void* user_data) {
  const LaunchConfig& launch = ProcessLaunchConfig();
  return PasskeyBeginWithRpIds(
      kind, options_json, callback, user_data,
      launch.has_passkey_rp_ids ? &launch.passkey_rp_ids : nullptr);
}

std::shared_ptr<PasskeyCeremony> PasskeyBeginWithRpIds(
    uint32_t kind, const char* options_json, laufey_passkey_result_fn callback,
    void* user_data, const std::vector<std::string>* allowed_rp_ids) {
  if (!callback)
    return nullptr;
  if (kind != LAUFEY_PASSKEY_CREATE && kind != LAUFEY_PASSKEY_GET) {
    PasskeyReportError(callback, user_data, kPasskeyUnknown,
                       "unknown passkey request kind");
    return nullptr;
  }
  if (!options_json) {
    PasskeyReportError(callback, user_data, kPasskeyUnknown,
                       "invalid passkey options: none given");
    return nullptr;
  }
  // Bounded length check: never scan more than the cap + 1 bytes.
  size_t len = 0;
  while (len <= kPasskeyMaxOptionsBytes && options_json[len] != '\0')
    ++len;
  if (len > kPasskeyMaxOptionsBytes) {
    PasskeyReportError(callback, user_data, kPasskeyUnknown,
                       "invalid passkey options: larger than " +
                           std::to_string(kPasskeyMaxOptionsBytes) + " bytes");
    return nullptr;
  }

  auto ceremony = std::make_shared<PasskeyCeremony>(kind, callback, user_data);
  std::string text(options_json, len);
  PasskeyError error;
  bool ok =
      kind == LAUFEY_PASSKEY_CREATE
          ? ParsePasskeyCreationOptions(text, &ceremony->creation(), &error)
          : ParsePasskeyRequestOptions(text, &ceremony->request(), &error);
  if (!ok) {
    // Never took the slot; answer through Finish so the destructor stays
    // quiet. (ReleaseSlot is a no-op for a ceremony that doesn't hold it.)
    ceremony->Finish(PasskeyErrorEnvelope(error.code, error.message));
    return nullptr;
  }
  // The launch file pins the relying parties the app may use: on Windows
  // nothing else ties an RP ID to the app, so without this any code that can
  // reach passkey_request could ask for any site's credential.
  if (allowed_rp_ids) {
    const std::string& rp_id = ceremony->is_create()
                                   ? ceremony->creation().rp_id
                                   : ceremony->request().rp_id;
    if (!IsPasskeyRpIdAllowed(rp_id, *allowed_rp_ids)) {
      ceremony->Finish(PasskeyErrorEnvelope(
          kPasskeyInvalidRp,
          "the RP ID is not one of the app's passkeyRpIds (launch file)"));
      return nullptr;
    }
  }

  {
    std::lock_guard<std::mutex> lock(SlotMutex());
    if (g_slot_owner == nullptr) {
      g_slot_owner = ceremony.get();
      SlotWeak() = ceremony;
      return ceremony;
    }
  }
  ceremony->Finish(PasskeyErrorEnvelope(kPasskeyUnknown, kPasskeyBusyMessage));
  return nullptr;
}

void PasskeyWindowClosing(const void* window_key) {
  if (!window_key)
    return;
  std::shared_ptr<PasskeyCeremony> active;
  {
    std::lock_guard<std::mutex> lock(SlotMutex());
    active = SlotWeak().lock();
  }
  if (active && active->window_key() == window_key)
    active->Abort(kPasskeyCancelled, "the window was closed");
}

bool PasskeyBusyForTesting() {
  std::lock_guard<std::mutex> lock(SlotMutex());
  return g_slot_owner != nullptr;
}

}  // namespace laufey_common
