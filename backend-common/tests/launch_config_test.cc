// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the launch configuration reader (src/launch_config.cc). No
// test framework: compile this file with src/launch_config.cc and
// src/data_dir.cc (plus src/strings_win.cc and shell32.lib ole32.lib on
// Windows), with backend-common/include and capi/include on the include path,
// and run it. CI does this in the `test` job. Exits non-zero if any
// expectation fails.
//
// The last test writes a laufey-launch.json next to this test binary to
// exercise the real lookup, and removes it again.

#include "laufey_launch_config.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "laufey_backend_common.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace laufey_common;

static int g_failures = 0;

// An absolute directory on this platform, as written in the JSON text and as
// parsed ('/' is a separator on Windows too).
#ifdef _WIN32
#define ABS "C:/data"
#else
#define ABS "/data"
#endif

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

static LaunchConfig Parse(const std::string& text, size_t* warning_count) {
  std::vector<std::string> warnings;
  LaunchConfig c = ParseLaunchConfig(text, &warnings);
  *warning_count = warnings.size();
  return c;
}

static bool IsEmpty(const LaunchConfig& c) {
  return !c.has_app_id && !c.has_data_dir && !c.has_custom_schemes &&
         !c.has_single_instance && !c.single_instance && c.app_id.empty() &&
         c.data_dir.empty() && c.custom_schemes.empty();
}

static void TestValid() {
  size_t w = 0;
  LaunchConfig c = Parse(
      "{ \"appId\": \"com.example.app\",\n"
      "  \"customSchemes\": [\"myapp\", \"other\"],\n"
      "  \"dataDir\": \"" ABS "/my app\" }\n",
      &w);
  EXPECT(w == 0);
  EXPECT(c.has_app_id && c.app_id == "com.example.app");
  EXPECT(c.has_data_dir && c.data_dir == ABS "/my app");
  EXPECT(c.has_custom_schemes && c.custom_schemes.size() == 2);
  EXPECT(c.custom_schemes.size() == 2 && c.custom_schemes[0] == "myapp" &&
         c.custom_schemes[1] == "other");

  // Every key is optional.
  c = Parse("{}", &w);
  EXPECT(w == 0 && IsEmpty(c));
  c = Parse(" \t\r\n{ \"appId\" : \"a\" } \n", &w);
  EXPECT(w == 0 && c.has_app_id && c.app_id == "a" && !c.has_data_dir);

  // An empty scheme list is valid (declares nothing).
  c = Parse("{\"customSchemes\": []}", &w);
  EXPECT(w == 0 && c.has_custom_schemes && c.custom_schemes.empty());

  // A UTF-8 byte order mark is tolerated.
  c = Parse("\xEF\xBB\xBF{\"appId\": \"bom\"}", &w);
  EXPECT(w == 0 && c.app_id == "bom");
}

static void TestMalformed() {
  const char* bad[] = {
      "",
      "   ",
      "{",
      "}",
      "{\"appId\"}",
      "{\"appId\":}",
      "{\"appId\": \"a\",}",
      "{\"appId\": \"a\" \"dataDir\": \"/x\"}",
      "{appId: \"a\"}",
      "{'appId': 'a'}",
      "{\"appId\": \"unterminated}",
      "{\"appId\": \"a\"} trailing",
      "{\"appId\": \"a\"}{}",
      "{\"appId\": \"tab\there\"}",  // raw control character
      "{\"appId\": \"bad \\x escape\"}",
      "{\"appId\": \"\\u12\"}",
      "{\"appId\": \"\\ud800\"}",         // lone high surrogate
      "{\"appId\": \"\\udc00\"}",         // lone low surrogate
      "{\"appId\": \"\\ud800\\u0041\"}",  // high + non-surrogate
      "{\"x\": 01}",
      "{\"x\": 1.}",
      "{\"x\": -}",
      "{\"x\": 1e}",
      "{\"x\": tru}",
      "{\"x\": nul}",
      "{\"x\": [1, 2,]}",
      "{\"x\": [1 2]}",
      "{\"x\": +1}",
      "{\"x\": NaN}",
  };
  for (const char* text : bad) {
    size_t w = 0;
    LaunchConfig c = Parse(text, &w);
    if (!(w == 1 && IsEmpty(c)))
      std::fprintf(stderr, "  (input: %s)\n", text);
    EXPECT(w == 1 && IsEmpty(c));
  }

  // Deep nesting is rejected, not a stack overflow.
  std::string deep = "{\"x\": ";
  for (int i = 0; i < 100000; ++i)
    deep += "[";
  size_t w = 0;
  EXPECT(IsEmpty(Parse(deep, &w)) && w == 1);

  // The top level must be an object.
  const char* not_object[] = {"[]", "\"appId\"", "42", "true", "null"};
  for (const char* text : not_object) {
    LaunchConfig c = Parse(text, &w);
    EXPECT(w == 1 && IsEmpty(c));
  }
}

