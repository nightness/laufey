// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The host's own command line. See laufey_launch_args.h.

#include "laufey_launch_args.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "laufey_backend_common.h"
#include "laufey_launch_config.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace laufey_common {

namespace {

const char kEndOfOptions[] = "--";
const char kRuntimeOption[] = "--runtime";
const char kRuntimeOptionEq[] = "--runtime=";

bool StartsWith(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

bool IsSchemeStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsSchemeChar(char c) {
  return IsSchemeStart(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
         c == '.';
}

char AsciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// The switch name of `arg` as Chromium parses it, or "" when it is not a
// switch. Chromium's prefixes: "--" and "-" everywhere, "/" on Windows; a
// prefix alone is not a switch.
std::string SwitchName(const std::string& arg) {
  size_t prefix = 0;
  if (StartsWith(arg, "--")) {
    prefix = 2;
  } else if (StartsWith(arg, "-")) {
    prefix = 1;
#ifdef _WIN32
  } else if (StartsWith(arg, "/")) {
    prefix = 1;
#endif
  } else {
    return std::string();
  }
  if (arg.size() <= prefix)
    return std::string();
  std::string name = arg.substr(prefix, arg.find('=') == std::string::npos
                                            ? std::string::npos
                                            : arg.find('=') - prefix);
#ifdef _WIN32
  std::transform(name.begin(), name.end(), name.begin(), AsciiLower);
#endif
  return name;
}

bool FileExists(const std::string& path) {
  if (path.empty())
    return false;
#ifdef _WIN32
  int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (n <= 0)
    return false;
  std::wstring wide(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], n);
  DWORD attrs = GetFileAttributesW(wide.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
  return access(path.c_str(), F_OK) == 0;
#endif
}

std::vector<std::string>& ProcessArgsStorage() {
  static std::vector<std::string>* args = new std::vector<std::string>();
  return *args;
}

}  // namespace

HostOptions ParseHostOptions(const std::vector<std::string>& args,
                             bool packaged) {
  HostOptions options;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == kEndOfOptions)
      break;
    if (arg == kRuntimeOption && i + 1 < args.size()) {
      ++i;
      if (!packaged)
        options.runtime_path = args[i];
    } else if (StartsWith(arg, kRuntimeOptionEq)) {
      if (!packaged)
        options.runtime_path = arg.substr(sizeof(kRuntimeOptionEq) - 1);
    }
  }
  if (packaged &&
      (std::find(args.begin(), args.end(), kRuntimeOption) != args.end() ||
       std::any_of(args.begin(), args.end(), [](const auto& a) {
         return StartsWith(a, kRuntimeOptionEq);
       }))) {
    std::fprintf(stderr,
                 "laufey: --runtime is ignored by a packaged app (it loads "
                 "the runtime next to its executable)\n");
  }
  return options;
}

bool IsUrlArgument(const std::string& arg) {
  size_t colon = arg.find(':');
  if (colon == std::string::npos || colon < 2 || !IsSchemeStart(arg[0]))
    return false;
  for (size_t i = 1; i < colon; ++i) {
    if (!IsSchemeChar(arg[i]))
      return false;
  }
  std::string scheme = arg.substr(0, colon);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), AsciiLower);
  return scheme != "file";
}

bool IsDeepLinkLaunch(const std::vector<std::string>& args) {
  bool options_ended = false;
  for (const std::string& arg : args) {
    if (!options_ended && arg == kEndOfOptions) {
      options_ended = true;
      continue;
    }
    if (!options_ended && StartsWith(arg, "-"))
      continue;
    if (IsUrlArgument(arg))
      return true;
  }
  return false;
}

std::vector<std::string> CommandLineSwitchNames(
    const std::vector<std::string>& args) {
  std::vector<std::string> names;
  for (const std::string& arg : args) {
    if (arg == kEndOfOptions)
      break;
    std::string name = SwitchName(arg);
    if (!name.empty() &&
        std::find(names.begin(), names.end(), name) == names.end()) {
      names.push_back(std::move(name));
    }
  }
  return names;
}

std::vector<std::string> DeepLinkSwitchesToStrip(
    const std::vector<std::string>& args,
    const std::vector<std::string>& allowed) {
  std::vector<std::string> strip;
  if (!IsDeepLinkLaunch(args))
    return strip;
  for (std::string& name : CommandLineSwitchNames(args)) {
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
      strip.push_back(std::move(name));
  }
  return strip;
}

