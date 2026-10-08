// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_bridge_origin.h: the origin every bridge call carries (API 44) and
// the launch file's "bridgeOrigins" pin. Plain strings, no engine. Plain
// asserts, no framework.

#include "laufey_bridge_origin.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;

static BridgeOriginPolicy PolicyOf(const std::string& launch_json) {
  std::vector<std::string> warnings;
  return BridgeOriginPolicyFrom(ParseLaunchConfig(launch_json, &warnings));
}

int main() {
  // --- Serialization -------------------------------------------------------
  EXPECT(OriginOfUrl("myapp://app/index.html") == "myapp://app");
  EXPECT(OriginOfUrl("MyApp://APP/x?y#z") == "myapp://app");
  EXPECT(OriginOfUrl("app://localhost") == "app://localhost");
  EXPECT(OriginOfUrl("https://example.com:443/a") == "https://example.com");
  EXPECT(OriginOfUrl("https://example.com:8443/a") ==
         "https://example.com:8443");
  EXPECT(OriginOfUrl("http://127.0.0.1:80/") == "http://127.0.0.1");
  EXPECT(OriginOfUrl("http://127.0.0.1:5173/") == "http://127.0.0.1:5173");
  EXPECT(OriginOfUrl("http://user:pw@evil.com@example.com/") ==
         "http://example.com");
  EXPECT(OriginOfUrl("http://[::1]:8080/") == "http://[::1]:8080");
  EXPECT(OriginOfUrl("myapp://app:0/") == "myapp://app:0");
  EXPECT(OriginOfUrl("blob:myapp://app/1234") == "myapp://app");
  for (const char* opaque : {
           "about:blank",
           "about:srcdoc",
           "data:text/html,hi",
           "file:///etc/passwd",
           "javascript:alert(1)",
           "",
           "noscheme",
           "1http://x/",
           "http:example.com",
           "http://",
           "http://:80/",
           "http://host:99999/",
           "http://host:8x/",
           "http://[::1/",
           "http://a\"b/",
       }) {
    if (OriginOfUrl(opaque) != kOpaqueOrigin) {
      std::fprintf(stderr, "not opaque: %s -> %s\n", opaque,
                   OriginOfUrl(opaque).c_str());
      std::exit(1);
    }
  }
  EXPECT(SerializeOrigin("MYAPP", "App", -1) == "myapp://app");
  EXPECT(SerializeOrigin("https", "example.com", 443) == "https://example.com");
  EXPECT(SerializeOrigin("https", "example.com", 0) == "https://example.com:0");
  EXPECT(SerializeOrigin("file", "", -1) == kOpaqueOrigin);

  // --- Policy: no launch file / no pin -> every origin -----------------------
  {
    BridgeOriginPolicy none = PolicyOf("{}");
    EXPECT(!none.restricted);
    EXPECT(BridgeOriginAllowed(none, "https://evil.example"));
    EXPECT(BridgeOriginAllowed(none, kOpaqueOrigin));
    EXPECT(BridgeOriginGuardJs(none) == "true");
    EXPECT(!PolicyOf(R"({"appId":"com.example.a"})").restricted);
  }

  // --- Default: the launch file's customSchemes ------------------------------
  {
    BridgeOriginPolicy p = PolicyOf(R"({"customSchemes":["myapp","Other"]})");
    EXPECT(p.restricted);
    EXPECT(BridgeOriginAllowed(p, "myapp://app"));
    EXPECT(BridgeOriginAllowed(p, "other://x"));
    EXPECT(!BridgeOriginAllowed(p, "myapp:/"));
    EXPECT(!BridgeOriginAllowed(p, "myappx://app"));
    EXPECT(!BridgeOriginAllowed(p, "https://evil.example"));
    EXPECT(!BridgeOriginAllowed(p, "http://127.0.0.1:5173"));
    EXPECT(!BridgeOriginAllowed(p, kOpaqueOrigin));
    EXPECT(!BridgeOriginAllowed(p, "app://localhost"));
  }

  // --- Explicit bridgeOrigins wins over customSchemes ------------------------
  {
    BridgeOriginPolicy p =
        PolicyOf(R"({"customSchemes":["myapp"],)"
                 R"("bridgeOrigins":["myapp://app","https://Example.com:443/",)"
                 R"("dev://*","http://127.0.0.1:5173"]})");
    EXPECT(p.restricted);
    EXPECT(BridgeOriginAllowed(p, "myapp://app"));
    EXPECT(!BridgeOriginAllowed(p, "myapp://other"));
    EXPECT(BridgeOriginAllowed(p, "https://example.com"));
    EXPECT(BridgeOriginAllowed(p, "dev://anything"));
    EXPECT(BridgeOriginAllowed(p, "http://127.0.0.1:5173"));
    EXPECT(!BridgeOriginAllowed(p, "http://127.0.0.1:5174"));
  }
  {
    // "*" lifts the pin; invalid entries are skipped but the key still
    // restricts (an app that pins its origins never serves every origin).
    EXPECT(!PolicyOf(R"({"customSchemes":["myapp"],"bridgeOrigins":["*"]})")
                .restricted);
    std::vector<std::string> warnings;
    LaunchConfig c = ParseLaunchConfig(
        R"({"bridgeOrigins":["https://a.example/path","", 5,"*x",)"
        R"J("javascript:alert(1)","myapp://app"]})J",
        &warnings);
    EXPECT(c.has_bridge_origins);
    EXPECT(c.bridge_origins.size() == 1);
    EXPECT(warnings.size() == 5);
    BridgeOriginPolicy p = BridgeOriginPolicyFrom(c);
    EXPECT(p.restricted && BridgeOriginAllowed(p, "myapp://app"));
    EXPECT(!BridgeOriginAllowed(p, "https://a.example"));
    BridgeOriginPolicy empty = PolicyOf(R"({"bridgeOrigins":[]})");
    EXPECT(empty.restricted);
    EXPECT(!BridgeOriginAllowed(empty, "myapp://app"));
    std::vector<std::string> w2;
    LaunchConfig bad = ParseLaunchConfig(R"({"bridgeOrigins":"x"})", &w2);
    EXPECT(!bad.has_bridge_origins && w2.size() == 1);
  }

  // --- The page-side guard ---------------------------------------------------
  {
    BridgeOriginPolicy p =
        PolicyOf(R"({"bridgeOrigins":["myapp://app","dev://*"]})");
    std::string js = BridgeOriginGuardJs(p);
    EXPECT(js.find("o==='myapp://app'") != std::string::npos);
    EXPECT(js.find("'dev://'") != std::string::npos);
    EXPECT(js.find("location.origin") != std::string::npos);
    EXPECT(js.find('"') == std::string::npos);
  }

  std::printf("bridge_origin_test: ok\n");
  return 0;
}