static void TestSchema() {
  size_t w = 0;
  // Unknown keys are reported and skipped; known keys still apply.
  LaunchConfig c = Parse(
      "{\"appId\": \"a\", \"future\": {\"nested\": [1, 2.5e-3, true, null]},"
      " \"other\": -0.5E+3}",
      &w);
  EXPECT(w == 2 && c.has_app_id && c.app_id == "a");

  // Wrong types are reported and skipped.
  c = Parse("{\"appId\": 5, \"dataDir\": null, \"customSchemes\": \"x\"}", &w);
  EXPECT(w == 3 && IsEmpty(c));
  c = Parse("{\"appId\": [\"a\"], \"dataDir\": {}, \"customSchemes\": {}}", &w);
  EXPECT(w == 3 && IsEmpty(c));

  // Empty strings and embedded NULs are rejected.
  c = Parse("{\"appId\": \"\", \"dataDir\": \"" ABS "/a\\u0000b\"}", &w);
  EXPECT(w == 2 && IsEmpty(c));

  // The same rules as the environment variables: IsSafeAppId for appId,
  // IsAbsolutePath for dataDir, IsValidSchemeName for scheme entries.
  const char* bad_ids[] = {"..", ".", "a/b", "a\\\\b", "my app", "caf\\u00e9"};
  for (const char* id : bad_ids) {
    c = Parse(std::string("{\"appId\": \"") + id + "\"}", &w);
    EXPECT(w == 1 && IsEmpty(c));
  }
  c = Parse("{\"appId\": \"com.Example_app-2\"}", &w);
  EXPECT(w == 0 && c.app_id == "com.Example_app-2");
  const char* bad_dirs[] = {"relative/dir", "./x", "~/x", "data"};
  for (const char* dir : bad_dirs) {
    c = Parse(std::string("{\"dataDir\": \"") + dir + "\"}", &w);
    EXPECT(w == 1 && IsEmpty(c));
  }

  // Bad scheme entries are skipped one by one; the rest are kept.
  c = Parse(
      "{\"customSchemes\": [\"good\", 1, \"\", \"a,b\", null, \"1bad\","
      " \"my app\", \"x://y\", \" pad\", \"Also-Good+1.x\"]}",
      &w);
  EXPECT(w == 8 && c.has_custom_schemes && c.custom_schemes.size() == 2);
  EXPECT(c.custom_schemes.size() == 2 && c.custom_schemes[0] == "good" &&
         c.custom_schemes[1] == "Also-Good+1.x");

  // Duplicate keys: reported, the last one wins.
  c = Parse("{\"appId\": \"first\", \"appId\": \"second\"}", &w);
  EXPECT(w == 1 && c.app_id == "second");
  // ...including when the last one is invalid.
  c = Parse("{\"appId\": \"first\", \"appId\": 7}", &w);
  EXPECT(w == 2 && !c.has_app_id);

  // Escapes decode to UTF-8.
  c = Parse("{\"dataDir\": \"" ABS
            "/caf\\u00e9/\\ud83d\\ude00/\\\"q\\\"\\\\\\/\"}",
            &w);
  EXPECT(w == 0 && c.data_dir == ABS "/caf\xC3\xA9/\xF0\x9F\x98\x80/\"q\"\\/");
}

static void TestPrecedence() {
  EXPECT(LaunchSettingFrom("env", true, "file") == "env");
  EXPECT(LaunchSettingFrom("", true, "file") == "file");
  EXPECT(LaunchSettingFrom("", false, "file") == "");
  EXPECT(LaunchSettingFrom("env", false, "") == "env");
  EXPECT(LaunchSettingFrom("", false, "") == "");
  // App id and data dir: the shipped file wins over an inherited environment
  // (another app that launched this one must not move it into its profile).
  EXPECT(LaunchPinnedSettingFrom("env", true, "file") == "file");
  EXPECT(LaunchPinnedSettingFrom("", true, "file") == "file");
  EXPECT(LaunchPinnedSettingFrom("env", false, "") == "env");
  EXPECT(LaunchPinnedSettingFrom("", false, "") == "");
}

