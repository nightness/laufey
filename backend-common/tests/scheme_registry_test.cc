// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for laufey_scheme_registry.h. Plain asserts, no framework: run
// via `ctest --test-dir webview/build` (or cef/build). Exits non-zero on the
// first failure.

#include "laufey_scheme_registry.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using laufey_common::DecideLocalNetworkPrompt;
using laufey_common::IsLocalNetworkTrustedOrigin;
using laufey_common::IsValidSchemeName;
using laufey_common::JoinForwardedSchemes;
using laufey_common::LocalNetworkPromptDecision;
using laufey_common::MergeForwardedSchemes;
using laufey_common::MergeSchemeLists;
using laufey_common::NormalizeSchemeName;
using laufey_common::OriginScheme;
using laufey_common::ParseSchemeList;
using laufey_common::SchemeRegistry;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static void TestSchemeNameGrammar() {
  EXPECT(IsValidSchemeName("app"));
  EXPECT(IsValidSchemeName("t3code"));
  EXPECT(IsValidSchemeName("my-app.v2+x"));
  EXPECT(IsValidSchemeName("A"));
  EXPECT(IsValidSchemeName("MyApp"));

  EXPECT(!IsValidSchemeName(""));
  EXPECT(!IsValidSchemeName("1app"));  // must start with a letter
  EXPECT(!IsValidSchemeName("-app"));
  EXPECT(!IsValidSchemeName("app://"));  // name only, no separator
  EXPECT(!IsValidSchemeName("my app"));
  EXPECT(!IsValidSchemeName("my_app"));  // "_" is not in the RFC grammar
  EXPECT(!IsValidSchemeName("app/x"));
  EXPECT(!IsValidSchemeName(std::string("app\0x", 5)));

  EXPECT(NormalizeSchemeName("MyApp") == "myapp");
  EXPECT(NormalizeSchemeName("app") == "app");
  EXPECT(NormalizeSchemeName("T3-Code.X+Y") == "t3-code.x+y");
}

static void TestRegistryDefaults() {
  SchemeRegistry registry;
  EXPECT(registry.size() == 1);
  EXPECT(registry.Contains("app"));
  EXPECT(registry.Contains("APP"));
  EXPECT(!registry.Contains("t3code"));
  std::vector<std::string> snapshot = registry.Snapshot();
  EXPECT(snapshot.size() == 1);
  EXPECT(snapshot[0] == "app");
}

static void TestRegistryAdd() {
  SchemeRegistry registry;

  // "app" is pre-registered: re-adding it is a no-op.
  EXPECT(!registry.Add("app"));
  EXPECT(!registry.Add("APP"));
  EXPECT(registry.size() == 1);

  // A new scheme reports true once, then false (already present), and is
  // stored lowercase regardless of how it was spelled.
  EXPECT(registry.Add("t3code"));
  EXPECT(!registry.Add("t3code"));
  EXPECT(!registry.Add("T3Code"));
  EXPECT(registry.Contains("t3code"));
  EXPECT(registry.Contains("T3CODE"));

  // Invalid names are rejected without being stored.
  EXPECT(!registry.Add(""));
  EXPECT(!registry.Add("bad scheme"));
  EXPECT(!registry.Add("app://"));
  EXPECT(!registry.Contains("bad scheme"));

  // Snapshot keeps "app" first and then registration order.
  EXPECT(registry.Add("Zeta"));
  EXPECT(registry.Add("alpha"));
  std::vector<std::string> snapshot = registry.Snapshot();
  EXPECT(snapshot.size() == 4);
  EXPECT(snapshot[0] == "app");
  EXPECT(snapshot[1] == "t3code");
  EXPECT(snapshot[2] == "zeta");
  EXPECT(snapshot[3] == "alpha");
  EXPECT(registry.size() == 4);
}

static void TestRegistryConcurrentAdd() {
  // The runtime registers from its own thread while the UI thread snapshots.
  // Hammer Add/Snapshot from several threads and check the final set is
  // exactly the union, with each name present once.
  SchemeRegistry registry;
  static constexpr int kThreads = 8;
  static constexpr int kPerThread = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&registry, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        // Overlapping names across threads: only one Add per name wins.
        registry.Add("s" + std::to_string(i % 50));
        registry.Add("thread" + std::to_string(t));
        std::vector<std::string> snapshot = registry.Snapshot();
        EXPECT(!snapshot.empty() && snapshot[0] == "app");
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT(registry.size() == 1 + 50 + kThreads);
  for (int i = 0; i < 50; ++i) {
    EXPECT(registry.Contains("s" + std::to_string(i)));
  }
  for (int t = 0; t < kThreads; ++t) {
    EXPECT(registry.Contains("thread" + std::to_string(t)));
  }
}

