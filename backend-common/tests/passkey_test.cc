// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for laufey_passkey.h (the platform-independent half): the
// options parser against what @clerk/electron really sends, base64url,
// UTF-8 and RP ID checks, the envelopes, and the ceremony lifecycle (one at a
// time, exactly-once delivery, timeout, abort, window close). Plain asserts,
// no framework: run via `ctest --test-dir webview/build` (or cef/build).
// Exits non-zero on the first failure. No OS passkey API is touched.

#include "laufey_passkey.h"
#include "laufey_single_instance.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/json_reader.h"

using laufey_common::Base64UrlDecode;
using laufey_common::Base64UrlEncode;
using laufey_common::BuildPasskeyClientDataJson;
using laufey_common::IsPasskeyRpIdAllowed;
using laufey_common::IsValidPasskeyRpId;
using laufey_common::IsValidUtf8;
using laufey_common::ParsePasskeyCreationOptions;
using laufey_common::ParsePasskeyRequestOptions;
using laufey_common::PasskeyAssertionEnvelope;
using laufey_common::PasskeyAssertionResult;
using laufey_common::PasskeyBegin;
using laufey_common::PasskeyBeginWithRpIds;
using laufey_common::PasskeyBusyForTesting;
using laufey_common::PasskeyCeremony;
using laufey_common::PasskeyCreationOptions;
using laufey_common::PasskeyError;
using laufey_common::PasskeyErrorEnvelope;
using laufey_common::PasskeyRegistrationEnvelope;
using laufey_common::PasskeyRegistrationResult;
using laufey_common::PasskeyReportNotSupported;
using laufey_common::PasskeyRequestOptions;
using laufey_common::PasskeyWindowClosing;
using laufey_common::json::JsonReader;
using laufey_common::json::JsonValue;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

// What @clerk/electron 0.0.44 sends: produced by its own
// serializeCreationOptions / serializeRequestOptions (createPasskeys({mode:
// "native"}) over a stub bridge, then JSON.stringify as its main process
// does), byte for byte.
static const char kClerkCreate[] =
    R"({"rp":{"id":"clerk.example.com","name":"Example"},"user":{"id":"dXNlcl8yYWJjREVG","displayName":"Ada Lovelace","name":"ada@example.com"},"challenge":"-_8AAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHP8","pubKeyCredParams":[{"type":"public-key","alg":-7},{"type":"public-key","alg":-257}],"timeout":60000,"authenticatorSelection":{"authenticatorAttachment":"platform","residentKey":"required","requireResidentKey":true,"userVerification":"required"},"attestation":"none","excludeCredentials":[{"type":"public-key","id":"AQID-g","transports":["internal","hybrid"]}]})";
static const char kClerkGet[] =
    R"({"challenge":"CQgHBgUEAwIBAP7__T4_QA","rpId":"clerk.example.com","timeout":300000,"userVerification":"preferred","allowCredentials":[{"type":"public-key","id":"AQID-g"}]})";
// The same serializer when the RP left optional members undefined (they
// vanish in JSON.stringify) and gave no rp.id (`rp.id ?? ""`).
static const char kClerkCreateNoRpId[] =
    R"({"rp":{"id":"","name":"Example"},"user":{"id":"dXNlcl8yYWJjREVG","displayName":"Ada Lovelace","name":"ada@example.com"},"challenge":"AAECAwQFBgcICQoLDA0ODw","pubKeyCredParams":[{"type":"public-key","alg":-7}],"excludeCredentials":[]})";

static std::vector<uint8_t> Bytes(std::initializer_list<int> v) {
  std::vector<uint8_t> out;
  for (int b : v)
    out.push_back(static_cast<uint8_t>(b));
  return out;
}