static void TestInspectable() {
  size_t w = 0;
  LaunchConfig c = Parse("{}", &w);
  EXPECT(w == 0 && !c.has_inspectable && c.inspectable);
  c = Parse("{\"inspectable\": false}", &w);
  EXPECT(w == 0 && c.has_inspectable && !c.inspectable);
  c = Parse("{\"inspectable\": true}", &w);
  EXPECT(w == 0 && c.has_inspectable && c.inspectable);
  // Only a JSON boolean counts; a wrong value leaves the default (on).
  const char* wrong[] = {"{\"inspectable\": 0}", "{\"inspectable\": \"no\"}",
                         "{\"inspectable\": null}"};
  for (const char* text : wrong) {
    c = Parse(text, &w);
    EXPECT(w == 1 && !c.has_inspectable && c.inspectable);
  }
  // The precedence LaunchInspectable uses: the environment wins, then the
  // file, then on.
  std::string warning;
  EXPECT(!LaunchBoolSettingFrom("V", "0", true, true, &warning));
  EXPECT(LaunchBoolSettingFrom("V", "1", true, false, &warning));
  EXPECT(!LaunchBoolSettingFrom("V", "", true, false, &warning));
  EXPECT(LaunchBoolSettingFrom("V", "", true, true, &warning));
  EXPECT(warning.empty());
  EXPECT(LaunchBoolSettingFrom("V", "maybe", true, true, &warning));
  EXPECT(!warning.empty());

  // What LaunchInspectable applies: a shipped "inspectable": false can't be
  // turned back on from the environment (reported), only off.
  warning.clear();
  EXPECT(!LaunchInspectableFrom("1", true, false, &warning));
  EXPECT(warning.find("ignored") != std::string::npos);
  warning.clear();
  EXPECT(!LaunchInspectableFrom("true", true, false, &warning));
  EXPECT(!LaunchInspectableFrom("", true, false, &warning));
  EXPECT(!LaunchInspectableFrom("0", true, true, &warning));
  EXPECT(!LaunchInspectableFrom("0", false, true, &warning));
  EXPECT(LaunchInspectableFrom("1", true, true, &warning));
  EXPECT(LaunchInspectableFrom("", true, true, &warning));
  EXPECT(LaunchInspectableFrom("", false, true, &warning));
}

static void TestSingleInstance() {
  size_t w = 0;
  LaunchConfig c = Parse("{\"singleInstance\": true}", &w);
  EXPECT(w == 0 && c.has_single_instance && c.single_instance);
  c = Parse("{\"singleInstance\": false}", &w);
  EXPECT(w == 0 && c.has_single_instance && !c.single_instance);
  // Only a JSON boolean counts.
  const char* wrong[] = {
      "{\"singleInstance\": 1}", "{\"singleInstance\": \"true\"}",
      "{\"singleInstance\": null}", "{\"singleInstance\": [true]}"};
  for (const char* text : wrong) {
    c = Parse(text, &w);
    EXPECT(w == 1 && !c.has_single_instance && !c.single_instance);
  }
  // The last duplicate wins; a wrong last value leaves it unset.
  c = Parse("{\"singleInstance\": true, \"singleInstance\": false}", &w);
  EXPECT(w == 1 && c.has_single_instance && !c.single_instance);
  c = Parse("{\"singleInstance\": true, \"singleInstance\": 0}", &w);
  EXPECT(w == 2 && !c.has_single_instance);

  // Environment precedence: 1/true/0/false win; anything else is reported
  // and the file decides.
  std::string warning;
  EXPECT(LaunchBoolSettingFrom("V", "1", false, false, &warning));
  EXPECT(LaunchBoolSettingFrom("V", "true", false, false, &warning));
  EXPECT(!LaunchBoolSettingFrom("V", "0", true, true, &warning));
  EXPECT(!LaunchBoolSettingFrom("V", "false", true, true, &warning));
  EXPECT(warning.empty());
  EXPECT(LaunchBoolSettingFrom("V", "", true, true, &warning));
  EXPECT(!LaunchBoolSettingFrom("V", "", false, true, &warning));
  EXPECT(!LaunchBoolSettingFrom("V", "", true, false, &warning));
  EXPECT(warning.empty());
  EXPECT(LaunchBoolSettingFrom("V", "yes", true, true, &warning));
  EXPECT(warning.find("V=\"yes\"") != std::string::npos);
  warning.clear();
  EXPECT(!LaunchBoolSettingFrom("V", "TRUE", false, false, &warning));
  EXPECT(!warning.empty());
}

