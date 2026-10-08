// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the host's command line (src/launch_args.cc). No test
// framework: compile this file with src/launch_args.cc, src/launch_config.cc
// and src/data_dir.cc, with backend-common/include and capi/include on the
// include path, and run it. CI does this in the `test` job and through
// ctest. Exits non-zero if any expectation fails.

#include "laufey_launch_args.h"

#include <algorithm>
#include <cstdio>
#include <functional>
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

using Args = std::vector<std::string>;

static bool Contains(const Args& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

static void TestRuntimeOption() {
  // A development host takes its runtime from the command line ...
  EXPECT(ParseHostOptions({"--runtime", "/rt.so"}, false).runtime_path ==
         "/rt.so");
  EXPECT(ParseHostOptions({"--runtime=/rt.so"}, false).runtime_path ==
         "/rt.so");
  // ... but only before "--": after it, `--runtime` is a positional argument.
  EXPECT(ParseHostOptions({"--", "--runtime", "/rt.so"}, false)
             .runtime_path.empty());
  EXPECT(
      ParseHostOptions({"--", "--runtime=/rt.so"}, false).runtime_path.empty());
  EXPECT(ParseHostOptions({"--runtime", "/a", "--", "--runtime", "/b"}, false)
             .runtime_path == "/a");
  // `--runtime` as the last argument has no value.
  EXPECT(ParseHostOptions({"--runtime"}, false).runtime_path.empty());
  // A packaged app never takes it.
  EXPECT(ParseHostOptions({"--runtime", "/rt.so"}, true).runtime_path.empty());
  EXPECT(ParseHostOptions({"--runtime=/rt.so"}, true).runtime_path.empty());
}

static void TestUrlArguments() {
  EXPECT(IsUrlArgument("acme://open/doc"));
  EXPECT(IsUrlArgument("t3code:bare"));
  EXPECT(IsUrlArgument("a+b.c-d://x"));
  EXPECT(IsUrlArgument("HTTPS://example.com"));
  // A drive letter, a file URL, a path, a flag: not deep links.
  EXPECT(!IsUrlArgument("C:\\Users\\me\\x.txt"));
  EXPECT(!IsUrlArgument("c:/x"));
  EXPECT(!IsUrlArgument("file:///tmp/x"));
  EXPECT(!IsUrlArgument("FILE:///tmp/x"));
  EXPECT(!IsUrlArgument("/tmp/x"));
  EXPECT(!IsUrlArgument("--flag=a:b"));
  EXPECT(!IsUrlArgument("1abc://x"));
  EXPECT(!IsUrlArgument("ac me://x"));
  EXPECT(!IsUrlArgument(""));

  EXPECT(IsDeepLinkLaunch({"--", "acme://x"}));
  EXPECT(IsDeepLinkLaunch({"acme://x"}));
  EXPECT(IsDeepLinkLaunch({"--runtime", "/rt", "--", "acme://x"}));
  // A switch value is not a positional argument.
  EXPECT(!IsDeepLinkLaunch({"--proxy-server=acme://x"}));
  EXPECT(!IsDeepLinkLaunch({"notes.txt", "C:\\x.txt"}));
  EXPECT(!IsDeepLinkLaunch({}));
  // After "--" even a dash-led argument is positional (and not a URL here).
  EXPECT(!IsDeepLinkLaunch({"--", "-x"}));
}

static void TestSwitchNames() {
  Args names =
      CommandLineSwitchNames({"--alpha", "--beta=1", "-gamma", "--alpha=2",
                              "plain", "-", "--", "--after", "-late"});
  EXPECT(names.size() == 3);
  EXPECT(Contains(names, "alpha"));
  EXPECT(Contains(names, "beta"));
  EXPECT(Contains(names, "gamma"));
  // Nothing after "--" is a switch.
  EXPECT(!Contains(names, "after"));
  EXPECT(!Contains(names, "late"));
#ifdef _WIN32
  // Chromium lower-cases switch names and accepts '/' on Windows.
  Args win = CommandLineSwitchNames({"--Mixed-Case=1", "/slash"});
  EXPECT(Contains(win, "mixed-case"));
  EXPECT(Contains(win, "slash"));
#endif
}

static void TestDeepLinkStrip() {
  // An ordinary launch keeps its switches.
  EXPECT(DeepLinkSwitchesToStrip({"--some-switch", "notes.txt"}, {}).empty());
  // The registered form: nothing before "--", so nothing to strip, and the
  // arguments after it are never switches.
  EXPECT(DeepLinkSwitchesToStrip({"--", "acme://x", "--after"}, {}).empty());
  // A link launch that carries switches before "--" (an old registration
  // without it): every one of them is dropped, whatever its name ...
  Args strip = DeepLinkSwitchesToStrip(
      {"acme://x", "--some-switch", "--another=1", "-third"}, {});
  EXPECT(strip.size() == 3);
  EXPECT(Contains(strip, "some-switch"));
  EXPECT(Contains(strip, "another"));
  EXPECT(Contains(strip, "third"));
  // ... except the allow-listed ones.
  strip = DeepLinkSwitchesToStrip({"--keep", "--drop", "acme://x"}, {"keep"});
  EXPECT(strip.size() == 1);
  EXPECT(Contains(strip, "drop"));
  // The Linux form (a .desktop entry's `Exec=app %u`, which the CEF Linux
  // host now records too): switches a launcher put next to the link are
  // dropped; the link itself, one argument even with spaces, is no switch.
  strip = DeepLinkSwitchesToStrip(
      {"--renderer-cmd-prefix=gdb --args", "--no-sandbox",
       "acme://open?q=a --remote-debugging-port=9222"},
      {});
  EXPECT(strip.size() == 2);
  EXPECT(Contains(strip, "renderer-cmd-prefix"));
  EXPECT(Contains(strip, "no-sandbox"));
  EXPECT(!Contains(strip, "remote-debugging-port"));
}

// A fake filesystem for ChooseRuntimePath: the paths in `files` exist.
static std::function<bool(const std::string&)> Files(Args files) {
  return [files](const std::string& path) { return Contains(files, path); };
}

static void TestChooseRuntimePath() {
  const Args bundled = {"/app/my.so"};
  const Args dev = {"./libruntime.so", "/usr/lib/laufey/libruntime.so"};

  // Development host: --runtime, then LAUFEY_RUNTIME_PATH, then the
  // fallbacks in order (the first that exists); nothing is reported.
  RuntimeChoice c = ChooseRuntimePath({"--runtime", "/arg.so"}, "/env.so",
                                      bundled, dev, false, Files({}));
  EXPECT(!c.packaged && c.path == "/arg.so" && c.warnings.empty());
  c = ChooseRuntimePath({"--runtime=/arg.so"}, "", bundled, dev, false,
                        Files({}));
  EXPECT(c.path == "/arg.so");
  c = ChooseRuntimePath({}, "/env.so", bundled, dev, false, Files({}));
  EXPECT(!c.packaged && c.path == "/env.so" && c.warnings.empty());
  c = ChooseRuntimePath({}, "", bundled, dev, false,
                        Files({"/usr/lib/laufey/libruntime.so"}));
  EXPECT(!c.packaged && c.path == "/usr/lib/laufey/libruntime.so");
  c = ChooseRuntimePath({}, "", bundled, dev, false,
                        Files({"./libruntime.so",
                               "/usr/lib/laufey/"
                               "libruntime.so"}));
  EXPECT(c.path == "./libruntime.so");
  c = ChooseRuntimePath({}, "", bundled, dev, false, Files({}));
  EXPECT(!c.packaged && c.path.empty() && c.warnings.empty());
  // A `--runtime` after "--" is a positional argument, not an option.
  c = ChooseRuntimePath({"--", "--runtime", "/arg.so"}, "", bundled, dev, false,
                        Files({}));
  EXPECT(c.path.empty());

  // A runtime the app ships makes it packaged: that one is loaded, and the
  // command line, the environment and the fallbacks are ignored (the first
  // two reported).
  c = ChooseRuntimePath({"--runtime", "/arg.so"}, "/env.so", bundled, dev,
                        false, Files({"/app/my.so", "./libruntime.so"}));
  EXPECT(c.packaged && c.path == "/app/my.so");
  EXPECT(c.warnings.size() == 2);
  EXPECT(c.warnings.size() == 2 &&
         c.warnings[0].find("--runtime") != std::string::npos &&
         c.warnings[1].find("LAUFEY_RUNTIME_PATH") != std::string::npos);
  c = ChooseRuntimePath({}, "", bundled, dev, false, Files({"/app/my.so"}));
  EXPECT(c.packaged && c.path == "/app/my.so" && c.warnings.empty());
  // The first shipped location that exists; empty entries are skipped.
  c = ChooseRuntimePath(
      {}, "", {"", "/app/Frameworks/libruntime.dylib", "/app/MacOS/x.dylib"},
      dev, false, Files({"/app/MacOS/x.dylib"}));
  EXPECT(c.packaged && c.path == "/app/MacOS/x.dylib");

  // A launch file makes it packaged too: without a shipped runtime there is
  // nothing to load (reported), however the host was started.
  c = ChooseRuntimePath({"--runtime=/arg.so"}, "/env.so", bundled, dev, true,
                        Files({"./libruntime.so", "/arg.so", "/env.so"}));
  EXPECT(c.packaged && c.path.empty());
  EXPECT(c.warnings.size() == 3);
  c = ChooseRuntimePath({}, "", bundled, dev, true,
                        Files({"/app/my.so", "./libruntime.so"}));
  EXPECT(c.packaged && c.path == "/app/my.so" && c.warnings.empty());
  // Only the environment set: one warning.
  c = ChooseRuntimePath({}, "/env.so", bundled, dev, true,
                        Files({"/app/my.so"}));
  EXPECT(c.path == "/app/my.so" && c.warnings.size() == 1);
}

static void TestMissingPackagedRuntime() {
  // Only a packaged app without its runtime must not start; a development
  // host without one keeps its own handling (CEF opens its demo window).
  RuntimeChoice c;
  EXPECT(!IsMissingPackagedRuntime(c));
  c.packaged = true;
  EXPECT(IsMissingPackagedRuntime(c));
  c.path = "/app/my.so";
  EXPECT(!IsMissingPackagedRuntime(c));
  c = ChooseRuntimePath({"--runtime", "/arg.so"}, "/env.so", {"/app/my.so"},
                        {}, true, Files({"/arg.so", "/env.so"}));
  EXPECT(IsMissingPackagedRuntime(c));
  c = ChooseRuntimePath({}, "/env.so", {"/app/my.so"}, {}, false, Files({}));
  EXPECT(!IsMissingPackagedRuntime(c));
  EXPECT(kMissingRuntimeExitCode != 0);
}

static void TestWebView2EnvironmentOverrides() {
  // A development launch, or a packaged app with DevTools on, honours them.
  EXPECT(WebView2EnvironmentOverridesToClear(false, false).empty());
  EXPECT(WebView2EnvironmentOverridesToClear(false, true).empty());
  EXPECT(WebView2EnvironmentOverridesToClear(true, true).empty());
  // A packaged app with DevTools off clears the dangerous ones only.
  Args clear = WebView2EnvironmentOverridesToClear(true, false);
  EXPECT(clear.size() == 5);
  EXPECT(Contains(clear, "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"));
  EXPECT(Contains(clear, "WEBVIEW2_BROWSER_EXECUTABLE_FOLDER"));
  EXPECT(Contains(clear, "WEBVIEW2_USER_DATA_FOLDER"));
  EXPECT(Contains(clear, "WEBVIEW2_PIPE_FOR_SCRIPT_DEBUGGER"));
  EXPECT(Contains(clear, "WEBVIEW2_WAIT_FOR_SCRIPT_DEBUGGER"));
  EXPECT(!Contains(clear, "WEBVIEW2_RELEASE_CHANNEL_PREFERENCE"));
}

static void TestProcessArgs() {
  EXPECT(ProcessArgs().empty());
  SetProcessArgs({"--", "acme://x"});
  EXPECT(ProcessArgs().size() == 2);
  EXPECT(ProcessArgs()[1] == "acme://x");
  // A colocated runtime alone makes it a packaged launch.
  EXPECT(IsPackagedLaunch(true));
}

static void TestHeadlessWorkerLaunch() {
  // `<exe> run <script>`: the embedder's CLI worker command (Deno Desktop's
  // update helper), with or without flags before the script.
  EXPECT(IsCliWorkerCommand({"run", "denext-update-helper", "apply", "42"}));
  EXPECT(IsCliWorkerCommand({"run", "-A", "--quiet", "worker.ts"}));
  EXPECT(IsCliWorkerCommand({"run", ""}));
  // Not a worker: no script, flags only, another verb, or the app itself.
  EXPECT(!IsCliWorkerCommand({}));
  EXPECT(!IsCliWorkerCommand({"run"}));
  EXPECT(!IsCliWorkerCommand({"run", "-A", "--quiet"}));
  EXPECT(!IsCliWorkerCommand({"serve", "worker.ts"}));
  EXPECT(!IsCliWorkerCommand({"--", "acme://run"}));
  EXPECT(!IsCliWorkerCommand({"--runtime", "/rt.so", "run", "x"}));

  auto env = [](std::vector<std::string> set) {
    return [set](const char* name) {
      return std::find(set.begin(), set.end(), name) != set.end();
    };
  };
  EXPECT(IsForkedWorkerEnvironment(env({"NODE_CHANNEL_FD"})));
  EXPECT(IsForkedWorkerEnvironment(env({"NEXT_PRIVATE_WORKER"})));
  EXPECT(!IsForkedWorkerEnvironment(env({})));
  // LAUFEY_SINGLE_INSTANCE has no say in it (a pinned launch file wins).
  EXPECT(!IsForkedWorkerEnvironment(env({"LAUFEY_SINGLE_INSTANCE"})));

  // The process-environment form agrees with the argument form.
  EXPECT(IsHeadlessWorkerLaunch({"run", "denext-update-helper"}));
}

int main() {
  TestRuntimeOption();
  TestUrlArguments();
  TestSwitchNames();
  TestDeepLinkStrip();
  TestChooseRuntimePath();
  TestMissingPackagedRuntime();
  TestWebView2EnvironmentOverrides();
  TestProcessArgs();
  TestHeadlessWorkerLaunch();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("launch_args_test: all passed\n");
  return 0;
}