static std::vector<uint8_t> Str(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

// --- base64url / UTF-8 / RP ID
// --------------------------------------------------

static void TestBase64Url() {
  // RFC 4648 section 10 vectors, unpadded.
  EXPECT(Base64UrlEncode(Str("")) == "");
  EXPECT(Base64UrlEncode(Str("f")) == "Zg");
  EXPECT(Base64UrlEncode(Str("fo")) == "Zm8");
  EXPECT(Base64UrlEncode(Str("foo")) == "Zm9v");
  EXPECT(Base64UrlEncode(Str("foob")) == "Zm9vYg");
  EXPECT(Base64UrlEncode(Str("fooba")) == "Zm9vYmE");
  EXPECT(Base64UrlEncode(Str("foobar")) == "Zm9vYmFy");
  // The URL alphabet: 0xfb 0xff -> "-_8", not "+/8".
  EXPECT(Base64UrlEncode(Bytes({0xfb, 0xff})) == "-_8");

  std::vector<uint8_t> out;
  EXPECT(Base64UrlDecode("Zm9vYmFy", &out) && out == Str("foobar"));
  EXPECT(Base64UrlDecode("Zm9vYg", &out) && out == Str("foob"));
  EXPECT(Base64UrlDecode("-_8", &out) && out == Bytes({0xfb, 0xff}));
  EXPECT(Base64UrlDecode("", &out) && out.empty());
  // Padding is tolerated, to a full quantum only.
  EXPECT(Base64UrlDecode("Zm9vYg==", &out) && out == Str("foob"));
  EXPECT(Base64UrlDecode("Zm8=", &out) && out == Str("fo"));
  EXPECT(!Base64UrlDecode("Zm8==", &out));
  EXPECT(!Base64UrlDecode("Zg=", &out));
  EXPECT(!Base64UrlDecode("Zg===", &out));
  // The standard alphabet, whitespace and stray characters are refused.
  EXPECT(!Base64UrlDecode("+/8", &out));
  EXPECT(!Base64UrlDecode("Zm9v Yg", &out));
  EXPECT(!Base64UrlDecode("Zm9v\n", &out));
  EXPECT(!Base64UrlDecode("Zm9v!", &out));
  // A length of 1 mod 4 can't be base64.
  EXPECT(!Base64UrlDecode("Zm9vY", &out));
  // Non-canonical: non-zero leftover bits ("Zh" would decode to 'f' too).
  EXPECT(!Base64UrlDecode("Zh", &out));
  EXPECT(!Base64UrlDecode("Zm9", &out));
  EXPECT(!Base64UrlDecode("Zm-", &out));

  // Round trip of every byte value.
  std::vector<uint8_t> all;
  for (int i = 0; i < 256; ++i)
    all.push_back(static_cast<uint8_t>(i));
  for (size_t n = 0; n <= all.size(); n += 37) {
    std::vector<uint8_t> in(all.begin(), all.begin() + n);
    EXPECT(Base64UrlDecode(Base64UrlEncode(in), &out) && out == in);
  }
}

static void TestRpId() {
  EXPECT(IsValidPasskeyRpId("example.com"));
  EXPECT(IsValidPasskeyRpId("clerk.example.com"));
  EXPECT(IsValidPasskeyRpId("localhost"));
  EXPECT(IsValidPasskeyRpId("a-b.c0.example"));
  EXPECT(IsValidPasskeyRpId("xn--bcher-kva.example"));

  EXPECT(!IsValidPasskeyRpId(""));
  EXPECT(!IsValidPasskeyRpId("Example.com"));  // lowercase only
  EXPECT(!IsValidPasskeyRpId("https://example.com"));
  EXPECT(!IsValidPasskeyRpId("example.com/"));
  EXPECT(!IsValidPasskeyRpId("example.com:443"));
  EXPECT(!IsValidPasskeyRpId("user@example.com"));
  EXPECT(!IsValidPasskeyRpId("exa mple.com"));
  EXPECT(!IsValidPasskeyRpId(".example.com"));
  EXPECT(!IsValidPasskeyRpId("example.com."));
  EXPECT(!IsValidPasskeyRpId("example..com"));
  EXPECT(!IsValidPasskeyRpId("-example.com"));
  EXPECT(!IsValidPasskeyRpId("example-.com"));
  EXPECT(!IsValidPasskeyRpId("127.0.0.1"));
  EXPECT(!IsValidPasskeyRpId("1234"));
  EXPECT(!IsValidPasskeyRpId("exa\"mple.com"));
  EXPECT(!IsValidPasskeyRpId(std::string("example.com\0x", 13)));
  EXPECT(
      !IsValidPasskeyRpId("b\xC3\xBC"
                          "cher.example"));  // not punycode
  EXPECT(!IsValidPasskeyRpId(std::string(64, 'a') + ".com"));
  EXPECT(IsValidPasskeyRpId(std::string(63, 'a') + ".com"));
  std::string long_name;
  while (long_name.size() < 250)
    long_name += "abcdefghi.";
  long_name += "com";  // 253
  EXPECT(long_name.size() == 253 && IsValidPasskeyRpId(long_name));
  EXPECT(!IsValidPasskeyRpId("a" + long_name));
}

// --- Parsing: the Clerk fixtures
// -----------------------------------------------

static void TestClerkCreationFixture() {
  PasskeyCreationOptions o;
  PasskeyError e;
  EXPECT(ParsePasskeyCreationOptions(kClerkCreate, &o, &e));
  EXPECT(o.rp_id == "clerk.example.com");
  EXPECT(o.rp_name == "Example");
  EXPECT(o.user_id == Str("user_2abcDEF"));
  EXPECT(o.user_name == "ada@example.com");
  EXPECT(o.user_display_name == "Ada Lovelace");
  EXPECT(o.challenge.size() == 32 && o.challenge[0] == 0xfb &&
         o.challenge[1] == 0xff && o.challenge[31] == 0xff);
  // Re-encoding reproduces exactly what Clerk sent.
  EXPECT(o.challenge_b64url == "-_8AAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHP8");
  EXPECT((o.algorithms == std::vector<int32_t>{-7, -257}));
  EXPECT(o.has_timeout && o.timeout_ms == 60000);
  EXPECT(o.authenticator_attachment == "platform");
  EXPECT(o.resident_key == "required");
  EXPECT(o.user_verification == "required");
  EXPECT(o.attestation == "none");
  EXPECT(o.exclude_credentials.size() == 1);
  EXPECT(o.exclude_credentials[0].id == Bytes({1, 2, 3, 250}));
  EXPECT((o.exclude_credentials[0].transports ==
          std::vector<std::string>{"internal", "hybrid"}));
}

static void TestClerkRequestFixture() {
  PasskeyRequestOptions o;
  PasskeyError e;
  EXPECT(ParsePasskeyRequestOptions(kClerkGet, &o, &e));
  EXPECT(o.rp_id == "clerk.example.com");
  EXPECT(o.challenge == Bytes({9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0xfe, 0xff, 0xfd,
                               0x3e, 0x3f, 0x40}));
  EXPECT(o.challenge_b64url == "CQgHBgUEAwIBAP7__T4_QA");
  EXPECT(o.has_timeout && o.timeout_ms == 300000);
  EXPECT(o.user_verification == "preferred");
  EXPECT(o.allow_credentials.size() == 1);
  EXPECT(o.allow_credentials[0].id == Bytes({1, 2, 3, 250}));
  // Clerk's serializer drops transports from allowCredentials.
  EXPECT(o.allow_credentials[0].transports.empty());
}

static void TestClerkCreationWithoutRpId() {
  PasskeyCreationOptions o;
  PasskeyError e;
  EXPECT(!ParsePasskeyCreationOptions(kClerkCreateNoRpId, &o, &e));
  EXPECT(e.code == "invalid_rp");
  // The same document with an RP ID is fine, and the absent members stay
  // absent.
  std::string fixed = kClerkCreateNoRpId;
  fixed.replace(fixed.find("\"id\":\"\""), 7, "\"id\":\"example.com\"");
  EXPECT(ParsePasskeyCreationOptions(fixed, &o, &e));
  EXPECT(!o.has_timeout);
  EXPECT(o.authenticator_attachment.empty() && o.resident_key.empty() &&
         o.user_verification.empty() && o.attestation.empty());
  EXPECT(o.exclude_credentials.empty());
}

// --- Parsing: strictness
// -------------------------------------------------------

static std::string CreateWith(const std::string& challenge,
                              const std::string& user_id,
                              const std::string& extra = "") {
  return R"({"rp":{"id":"example.com"},"user":{"id":")" + user_id +
         R"("},"challenge":")" + challenge + "\"" + extra + "}";
}