bool IsPackagedLaunch(bool has_colocated_runtime) {
  if (has_colocated_runtime)
    return true;
  return FileExists(LaunchConfigPathForExecutable(ExecutablePath()));
}

RuntimeChoice ChooseRuntimePath(
    const std::vector<std::string>& args, const std::string& env_runtime_path,
    const std::vector<std::string>& bundled,
    const std::vector<std::string>& development, bool has_launch_file,
    const std::function<bool(const std::string&)>& exists) {
  RuntimeChoice choice;
  std::string shipped;
  for (const std::string& path : bundled) {
    if (!path.empty() && exists(path)) {
      shipped = path;
      break;
    }
  }
  // `--runtime` as a development host reads it (ParseHostOptions only warns
  // when told the app is packaged).
  std::string requested = ParseHostOptions(args, false).runtime_path;
  choice.packaged = has_launch_file || !shipped.empty();
  if (choice.packaged) {
    choice.path = shipped;
    if (!requested.empty()) {
      choice.warnings.push_back(
          "--runtime is ignored by a packaged app (it loads the runtime it "
          "ships)");
    }
    if (!env_runtime_path.empty()) {
      choice.warnings.push_back(
          "LAUFEY_RUNTIME_PATH is ignored by a packaged app (it loads the "
          "runtime it ships)");
    }
    if (shipped.empty()) {
      choice.warnings.push_back(
          "this packaged app (it has a laufey-launch.json) has no runtime "
          "library next to its executable");
    }
    return choice;
  }
  if (!requested.empty()) {
    choice.path = requested;
  } else if (!env_runtime_path.empty()) {
    choice.path = env_runtime_path;
  } else {
    for (const std::string& path : development) {
      if (!path.empty() && exists(path)) {
        choice.path = path;
        break;
      }
    }
  }
  return choice;
}

RuntimeChoice ResolveRuntimePath(const std::vector<std::string>& args,
                                 const std::vector<std::string>& bundled,
                                 const std::vector<std::string>& development) {
  RuntimeChoice choice = ChooseRuntimePath(
      args, GetEnvUtf8("LAUFEY_RUNTIME_PATH"), bundled, development,
      FileExists(LaunchConfigPathForExecutable(ExecutablePath())), FileExists);
  for (const std::string& warning : choice.warnings)
    std::fprintf(stderr, "laufey: %s\n", warning.c_str());
  return choice;
}

std::vector<std::string> WebView2EnvironmentOverridesToClear(
    bool packaged, bool inspectable) {
  if (!packaged || inspectable)
    return {};
  return {
      "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
      "WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",
      "WEBVIEW2_USER_DATA_FOLDER",
      "WEBVIEW2_PIPE_FOR_SCRIPT_DEBUGGER",
      "WEBVIEW2_WAIT_FOR_SCRIPT_DEBUGGER",
  };
}

bool IsCliWorkerCommand(const std::vector<std::string>& args) {
  if (args.size() < 2 || args[0] != "run")
    return false;
  for (size_t i = 1; i < args.size(); ++i) {
    if (!args[i].empty() && args[i][0] == '-')
      continue;
    return true;
  }
  return false;
}

bool IsForkedWorkerEnvironment(const std::function<bool(const char*)>& is_set) {
  return is_set("NODE_CHANNEL_FD") || is_set("NEXT_PRIVATE_WORKER");
}

bool IsHeadlessWorkerLaunch(const std::vector<std::string>& args) {
  return IsCliWorkerCommand(args) ||
         IsForkedWorkerEnvironment([](const char* name) {
#ifdef _WIN32
           char buf[2];
           return GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0;
#else
           return std::getenv(name) != nullptr;
#endif
         });
}

void SetProcessArgs(std::vector<std::string> args) {
  ProcessArgsStorage() = std::move(args);
}

const std::vector<std::string>& ProcessArgs() {
  return ProcessArgsStorage();
}

void ReportMissingPackagedRuntime() {
  std::fprintf(stderr,
               "laufey: this packaged app ships no runtime library next to "
               "its executable; exiting\n");
  std::fflush(stderr);
}

}  // namespace laufey_common