static void TestPaths() {
  EXPECT(MacBundleResourcesDir("/Applications/My App.app/Contents/MacOS/My") ==
         "/Applications/My App.app/Contents/Resources");
  // CEF helper apps resolve to the main app's Resources.
  EXPECT(MacBundleResourcesDir(
             "/A/My.app/Contents/Frameworks/My Helper (Renderer).app/"
             "Contents/MacOS/My Helper (Renderer)") ==
         "/A/My.app/Contents/Resources");
  // A bundle in some other Frameworks directory is its own app.
  EXPECT(
      MacBundleResourcesDir("/A/Contents/Frameworks/H.app/Contents/MacOS/H") ==
      "/A/Contents/Frameworks/H.app/Contents/Resources");
  EXPECT(MacBundleResourcesDir("/usr/local/bin/laufey") == "");
  EXPECT(MacBundleResourcesDir("/x/Contents/MacOS/laufey") == "");
  EXPECT(MacBundleResourcesDir("laufey") == "");
  EXPECT(MacBundleResourcesDir("") == "");

#ifdef _WIN32
  EXPECT(LaunchConfigPathForExecutable("C:\\Program Files\\My\\my.exe") ==
         "C:\\Program Files\\My\\laufey-launch.json");
  EXPECT(LaunchConfigPathForExecutable("C:/p/my.exe") ==
         "C:/p/laufey-launch.json");
#else
  EXPECT(LaunchConfigPathForExecutable("/opt/my app/bin/my") ==
         "/opt/my app/bin/laufey-launch.json");
#endif
#ifdef __APPLE__
  EXPECT(LaunchConfigPathForExecutable("/A/My.app/Contents/MacOS/My") ==
         "/A/My.app/Contents/Resources/laufey-launch.json");
#elif !defined(_WIN32)
  // Only macOS applies the bundle layout.
  EXPECT(LaunchConfigPathForExecutable("/A/My.app/Contents/MacOS/My") ==
         "/A/My.app/Contents/MacOS/laufey-launch.json");
#endif
  EXPECT(LaunchConfigPathForExecutable("my") == "");
  EXPECT(LaunchConfigPathForExecutable("") == "");
}

static void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
  SetEnvironmentVariableW(Utf8ToWide(name).c_str(),
                          value ? Utf8ToWide(value).c_str() : nullptr);
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

// The real lookup: a launch file next to this executable, with and without
// the environment overriding it.
static void TestProcessLaunchConfig() {
  std::string exe = ExecutablePath();
  EXPECT(!exe.empty());
  std::string path = LaunchConfigPathForExecutable(exe);
  EXPECT(!path.empty());
  if (path.empty())
    return;
#ifdef _WIN32
  FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"wb");
#else
  FILE* f = std::fopen(path.c_str(), "wb");
#endif
  EXPECT(f != nullptr);
  if (!f)
    return;
  std::fputs("{\"appId\": \"dev.laufey.test\", \"dataDir\": \"" ABS
             "/from/file\","
             " \"customSchemes\": [\"one\", \"two\"], \"extra\": 1,"
             " \"singleInstance\": true}",
             f);
  std::fclose(f);

  SetEnv("LAUFEY_APP_ID", nullptr);
  SetEnv("LAUFEY_DATA_DIR", "");  // set but empty counts as unset
  SetEnv("LAUFEY_CUSTOM_SCHEMES", nullptr);
  SetEnv("LAUFEY_SINGLE_INSTANCE", nullptr);
  const LaunchConfig& c = ProcessLaunchConfig();
  EXPECT(c.has_app_id && c.app_id == "dev.laufey.test");
  EXPECT(LaunchAppId() == "dev.laufey.test");
  EXPECT(LaunchDataDir() == ABS "/from/file");
  EXPECT(LaunchCustomSchemes() == "one,two");
  EXPECT(LaunchSingleInstance());

  // The environment wins, key by key, except for the app id and data dir
  // the file pins (an inherited environment can't move the app's profile).
  SetEnv("LAUFEY_APP_ID", "from.env");
  SetEnv("LAUFEY_DATA_DIR", ABS "/from/env");
  SetEnv("LAUFEY_CUSTOM_SCHEMES", "envscheme");
  EXPECT(LaunchAppId() == "dev.laufey.test");
  EXPECT(LaunchDataDir() == ABS "/from/file");
  EXPECT(LaunchCustomSchemes() == "envscheme");
  SetEnv("LAUFEY_SINGLE_INSTANCE", "0");
  EXPECT(!LaunchSingleInstance());
  SetEnv("LAUFEY_SINGLE_INSTANCE", "bogus");  // reported, file decides
  EXPECT(LaunchSingleInstance());
  SetEnv("LAUFEY_SINGLE_INSTANCE", nullptr);
  SetEnv("LAUFEY_APP_ID", nullptr);
  SetEnv("LAUFEY_DATA_DIR", nullptr);
  SetEnv("LAUFEY_CUSTOM_SCHEMES", nullptr);

  // Read once per process: removing the file doesn't change the result.
#ifdef _WIN32
  _wremove(Utf8ToWide(path).c_str());
#else
  std::remove(path.c_str());
#endif
  EXPECT(LaunchAppId() == "dev.laufey.test");
}

int main() {
  TestValid();
  TestMalformed();
  TestSchema();
  TestPrecedence();
  TestSingleInstance();
  TestInspectable();
  TestPaths();
  TestProcessLaunchConfig();
  if (g_failures) {
    std::fprintf(stderr, "%d expectation(s) failed\n", g_failures);
    return 1;
  }
  std::printf("launch_config_test: all passed\n");
  return 0;
}