static bool CreateFails(const std::string& json, const char* code) {
  PasskeyCreationOptions o;
  PasskeyError e;
  if (ParsePasskeyCreationOptions(json, &o, &e))
    return false;
  return e.code == code;
}

static bool GetFails(const std::string& json, const char* code) {
  PasskeyRequestOptions o;
  PasskeyError e;
  if (ParsePasskeyRequestOptions(json, &o, &e))
    return false;
  return e.code == code;
}

static void TestStrictParsing() {
  const std::string ch = "AAECAwQFBgcICQoLDA0ODw";
  const std::string uid = "dXNlcg";
  PasskeyCreationOptions o;
  PasskeyRequestOptions r;
  PasskeyError e;
  EXPECT(ParsePasskeyCreationOptions(CreateWith(ch, uid), &o, &e));

  // Not a document / not an object / trailing garbage / too deep.
  EXPECT(CreateFails("", "unknown"));
  EXPECT(CreateFails("[]", "unknown"));
  EXPECT(CreateFails("null", "unknown"));
  EXPECT(CreateFails(CreateWith(ch, uid) + "x", "unknown"));
  std::string deep(40, '[');
  EXPECT(
      CreateFails(R"({"x":)" + deep + std::string(40, ']') + "}", "unknown"));
  // Not UTF-8 (a raw 0xFF byte inside a string).
  EXPECT(CreateFails(CreateWith(ch, uid, ",\"x\":\"\xFF\""), "unknown"));
  // Oversized.
  std::string big =
      CreateWith(ch, uid, ",\"pad\":\"" + std::string(70000, 'a') + "\"");
  EXPECT(CreateFails(big, "unknown"));

  // RP problems are invalid_rp.
  EXPECT(CreateFails(R"({"user":{"id":"dXNlcg"},"challenge":"AAAA"})",
                     "invalid_rp"));
  EXPECT(CreateFails(R"({"rp":"example.com","user":{"id":"dXNlcg"},)"
                     R"("challenge":"AAAA"})",
                     "invalid_rp"));
  EXPECT(CreateFails(R"({"rp":{"name":"x"},"user":{"id":"dXNlcg"},)"
                     R"("challenge":"AAAA"})",
                     "invalid_rp"));
  EXPECT(CreateFails(R"({"rp":{"id":"https://example.com"},)"
                     R"("user":{"id":"dXNlcg"},"challenge":"AAAA"})",
                     "invalid_rp"));
  EXPECT(CreateFails(R"({"rp":{"id":7},"user":{"id":"dXNlcg"},)"
                     R"("challenge":"AAAA"})",
                     "unknown"));  // a type error, not an RP decision
  EXPECT(GetFails(R"({"challenge":"AAAA"})", "invalid_rp"));
  EXPECT(GetFails(R"({"challenge":"AAAA","rpId":""})", "invalid_rp"));
  EXPECT(
      GetFails(R"({"challenge":"AAAA","rpId":"EXAMPLE.com"})", "invalid_rp"));

  // Challenge: required, base64url, non-empty.
  EXPECT(CreateFails(R"({"rp":{"id":"example.com"},"user":{"id":"dXNlcg"}})",
                     "unknown"));
  EXPECT(CreateFails(CreateWith("", uid), "unknown"));
  EXPECT(CreateFails(CreateWith("not base64!", uid), "unknown"));
  EXPECT(CreateFails(CreateWith("AAEC+/8", uid), "unknown"));
  EXPECT(GetFails(R"({"challenge":"*","rpId":"example.com"})", "unknown"));
  EXPECT(GetFails(R"({"challenge":12,"rpId":"example.com"})", "unknown"));

  // User: required object with a 1..64-byte id.
  EXPECT(CreateFails(R"({"rp":{"id":"example.com"},"challenge":"AAAA"})",
                     "unknown"));
  EXPECT(CreateFails(CreateWith(ch, ""), "unknown"));
  EXPECT(ParsePasskeyCreationOptions(
      CreateWith(ch, Base64UrlEncode(std::vector<uint8_t>(64, 7))), &o, &e));
  EXPECT(CreateFails(
      CreateWith(ch, Base64UrlEncode(std::vector<uint8_t>(65, 7))), "unknown"));
  EXPECT(CreateFails(R"({"rp":{"id":"example.com"},"user":{"id":"dXNlcg",)"
                     R"("name":5},"challenge":"AAAA"})",
                     "unknown"));

  // A key given twice.
  EXPECT(CreateFails(CreateWith(ch, uid, R"(,"challenge":"AAAA")"), "unknown"));

  // timeout: integer >= 0; clamped; null means absent.
  EXPECT(CreateFails(CreateWith(ch, uid, R"(,"timeout":-1)"), "unknown"));
  EXPECT(CreateFails(CreateWith(ch, uid, R"(,"timeout":1.5)"), "unknown"));
  EXPECT(CreateFails(CreateWith(ch, uid, R"(,"timeout":"60000")"), "unknown"));
  EXPECT(
      CreateFails(CreateWith(ch, uid, R"(,"timeout":4294967296)"), "unknown"));
  EXPECT(ParsePasskeyCreationOptions(CreateWith(ch, uid, R"(,"timeout":0)"), &o,
                                     &e) &&
         o.has_timeout && o.timeout_ms == 1000);
  EXPECT(ParsePasskeyCreationOptions(
             CreateWith(ch, uid, R"(,"timeout":99999999)"), &o, &e) &&
         o.timeout_ms == 600000);
  EXPECT(ParsePasskeyCreationOptions(CreateWith(ch, uid, R"(,"timeout":null)"),
                                     &o, &e) &&
         !o.has_timeout);

  // pubKeyCredParams: integers; foreign types skipped; none left is
  // not_supported; an empty list is fine.
  EXPECT(CreateFails(
      CreateWith(ch, uid, R"(,"pubKeyCredParams":[{"type":"public-key"}])"),
      "unknown"));
  EXPECT(CreateFails(
      CreateWith(ch, uid, R"(,"pubKeyCredParams":[{"alg":-7.5}])"), "unknown"));
  EXPECT(CreateFails(
      CreateWith(ch, uid, R"(,"pubKeyCredParams":[{"type":"x","alg":-7}])"),
      "not_supported"));
  EXPECT(ParsePasskeyCreationOptions(
             CreateWith(ch, uid,
                        R"(,"pubKeyCredParams":[{"type":"x","alg":-8},)"
                        R"({"alg":-7}])"),
             &o, &e) &&
         o.algorithms == std::vector<int32_t>{-7});
  EXPECT(ParsePasskeyCreationOptions(
             CreateWith(ch, uid, R"(,"pubKeyCredParams":[])"), &o, &e) &&
         o.algorithms.empty());

  // Enumerations: unknown values are ignored (WebAuthn), wrong types fail.
  EXPECT(ParsePasskeyCreationOptions(
             CreateWith(ch, uid,
                        R"(,"attestation":"bogus","authenticatorSelection":)"
                        R"({"authenticatorAttachment":"usb-thing",)"
                        R"("userVerification":"maybe"})"),
             &o, &e) &&
         o.attestation.empty() && o.authenticator_attachment.empty() &&
         o.user_verification.empty());
  EXPECT(CreateFails(CreateWith(ch, uid, R"(,"attestation":1)"), "unknown"));
  EXPECT(CreateFails(CreateWith(ch, uid, R"(,"authenticatorSelection":[])"),
                     "unknown"));
  // requireResidentKey counts only without residentKey.
  EXPECT(ParsePasskeyCreationOptions(
             CreateWith(ch, uid,
                        R"(,"authenticatorSelection":{"requireResidentKey":)"
                        R"(true})"),
             &o, &e) &&
         o.resident_key == "required");
  EXPECT(ParsePasskeyCreationOptions(
             CreateWith(ch, uid,
                        R"(,"authenticatorSelection":{"requireResidentKey":)"
                        R"(false})"),
             &o, &e) &&
         o.resident_key == "discouraged");
  EXPECT(ParsePasskeyCreationOptions(
             CreateWith(ch, uid,
                        R"(,"authenticatorSelection":{"residentKey":)"
                        R"("preferred","requireResidentKey":true})"),
             &o, &e) &&
         o.resident_key == "preferred");
  EXPECT(CreateFails(CreateWith(ch, uid,
                                R"(,"authenticatorSelection":)"
                                R"({"requireResidentKey":"yes"})"),
                     "unknown"));

  // Descriptors: id required and base64url; foreign types skipped; unknown
  // transports dropped; caps.
  EXPECT(CreateFails(CreateWith(ch, uid, R"(,"excludeCredentials":[{}])"),
                     "unknown"));
  EXPECT(
      CreateFails(CreateWith(ch, uid, R"(,"excludeCredentials":[{"id":"%%"}])"),
                  "unknown"));
  EXPECT(CreateFails(CreateWith(ch, uid, R"(,"excludeCredentials":{})"),
                     "unknown"));
  EXPECT(ParsePasskeyCreationOptions(
             CreateWith(ch, uid,
                        R"(,"excludeCredentials":[{"type":"other","id":)"
                        R"("AQ"},{"id":"Ag","transports":["usb","carrier-)"
                        R"(pigeon","nfc"]}])"),
             &o, &e) &&
         o.exclude_credentials.size() == 1 &&
         o.exclude_credentials[0].id == Bytes({2}) &&
         (o.exclude_credentials[0].transports ==
          std::vector<std::string>{"usb", "nfc"}));
  std::string many = R"(,"excludeCredentials":[)";
  for (int i = 0; i < 257; ++i)
    many += std::string(i ? "," : "") + R"({"id":"AQ"})";
  EXPECT(CreateFails(CreateWith(ch, uid, many + "]"), "unknown"));
  EXPECT(GetFails(R"({"challenge":"AAAA","rpId":"example.com",)"
                  R"("allowCredentials":[{"id":""}]})",
                  "unknown"));

  // Unknown members (extensions, hints, ...) are ignored.
  EXPECT(ParsePasskeyRequestOptions(
      R"({"challenge":"AAAA","rpId":"example.com","extensions":{"x":1},)"
      R"("hints":["client-device"]})",
      &r, &e));
}

