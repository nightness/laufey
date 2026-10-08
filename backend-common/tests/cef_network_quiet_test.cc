// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the CEF host's network-quiet defaults
// (src/cef_network_quiet.cc). No test framework; exits non-zero if any
// expectation fails.

#include "laufey_cef_network_quiet.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace laufey_common;

static int g_failures = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

using Names = std::vector<std::string>;

static const char kAll[] =
    "NetworkTimeServiceQuerying,AimEnabled,PreconnectToSearch";

static void TestFeatureListNames() {
  EXPECT(FeatureListNames("") == Names{});
  EXPECT(FeatureListNames(",, ,") == Names{});
  EXPECT((FeatureListNames("A,B") == Names{"A", "B"}));
  // Field-trial suffixes, params and the "*" prefix are not part of the name.
  EXPECT((FeatureListNames("*A<Trial,B:x/1/y/2,C<T.G:p/v, D ") ==
          Names{"A", "B", "C", "D"}));
}

static void TestMerge() {
  // Nothing on the command line: every default, in order.
  EXPECT(MergeQuietDisabledFeatures("", "") == kAll);
  EXPECT(CefQuietDisabledFeatures().size() == 3);
  // The app's own disabled features come first and are kept as written.
  EXPECT(MergeQuietDisabledFeatures("Foo<Trial", "") ==
         std::string("Foo<Trial,") + kAll);
  EXPECT(MergeQuietDisabledFeatures("Foo,", "") == std::string("Foo,") + kAll);
  // A default already disabled is not repeated.
  EXPECT(MergeQuietDisabledFeatures("AimEnabled", "") ==
         "AimEnabled,NetworkTimeServiceQuerying,PreconnectToSearch");
  EXPECT(MergeQuietDisabledFeatures(kAll, "") == kAll);
  // An app that enables one of them keeps it: the explicit choice wins.
  EXPECT(MergeQuietDisabledFeatures("", "PreconnectToSearch") ==
         "NetworkTimeServiceQuerying,AimEnabled");
  EXPECT(MergeQuietDisabledFeatures("", "OverlayScrollbar,*AimEnabled<T") ==
         "NetworkTimeServiceQuerying,PreconnectToSearch");
  EXPECT(MergeQuietDisabledFeatures(
             "X", "NetworkTimeServiceQuerying,AimEnabled,PreconnectToSearch") ==
         "X");
}

static void TestGaiaUrl() {
  // An http(s) origin with no path: what Chromium's --gaia-url accepts.
  EXPECT(std::string(kCefQuietGaiaUrl) == "https://localhost:9");
}

static void TestLocalHunspellDictionaries() {
  const Names files = {"en-US-10-1.bdic", "DE-de-3-0.BDIC", "fr-FR.bdic",
                       "es-ES-3-0.bdic.tmp", "Custom Dictionary.txt"};
  // Present (case does not matter) and kept in the profile's order.
  EXPECT((LocalHunspellDictionaries({"de-DE", "en-US"}, files) ==
          Names{"de-DE", "en-US"}));
  // No file, an unversioned name or a partial download: dropped.
  EXPECT(LocalHunspellDictionaries({"fr-FR", "es-ES", "it-IT"}, files) ==
         Names{});
  // A language is not matched by a longer one's file.
  EXPECT(LocalHunspellDictionaries({"en"}, files) == Names{});
  EXPECT(LocalHunspellDictionaries({"en-US"}, {}) == Names{});
  EXPECT(LocalHunspellDictionaries({}, files) == Names{});
  EXPECT(LocalHunspellDictionaries({""}, {"-1-0.bdic"}) == Names{});
}

int main() {
  TestFeatureListNames();
  TestMerge();
  TestGaiaUrl();
  TestLocalHunspellDictionaries();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("cef_network_quiet_test: all passed\n");
  return 0;
}