static void TestGlobalInstance() {
  SchemeRegistry* a = SchemeRegistry::GetInstance();
  SchemeRegistry* b = SchemeRegistry::GetInstance();
  EXPECT(a == b);
  EXPECT(a->Contains("app"));
}

static void TestParseSchemeList() {
  std::vector<std::string> rejected;

  std::vector<std::string> empty = ParseSchemeList("", &rejected);
  EXPECT(empty.empty());
  EXPECT(rejected.empty());

  std::vector<std::string> one = ParseSchemeList("t3code", &rejected);
  EXPECT(one.size() == 1 && one[0] == "t3code");

  // Whitespace, case, duplicates and empty entries.
  std::vector<std::string> many =
      ParseSchemeList(" T3Code , other,,t3code, third ", &rejected);
  EXPECT(many.size() == 3);
  EXPECT(many[0] == "t3code");
  EXPECT(many[1] == "other");
  EXPECT(many[2] == "third");
  EXPECT(rejected.empty());

  // Invalid entries are reported and skipped; valid neighbours survive.
  std::vector<std::string> mixed =
      ParseSchemeList("good,1bad,also good,app://,fine", &rejected);
  EXPECT(mixed.size() == 2);
  EXPECT(mixed[0] == "good");
  EXPECT(mixed[1] == "fine");
  EXPECT(rejected.size() == 3);
  EXPECT(rejected[0] == "1bad");
  EXPECT(rejected[1] == "also good");
  EXPECT(rejected[2] == "app://");

  // A null `rejected` is allowed.
  std::vector<std::string> quiet = ParseSchemeList("ok,not ok");
  EXPECT(quiet.size() == 1 && quiet[0] == "ok");
}

// The CEF backend's startup declaration (cef/src/custom_schemes.cc): the
// --laufey-custom-schemes switch value and LAUFEY_CUSTOM_SCHEMES are merged
// behind the built-in "app", and the result minus "app" is forwarded to child
// processes.
static void TestMergeSchemeLists() {
  std::vector<std::string> rejected;

  // Nothing declared: just "app".
  std::vector<std::string> none = MergeSchemeLists({}, &rejected);
  EXPECT(none.size() == 1 && none[0] == "app");
  EXPECT(MergeSchemeLists({"", " , "}, &rejected).size() == 1);
  EXPECT(rejected.empty());

  // Switch then env: "app" stays first, order is first-seen, case and
  // duplicates (within and across the two sources, "app" included) collapse.
  std::vector<std::string> merged =
      MergeSchemeLists({"MyApp,app,other", "other, THIRD ,myapp"}, &rejected);
  EXPECT(merged.size() == 4);
  EXPECT(merged[0] == "app");
  EXPECT(merged[1] == "myapp");
  EXPECT(merged[2] == "other");
  EXPECT(merged[3] == "third");
  EXPECT(rejected.empty());

  // Invalid names from either source are reported and dropped.
  std::vector<std::string> mixed =
      MergeSchemeLists({"good,bad name", "1bad,fine"}, &rejected);
  EXPECT(mixed.size() == 3);
  EXPECT(mixed[1] == "good" && mixed[2] == "fine");
  EXPECT(rejected.size() == 2);
  EXPECT(rejected[0] == "bad name" && rejected[1] == "1bad");

  // A null `rejected` is allowed.
  EXPECT(MergeSchemeLists({"ok,not ok"}).size() == 2);
}

static void TestJoinForwardedSchemes() {
  // "app" alone forwards nothing (children declare it themselves).
  EXPECT(JoinForwardedSchemes({"app"}).empty());
  EXPECT(JoinForwardedSchemes({}).empty());
  EXPECT(JoinForwardedSchemes({"app", "myapp"}) == "myapp");
  EXPECT(JoinForwardedSchemes({"app", "myapp", "other"}) == "myapp,other");
  // The forwarded value parses back to the same declaration in the child.
  std::vector<std::string> declared =
      MergeSchemeLists({"myapp,other", "third"});
  EXPECT(MergeSchemeLists({JoinForwardedSchemes(declared)}) == declared);
}