// Error messages name the field, never echo what was in it.
static void TestMessagesDoNotLeakValues() {
  PasskeyCreationOptions o;
  PasskeyRequestOptions r;
  PasskeyError e;
  EXPECT(!ParsePasskeyRequestOptions(
      R"({"challenge":"SECRET-CHALLENGE!","rpId":"example.com"})", &r, &e));
  EXPECT(e.message.find("challenge") != std::string::npos);
  EXPECT(e.message.find("SECRET") == std::string::npos);
  std::string uid = Base64UrlEncode(std::vector<uint8_t>(80, 0x41));
  EXPECT(!ParsePasskeyCreationOptions(CreateWith("AAAA", uid), &o, &e));
  EXPECT(e.message.find("user.id") != std::string::npos);
  EXPECT(e.message.find(uid.substr(0, 10)) == std::string::npos);
  EXPECT(!ParsePasskeyRequestOptions(
      R"({"challenge":"AAAA","rpId":"evil.example/phish"})", &r, &e));
  EXPECT(e.message.find("phish") == std::string::npos);
}

// --- Envelopes
// ----------------------------------------------------------------

static JsonValue ParseJson(const std::string& text) {
  JsonValue v;
  std::string error;
  EXPECT(IsValidUtf8(text));
  EXPECT(JsonReader(text).ParseDocument(&v, &error));
  return v;
}

