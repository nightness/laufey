// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The host's own command line. See laufey_launch_args.h.

#include "laufey_launch_args.h"

#include <algorithm>
#include <cstdio>
#include <utility>

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

void SetProcessArgs(std::vector<std::string> args) {
  ProcessArgsStorage() = std::move(args);
}

const std::vector<std::string>& ProcessArgs() {
  return ProcessArgsStorage();
}

}  // namespace laufey_common