static void TestMergeForwardedSchemes() {
  // No switch on the child yet: the declared list, as JoinForwardedSchemes.
  EXPECT(MergeForwardedSchemes("", {"app", "myapp"}) == "myapp");
  EXPECT(MergeForwardedSchemes("", {"app"}).empty());
  // The child already has some: keep them, add the rest, no duplicates.
  EXPECT(MergeForwardedSchemes("myapp", {"app", "myapp", "other"}) ==
         "myapp,other");
  EXPECT(MergeForwardedSchemes("other,myapp", {"app", "myapp", "third"}) ==
         "other,myapp,third");
  // Case-insensitive, and "app" / invalid entries on the child are dropped.
  EXPECT(MergeForwardedSchemes("MyApp,app,not ok", {"app", "myapp"}) ==
         "myapp");
  // Nothing to add leaves the child's value as it was.
  EXPECT(MergeForwardedSchemes("myapp,other", {"app", "other"}) ==
         "myapp,other");
}

static void TestOriginScheme() {
  EXPECT(OriginScheme("myapp://app") == "myapp");
  EXPECT(OriginScheme("MyApp://app/") == "myapp");
  EXPECT(OriginScheme("https://example.com:8443") == "https");
  EXPECT(OriginScheme("app://localhost/index.html?x#y") == "app");
  EXPECT(OriginScheme("null").empty());  // an opaque origin
  EXPECT(OriginScheme("").empty());
  EXPECT(OriginScheme("://app").empty());
  EXPECT(OriginScheme("my app://x").empty());
  EXPECT(OriginScheme("data:text/html,x").empty());
}

static void TestLocalNetworkPrompt() {
  // Bits as in CEF's cef_permission_request_types_t: LOCAL_NETWORK_ACCESS,
  // LOCAL_NETWORK, LOOPBACK_NETWORK, and two unrelated ones.
  const uint32_t kLna = 1u << 25, kLocal = 1u << 26, kLoopback = 1u << 27;
  const uint32_t kMask = kLna | kLocal | kLoopback;
  const uint32_t kCamera = 1u << 2, kNotifications = 1u << 15;
  const std::vector<std::string> declared = MergeSchemeLists({"t3code"});
  using D = LocalNetworkPromptDecision;

  // The embedder's own origins get local network access.
  EXPECT(DecideLocalNetworkPrompt("t3code://app", kLoopback, kMask,
                                  declared) == D::kAccept);
  EXPECT(DecideLocalNetworkPrompt("t3code://app/", kLna, kMask, declared) ==
         D::kAccept);
  EXPECT(DecideLocalNetworkPrompt("T3Code://app", kLocal | kLoopback, kMask,
                                  declared) == D::kAccept);
  EXPECT(DecideLocalNetworkPrompt("app://localhost", kLoopback, kMask,
                                  declared) == D::kAccept);
  EXPECT(IsLocalNetworkTrustedOrigin("t3code://other-host", declared));

  // Everyone else is denied: remote and loopback http(s), file, data, an
  // opaque origin, a scheme that wasn't declared at launch.
  for (const char* origin :
       {"https://example.com", "http://example.com", "http://127.0.0.1:8080",
        "http://localhost:3000", "file:///tmp/x.html", "null", "",
        "data:text/html,x", "other://app", "t3codex://app"}) {
    EXPECT(!IsLocalNetworkTrustedOrigin(origin, declared));
    EXPECT(DecideLocalNetworkPrompt(origin, kLoopback, kMask, declared) ==
           D::kDeny);
  }

  // A prompt that bundles another permission is never granted through this
  // path, not even for the app's origin.
  EXPECT(DecideLocalNetworkPrompt("t3code://app", kLoopback | kCamera, kMask,
                                  declared) == D::kDeny);

  // Prompts without a local network bit keep the default handling,
  // whatever the origin.
  EXPECT(DecideLocalNetworkPrompt("t3code://app", kNotifications, kMask,
                                  declared) == D::kDefault);
  EXPECT(DecideLocalNetworkPrompt("https://example.com", kCamera, kMask,
                                  declared) == D::kDefault);
  EXPECT(DecideLocalNetworkPrompt("t3code://app", 0, kMask, declared) ==
         D::kDefault);

  // With only "app" declared, a custom scheme is not trusted.
  EXPECT(DecideLocalNetworkPrompt("t3code://app", kLoopback, kMask,
                                  MergeSchemeLists({})) == D::kDeny);
}

int main() {
  TestSchemeNameGrammar();
  TestRegistryDefaults();
  TestRegistryAdd();
  TestRegistryConcurrentAdd();
  TestGlobalInstance();
  TestParseSchemeList();
  TestMergeSchemeLists();
  TestJoinForwardedSchemes();
  TestMergeForwardedSchemes();
  TestOriginScheme();
  TestLocalNetworkPrompt();
  std::printf("scheme_registry_test: all tests passed\n");
  return 0;
}