static const JsonValue* Get(const JsonValue& obj, const char* key) {
  for (const auto& m : obj.object) {
    if (m.first == key)
      return &m.second;
  }
  return nullptr;
}

static std::vector<uint8_t> GetB64(const JsonValue& obj, const char* key) {
  const JsonValue* v = Get(obj, key);
  EXPECT(v && v->type == JsonValue::Type::kString);
  std::vector<uint8_t> out;
  EXPECT(Base64UrlDecode(v->string, &out));
  return out;
}

static void TestEnvelopes() {
  // Exact bytes: what @clerk/electron's deserializeCreationResponse reads
  // (id, rawId, type, authenticatorAttachment, response.clientDataJSON /
  // attestationObject / transports).
  PasskeyRegistrationResult reg;
  reg.credential_id = Bytes({1, 2, 3, 250});
  reg.client_data_json = Str("{}");
  reg.attestation_object = Bytes({0xa0});
  reg.attachment = "platform";
  reg.transports = {"internal", "hybrid"};
  EXPECT(PasskeyRegistrationEnvelope(reg) ==
         R"({"ok":true,"credential":{"id":"AQID-g","rawId":"AQID-g",)"
         R"("type":"public-key","authenticatorAttachment":"platform",)"
         R"("response":{"clientDataJSON":"e30","attestationObject":"oA",)"
         R"("transports":["internal","hybrid"]}}})");

  PasskeyAssertionResult as;
  as.credential_id = Bytes({1, 2, 3, 250});
  as.client_data_json = Str("{}");
  as.authenticator_data = Bytes({0, 1});
  as.signature = Bytes({0x30, 0x45});
  as.attachment = "cross-platform";
  // No user handle: the member is left out (the reader maps it to null).
  EXPECT(PasskeyAssertionEnvelope(as) ==
         R"({"ok":true,"credential":{"id":"AQID-g","rawId":"AQID-g",)"
         R"("type":"public-key","authenticatorAttachment":"cross-platform",)"
         R"("response":{"clientDataJSON":"e30","authenticatorData":"AAE",)"
         R"("signature":"MEU"}}})");
  as.user_handle = Str("user_2abcDEF");
  as.attachment.clear();  // unknown attachment: left out too
  std::string with_handle = PasskeyAssertionEnvelope(as);
  JsonValue v = ParseJson(with_handle);
  const JsonValue* cred = Get(v, "credential");
  EXPECT(cred && !Get(*cred, "authenticatorAttachment"));
  const JsonValue* resp = Get(*cred, "response");
  EXPECT(resp && GetB64(*resp, "userHandle") == Str("user_2abcDEF"));
  EXPECT(GetB64(*resp, "signature") == Bytes({0x30, 0x45}));

  // Errors: quotes, backslashes, control characters, invalid UTF-8.
  EXPECT(PasskeyErrorEnvelope("cancelled", "The user canceled.") ==
         R"({"ok":false,"error":{"code":"cancelled",)"
         R"("message":"The user canceled."}})");
  std::string tricky =
      PasskeyErrorEnvelope("unknown", std::string("a\"b\\c\nd\x01"
                                                  "e\xFF"
                                                  "f\xC3\xA9",
                                                  13));
  JsonValue t = ParseJson(tricky);
  const JsonValue* err = Get(t, "error");
  EXPECT(err && Get(*err, "message")->string == std::string("a\"b\\c\nd\x01"
                                                            "e\xEF\xBF\xBD"
                                                            "f\xC3\xA9"));
  const JsonValue* ok = Get(t, "ok");
  EXPECT(ok && ok->type == JsonValue::Type::kBool && !ok->boolean);
}

static void TestClientDataJson() {
  EXPECT(BuildPasskeyClientDataJson("webauthn.create",
                                    "-_8AAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRo"
                                    "bHP8",
                                    "clerk.example.com") ==
         R"({"type":"webauthn.create","challenge":)"
         R"("-_8AAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHP8",)"
         R"("origin":"https://clerk.example.com","crossOrigin":false})");
  EXPECT(BuildPasskeyClientDataJson("webauthn.get", "AAAA", "example.com") ==
         R"({"type":"webauthn.get","challenge":"AAAA",)"
         R"("origin":"https://example.com","crossOrigin":false})");
}

// The full round trip: Clerk's options in, the challenge into our
// clientDataJSON, a credential out that decodes back to the same bytes.
static void TestClerkRoundTrip() {
  PasskeyRequestOptions o;
  PasskeyError e;
  EXPECT(ParsePasskeyRequestOptions(kClerkGet, &o, &e));
  std::string cdj =
      BuildPasskeyClientDataJson("webauthn.get", o.challenge_b64url, o.rp_id);
  PasskeyAssertionResult as;
  as.credential_id = o.allow_credentials[0].id;
  as.client_data_json = Str(cdj);
  as.authenticator_data = Bytes({1});
  as.signature = Bytes({2});
  as.user_handle = Str("user_2abcDEF");
  as.attachment = "platform";
  JsonValue v = ParseJson(PasskeyAssertionEnvelope(as));
  const JsonValue* cred = Get(v, "credential");
  EXPECT(Get(*cred, "id")->string == "AQID-g");
  JsonValue client = ParseJson([&] {
    auto b = GetB64(*Get(*cred, "response"), "clientDataJSON");
    return std::string(b.begin(), b.end());
  }());
  EXPECT(Get(client, "challenge")->string == "CQgHBgUEAwIBAP7__T4_QA");
  EXPECT(Get(client, "origin")->string == "https://clerk.example.com");
  EXPECT(Get(client, "type")->string == "webauthn.get");
}

// --- Ceremonies
// -----------------------------------------------------------------

struct Recorder {
  std::mutex mutex;
  int count = 0;
  std::string last;
};

static void Record(void* user_data, const char* json) {
  auto* r = static_cast<Recorder*>(user_data);
  std::lock_guard<std::mutex> lock(r->mutex);
  ++r->count;
  r->last = json ? json : "";
}

static int CountOf(Recorder& r) {
  std::lock_guard<std::mutex> lock(r.mutex);
  return r.count;
}

static std::string LastOf(Recorder& r) {
  std::lock_guard<std::mutex> lock(r.mutex);
  return r.last;
}

static bool HasCode(const std::string& envelope, const char* code) {
  return envelope.find(std::string("\"code\":\"") + code + "\"") !=
         std::string::npos;
}

// webview/src/runtime_loader.h answers with this literal where
// backend-common isn't linked (iOS); it must stay the same text as
// PasskeyReportNotSupported, which also matches @clerk/electron-passkeys'
// own "not supported" answer.
static void TestNotSupportedText() {
  Recorder r;
  PasskeyReportNotSupported(Record, &r);
  EXPECT(CountOf(r) == 1);
  EXPECT(
      LastOf(r) ==
      R"({"ok":false,"error":{"code":"not_supported",)"
      R"("message":"Native passkeys are not supported on this platform."}})");
  PasskeyReportNotSupported(nullptr, nullptr);  // no-op
}

static void TestBeginRefusals() {
  EXPECT(!PasskeyBusyForTesting());
  // NULL callback: nothing to answer, nothing taken.
  EXPECT(!PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, nullptr, nullptr));
  EXPECT(!PasskeyBusyForTesting());

  Recorder r;
  EXPECT(!PasskeyBegin(7, kClerkGet, Record, &r));
  EXPECT(CountOf(r) == 1 && HasCode(LastOf(r), "unknown"));
  EXPECT(!PasskeyBegin(LAUFEY_PASSKEY_GET, nullptr, Record, &r));
  EXPECT(CountOf(r) == 2 && HasCode(LastOf(r), "unknown"));
  // The kinds are not interchangeable.
  EXPECT(!PasskeyBegin(LAUFEY_PASSKEY_CREATE, kClerkGet, Record, &r));
  EXPECT(CountOf(r) == 3 && HasCode(LastOf(r), "invalid_rp"));
  // Oversized, without reading past the cap.
  std::string big(LAUFEY_PASSKEY_MAX_OPTIONS_BYTES + 1, ' ');
  EXPECT(!PasskeyBegin(LAUFEY_PASSKEY_GET, big.c_str(), Record, &r));
  EXPECT(CountOf(r) == 4 && HasCode(LastOf(r), "unknown"));
  EXPECT(!PasskeyBusyForTesting());
}

// The launch file's "passkeyRpIds" pins the relying parties: a ceremony for
// any other RP ID is refused with invalid_rp before it takes the slot (so
// before any OS UI), matching case-insensitively; without the key, any RP ID
// the parser accepts goes through.
static void TestRpIdPin() {
  const std::vector<std::string> allowed = {"example.com", "clerk.example.com"};
  EXPECT(IsPasskeyRpIdAllowed("clerk.example.com", allowed));
  EXPECT(IsPasskeyRpIdAllowed("Clerk.Example.COM", allowed));
  EXPECT(IsPasskeyRpIdAllowed("example.com", {"EXAMPLE.com"}));
  // Exact names only: no subdomains, suffixes or registrable-domain logic.
  EXPECT(!IsPasskeyRpIdAllowed("evil.example.com", allowed));
  EXPECT(!IsPasskeyRpIdAllowed("example.co", allowed));
  EXPECT(!IsPasskeyRpIdAllowed("example.com.evil", allowed));
  EXPECT(!IsPasskeyRpIdAllowed("", allowed));
  EXPECT(!IsPasskeyRpIdAllowed("example.com", {}));

  Recorder r;
  // Listed: the ceremony starts (and holds the slot) as without a pin.
  auto c = PasskeyBeginWithRpIds(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r,
                                 &allowed);
  EXPECT(c && PasskeyBusyForTesting() && CountOf(r) == 0);
  c->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  EXPECT(CountOf(r) == 1 && !PasskeyBusyForTesting());
  c.reset();
  c = PasskeyBeginWithRpIds(LAUFEY_PASSKEY_CREATE, kClerkCreate, Record, &r,
                            &allowed);
  EXPECT(c && c->creation().rp_id == "clerk.example.com");
  c->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  EXPECT(CountOf(r) == 2);
  c.reset();

  // Not listed: refused synchronously with invalid_rp, nothing taken. Both
  // kinds (rp.id for a registration, rpId for an authentication).
  const std::vector<std::string> other = {"example.org"};
  EXPECT(!PasskeyBeginWithRpIds(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r,
                                &other));
  EXPECT(CountOf(r) == 3 && HasCode(LastOf(r), "invalid_rp"));
  EXPECT(LastOf(r).find("passkeyRpIds") != std::string::npos);
  // The message names the key, never the RP ID asked for.
  EXPECT(LastOf(r).find("clerk.example.com") == std::string::npos);
  EXPECT(!PasskeyBusyForTesting());
  EXPECT(!PasskeyBeginWithRpIds(LAUFEY_PASSKEY_CREATE, kClerkCreate, Record, &r,
                                &other));
  EXPECT(CountOf(r) == 4 && HasCode(LastOf(r), "invalid_rp"));
  // An empty list (the key with no valid entry) refuses every RP ID.
  const std::vector<std::string> none;
  EXPECT(
      !PasskeyBeginWithRpIds(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r, &none));
  EXPECT(CountOf(r) == 5 && HasCode(LastOf(r), "invalid_rp"));
  EXPECT(!PasskeyBusyForTesting());
  // Malformed options are still answered by the parser first.
  EXPECT(!PasskeyBeginWithRpIds(LAUFEY_PASSKEY_GET, R"({"rpId":"example.org"})",
                                Record, &r, &other));
  EXPECT(CountOf(r) == 6 && HasCode(LastOf(r), "unknown"));

  // No key (null): any valid RP ID goes through.
  c = PasskeyBeginWithRpIds(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r, nullptr);
  EXPECT(c && PasskeyBusyForTesting());
  c->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  EXPECT(CountOf(r) == 7 && !PasskeyBusyForTesting());
}

static void TestOneAtATime() {
  Recorder first, second;
  auto c = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &first);
  EXPECT(c && c->kind() == LAUFEY_PASSKEY_GET);
  EXPECT(c->request().rp_id == "clerk.example.com");
  EXPECT(c->has_timeout() && c->timeout_ms() == 300000);
  EXPECT(PasskeyBusyForTesting());
  EXPECT(CountOf(first) == 0);

  // A second request, of either kind, is refused synchronously.
  EXPECT(!PasskeyBegin(LAUFEY_PASSKEY_CREATE, kClerkCreate, Record, &second));
  EXPECT(CountOf(second) == 1 && HasCode(LastOf(second), "unknown"));
  EXPECT(LastOf(second).find("already in progress") != std::string::npos);
  // ...but a malformed one still gets its own error.
  EXPECT(!PasskeyBegin(LAUFEY_PASSKEY_GET, "{}", Record, &second));
  EXPECT(CountOf(second) == 2 && HasCode(LastOf(second), "invalid_rp"));

  c->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  EXPECT(CountOf(first) == 1 && HasCode(LastOf(first), "cancelled"));
  EXPECT(!PasskeyBusyForTesting());
  // Exactly once: later results are dropped.
  c->Finish(PasskeyErrorEnvelope("unknown", "late"));
  c->Abort("timeout", "late");
  EXPECT(CountOf(first) == 1);
  c.reset();
  EXPECT(CountOf(first) == 1);
}

// The slot is free by the time the callback runs, so the caller can start
// the next request from inside it.
struct Chain {
  Recorder inner;
  std::shared_ptr<PasskeyCeremony> next;
};
static void StartNext(void* user_data, const char*) {
  auto* chain = static_cast<Chain*>(user_data);
  chain->next =
      PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &chain->inner);
}

static void TestRequestFromCallback() {
  Chain chain;
  auto c = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, StartNext, &chain);
  EXPECT(c);
  c->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  EXPECT(chain.next && CountOf(chain.inner) == 0);
  chain.next->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  EXPECT(CountOf(chain.inner) == 1);
  EXPECT(!PasskeyBusyForTesting());
}

static void TestAbortKeepsSlotUntilFinish() {
  Recorder r;
  std::atomic<int> cancels{0};
  auto c = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r);
  EXPECT(c);
  c->SetCanceller([&] { ++cancels; });
  c->Abort("timeout", "the passkey request timed out");
  EXPECT(CountOf(r) == 1 && HasCode(LastOf(r), "timeout"));
  EXPECT(cancels == 1);
  // The OS hasn't reported back yet: the slot is still held.
  EXPECT(PasskeyBusyForTesting());
  Recorder other;
  EXPECT(!PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &other));
  EXPECT(HasCode(LastOf(other), "unknown"));
  c->Abort("cancelled", "again");  // no second delivery, no second cancel
  // The OS answers (cancelled): the slot frees, nothing more is delivered.
  c->Finish(PasskeyErrorEnvelope("cancelled", "The operation was canceled."));
  EXPECT(CountOf(r) == 1 && HasCode(LastOf(r), "timeout"));
  EXPECT(cancels == 1);
  EXPECT(!PasskeyBusyForTesting());
}

static void TestCancellerInstalledAfterAbort() {
  Recorder r;
  std::atomic<int> cancels{0};
  auto c = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r);
  c->Abort("timeout", "t");
  c->SetCanceller([&] { ++cancels; });  // runs at once
  EXPECT(cancels == 1);
  c->SetCanceller([&] { ++cancels; });  // never twice
  EXPECT(cancels == 1);
  c->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  // After Finish a canceller is never run.
  c->SetCanceller([&] { ++cancels; });
  EXPECT(cancels == 1 && CountOf(r) == 1);
}

// An OS that drops the cancels which arrive before its operation is
// registered (Windows WebAuthn answers them S_OK and the call goes on, past
// its own timeout when nobody is at the dialog): a canceller installed with a
// repeat interval runs again until the OS reports back, so the slot frees.
// Here the first cancel is dropped and the second one ends the operation.
static void TestDroppedCancelIsRepeated() {
  Recorder r;
  std::atomic<int> cancels{0};
  auto c = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r);
  EXPECT(c);
  std::weak_ptr<PasskeyCeremony> weak = c;
  // The "OS": a thread that ends the operation once a cancel reaches it
  // after the first.
  std::thread os([weak, &cancels] {
    for (int i = 0; i < 400; ++i) {
      if (cancels.load() >= 2) {
        if (auto self = weak.lock())
          self->Finish(PasskeyErrorEnvelope("cancelled", "x"));
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  c->SetCanceller([&] { ++cancels; }, 20);
  c->StartTimeout(50);
  for (int i = 0; i < 300 && PasskeyBusyForTesting(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  os.join();
  EXPECT(CountOf(r) == 1 && HasCode(LastOf(r), "timeout"));
  EXPECT(cancels >= 2);
  EXPECT(!PasskeyBusyForTesting());
  // Once the OS has reported back, the repeats stop (one may have been on
  // its way while it did).
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  int after = cancels.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT(cancels.load() == after);

  // Installed after the abort (the OS call started late): the same.
  Recorder r2;
  std::atomic<int> cancels2{0};
  auto c2 = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r2);
  EXPECT(c2);
  c2->Abort("timeout", "t");
  c2->SetCanceller([&] { ++cancels2; }, 20);
  for (int i = 0; i < 300 && cancels2.load() < 3; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT(cancels2 >= 3 && PasskeyBusyForTesting());
  c2->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  EXPECT(!PasskeyBusyForTesting());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  after = cancels2.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT(cancels2.load() == after && CountOf(r2) == 1);
}

static void TestTimeout() {
  Recorder r;
  std::atomic<int> cancels{0};
  auto c = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r);
  c->SetCanceller([&] { ++cancels; });
  c->StartTimeout(50);
  for (int i = 0; i < 200 && CountOf(r) == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT(CountOf(r) == 1 && HasCode(LastOf(r), "timeout"));
  EXPECT(cancels == 1);
  c->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  EXPECT(CountOf(r) == 1);
  EXPECT(!PasskeyBusyForTesting());

  // A result before the deadline wins; the timer then does nothing.
  Recorder r2;
  auto c2 = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r2);
  c2->StartTimeout(100);
  c2->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT(CountOf(r2) == 1 && HasCode(LastOf(r2), "cancelled"));
}

// The timeout thread and the OS's answer race on every iteration: exactly
// one answer is delivered, the slot frees, and nothing touches the ceremony
// after its last reference is gone. Finish / Abort notify cv_ after
// unlocking, unlike the stack-frame rendezvous of laufey_sync_call.h, and
// that is safe here only because cv_ is a member of a heap object that every
// notifier holds a reference to (the timeout thread its own `self`, a backend
// its shared_ptr), so the waiter returning can't destroy it. The `tsan` CI
// job runs this under ThreadSanitizer to keep it that way.
static void TestTimeoutRacesFinish() {
  constexpr int kRounds = 400;
  // Kept alive to the end: a losing timeout thread may still be returning
  // from its (dropped) Abort when the round ends.
  static Recorder recorders[kRounds];
  for (int i = 0; i < kRounds; ++i) {
    Recorder& r = recorders[i];
    auto c = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r);
    EXPECT(c);
    c->SetCanceller([] {});
    c->StartTimeout(static_cast<uint32_t>(i % 3));
    // The OS answers from another thread, which then drops its reference;
    // ours is dropped first on odd rounds, so either side can be the last.
    std::thread os([c, i]() mutable {
      if (i % 4 == 0)
        std::this_thread::sleep_for(std::chrono::microseconds(i % 7 * 100));
      c->Finish(PasskeyErrorEnvelope("cancelled", "x"));
      c.reset();
    });
    if (i % 2)
      c.reset();
    os.join();
    c.reset();
    for (int k = 0; k < 500 && CountOf(r) == 0; ++k)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT(CountOf(r) == 1);
    EXPECT(HasCode(LastOf(r), "cancelled") || HasCode(LastOf(r), "timeout"));
    // Finish ran on every round, so the slot is free for the next one.
    EXPECT(!PasskeyBusyForTesting());
  }
  // Let the last timeout threads exit before the recorders' lifetime ends.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  for (int i = 0; i < kRounds; ++i)
    EXPECT(CountOf(recorders[i]) == 1);
}

static void TestWindowClosing() {
  Recorder r;
  int a = 0, b = 0;
  auto c = PasskeyBegin(LAUFEY_PASSKEY_CREATE, kClerkCreate, Record, &r);
  EXPECT(c && c->creation().rp_id == "clerk.example.com");
  c->SetWindowKey(&a);
  PasskeyWindowClosing(&b);  // another window: ignored
  PasskeyWindowClosing(nullptr);
  EXPECT(CountOf(r) == 0);
  PasskeyWindowClosing(&a);
  EXPECT(CountOf(r) == 1 && HasCode(LastOf(r), "cancelled"));
  c->Finish(PasskeyErrorEnvelope("cancelled", "x"));
  EXPECT(CountOf(r) == 1 && !PasskeyBusyForTesting());
}

static void TestDroppedCeremonyStillAnswers() {
  Recorder r;
  {
    auto c = PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &r);
    EXPECT(c && PasskeyBusyForTesting());
  }  // dropped without Finish (a backend bug)
  EXPECT(CountOf(r) == 1 && HasCode(LastOf(r), "unknown"));
  EXPECT(!PasskeyBusyForTesting());
}

static void TestConcurrentBegin() {
  constexpr int kThreads = 16;
  Recorder recorders[kThreads];
  std::shared_ptr<PasskeyCeremony> got[kThreads];
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      while (!go.load()) {
      }
      got[i] =
          PasskeyBegin(LAUFEY_PASSKEY_GET, kClerkGet, Record, &recorders[i]);
    });
  }
  go = true;
  for (auto& t : threads)
    t.join();
  int winners = 0;
  for (int i = 0; i < kThreads; ++i) {
    if (got[i]) {
      ++winners;
      EXPECT(CountOf(recorders[i]) == 0);
      got[i]->Finish(PasskeyErrorEnvelope("cancelled", "x"));
    } else {
      EXPECT(CountOf(recorders[i]) == 1);
      EXPECT(LastOf(recorders[i]).find("already in progress") !=
             std::string::npos);
    }
    EXPECT(CountOf(recorders[i]) == 1);
  }
  EXPECT(winners == 1);
  EXPECT(!PasskeyBusyForTesting());
}

int main() {
  TestBase64Url();
  TestRpId();
  TestClerkCreationFixture();
  TestClerkRequestFixture();
  TestClerkCreationWithoutRpId();
  TestStrictParsing();
  TestMessagesDoNotLeakValues();
  TestEnvelopes();
  TestClientDataJson();
  TestClerkRoundTrip();
  TestNotSupportedText();
  TestBeginRefusals();
  TestRpIdPin();
  TestOneAtATime();
  TestRequestFromCallback();
  TestAbortKeepsSlotUntilFinish();
  TestCancellerInstalledAfterAbort();
  TestTimeout();
  TestDroppedCancelIsRepeated();
  TestTimeoutRacesFinish();
  TestWindowClosing();
  TestDroppedCeremonyStillAnswers();
  TestConcurrentBegin();
  std::printf("passkey_test: all tests passed\n");
  return 0;
}
