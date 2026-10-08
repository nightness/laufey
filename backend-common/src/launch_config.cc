// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Launch configuration (laufey-launch.json). See laufey_launch_config.h and
// docs/launch-config.md.

#include "laufey_launch_config.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

#include "json_reader.h"
#include "laufey_bridge_origin.h"
#include "laufey_backend_common.h"
#include "laufey_passkey_rp_id.h"
#include "laufey_scheme_registry.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace laufey_common {

const char kLaunchConfigFileName[] = "laufey-launch.json";

namespace {

// A launch file is a few hundred bytes; anything this large is not one.
constexpr size_t kMaxLaunchConfigBytes = 64 * 1024;

// The reader lives in json_reader.h (shared with the passkey options parser).
using json::JsonReader;
using json::JsonValue;
using json::TypeName;

// --- Schema -----------------------------------------------------------------
//
// Values are validated with the same rules as their environment variables:
// IsSafeAppId (appId), IsAbsolutePath (dataDir) and IsValidSchemeName
// (customSchemes entries). passkeyRpIds entries (no environment variable)
// follow the passkey options parser's IsValidPasskeyRpId, after lowercasing.

void Warn(std::vector<std::string>* warnings, const std::string& message) {
  if (warnings)
    warnings->push_back(message);
}

// A string value the schema accepts: non-empty, no NUL.
bool CheckString(const std::string& key, const JsonValue& value,
                 std::vector<std::string>* warnings) {
  if (value.type != JsonValue::Type::kString) {
    Warn(warnings, "\"" + key + "\" must be a string, not " +
                       TypeName(value.type) + "; ignoring it");
    return false;
  }
  if (value.string.empty() || value.string.find('\0') != std::string::npos) {
    Warn(warnings, "\"" + key +
                       "\" must be a non-empty string without NUL "
                       "characters; ignoring it");
    return false;
  }
  return true;
}

bool IsPathSeparator(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

// Everything before the last '/' of `path` ("" if there is none).
std::string ParentOf(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool EndsWith(const std::string& s, const char* suffix) {
  size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Reads the launch file at `path` into `text`. Returns false (silently) if it
// doesn't exist, false with a warning if it can't be read or is too large.
bool ReadLaunchFile(const std::string& path, std::string* text,
                    std::vector<std::string>* warnings) {
#ifdef _WIN32
  FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"rb");
#else
  FILE* f = std::fopen(path.c_str(), "rb");
#endif
  if (!f) {
    if (errno != ENOENT && errno != ENOTDIR)
      Warn(warnings, std::string("could not open it: ") + std::strerror(errno));
    return false;
  }
  char buf[4096];
  size_t n;
  bool too_large = false;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    text->append(buf, n);
    if (text->size() > kMaxLaunchConfigBytes) {
      too_large = true;
      break;
    }
  }
  bool read_error = std::ferror(f) != 0;
  std::fclose(f);
  if (too_large) {
    Warn(warnings, "larger than " + std::to_string(kMaxLaunchConfigBytes) +
                       " bytes; ignoring the file");
    return false;
  }
  if (read_error) {
    Warn(warnings, "read error; ignoring the file");
    return false;
  }
  return true;
}

// Reports `warning` on stderr the first time `reported` is still clear: an
// ignored environment variable is worth one line per process, not one per
// read.
void ReportOnce(std::atomic<bool>* reported, const std::string& warning) {
  if (warning.empty() || reported->exchange(true))
    return;
  std::cerr << "laufey: " << warning << std::endl;
}

// The warning for an environment variable a pinned app id overrides.
std::string PinnedEnvWarning(const char* env_name) {
  return std::string(env_name) +
         " is ignored: the app's launch file pins its app id";
}

LaunchConfig LoadProcessLaunchConfig() {
  std::string path = LaunchConfigPathForExecutable(ExecutablePath());
  if (path.empty())
    return LaunchConfig();
  std::vector<std::string> warnings;
  std::string text;
  LaunchConfig config;
  if (ReadLaunchFile(path, &text, &warnings))
    config = ParseLaunchConfig(text, &warnings);
  for (const std::string& warning : warnings)
    std::cerr << "laufey: " << path << ": " << warning << std::endl;
  return config;
}

}  // namespace

LaunchConfig ParseLaunchConfig(const std::string& text,
                               std::vector<std::string>* warnings) {
  LaunchConfig config;
  // Tolerate a UTF-8 byte order mark (some Windows editors write one).
  std::string body = text;
  if (body.compare(0, 3, "\xEF\xBB\xBF") == 0)
    body.erase(0, 3);
  JsonValue root;
  std::string error;
  if (!JsonReader(body).ParseDocument(&root, &error)) {
    Warn(warnings, "not valid JSON (" + error + "); ignoring the file");
    return config;
  }
  if (root.type != JsonValue::Type::kObject) {
    Warn(warnings, std::string("the top level must be an object, not ") +
                       TypeName(root.type) + "; ignoring the file");
    return config;
  }
  std::vector<std::string> seen;
  for (const auto& [key, value] : root.object) {
    bool duplicate = false;
    for (const std::string& k : seen)
      duplicate = duplicate || k == key;
    if (duplicate)
      Warn(warnings, "duplicate key \"" + key + "\"; the last one wins");
    else
      seen.push_back(key);
    if (key == "appId") {
      config.has_app_id = false;
      if (!CheckString(key, value, warnings))
        continue;
      if (!IsSafeAppId(value.string)) {
        Warn(warnings, "\"appId\" \"" + value.string +
                           "\" is not a safe directory name (allowed: A-Z "
                           "a-z 0-9 . _ -); ignoring it");
        continue;
      }
      config.has_app_id = true;
      config.app_id = value.string;
    } else if (key == "dataDir") {
      config.has_data_dir = false;
      if (!CheckString(key, value, warnings))
        continue;
      if (!IsAbsolutePath(value.string)) {
        Warn(warnings, "\"dataDir\" must be an absolute path; ignoring \"" +
                           value.string + "\"");
        continue;
      }
      config.has_data_dir = true;
      config.data_dir = value.string;
    } else if (key == "customSchemes") {
      config.has_custom_schemes = false;
      config.custom_schemes.clear();
      if (value.type != JsonValue::Type::kArray) {
        Warn(warnings, std::string("\"customSchemes\" must be an array of "
                                   "strings, not ") +
                           TypeName(value.type) + "; ignoring it");
        continue;
      }
      config.has_custom_schemes = true;
      for (const JsonValue& item : value.array) {
        if (!CheckString("customSchemes[]", item, warnings))
          continue;
        if (!IsValidSchemeName(item.string)) {
          Warn(warnings, "\"customSchemes\" entry \"" + item.string +
                             "\" is not a valid URL scheme name (or is "
                             "reserved); ignoring it");
          continue;
        }
        config.custom_schemes.push_back(item.string);
      }
    } else if (key == "singleInstance") {
      config.has_single_instance = false;
      if (value.type != JsonValue::Type::kBool) {
        Warn(warnings, std::string("\"singleInstance\" must be a boolean, "
                                   "not ") +
                           TypeName(value.type) + "; ignoring it");
        continue;
      }
      config.has_single_instance = true;
      config.single_instance = value.boolean;
    } else if (key == "inspectable") {
      config.has_inspectable = false;
      config.inspectable = true;
      if (value.type != JsonValue::Type::kBool) {
        Warn(warnings, std::string("\"inspectable\" must be a boolean, not ") +
                           TypeName(value.type) + "; ignoring it");
        continue;
      }
      config.has_inspectable = true;
      config.inspectable = value.boolean;
    } else if (key == "requireSandbox") {
      config.has_require_sandbox = false;
      config.require_sandbox = false;
      if (value.type != JsonValue::Type::kBool) {
        Warn(warnings,
             std::string("\"requireSandbox\" must be a boolean, not ") +
                 TypeName(value.type) + "; ignoring it");
        continue;
      }
      config.has_require_sandbox = true;
      config.require_sandbox = value.boolean;
    } else if (key == "bridgeOrigins") {
      config.has_bridge_origins = false;
      config.bridge_origins.clear();
      if (value.type != JsonValue::Type::kArray) {
        Warn(warnings, std::string("\"bridgeOrigins\" must be an array of "
                                   "strings, not ") +
                           TypeName(value.type) + "; ignoring it");
        continue;
      }
      // The key restricts the bridge even if every entry is invalid: an app
      // that pins its origins never falls back to serving every origin.
      config.has_bridge_origins = true;
      for (const JsonValue& item : value.array) {
        if (!CheckString("bridgeOrigins[]", item, warnings))
          continue;
        std::string origin, scheme;
        bool any = false;
        if (!ParseBridgeOriginEntry(item.string, &origin, &scheme, &any)) {
          Warn(warnings, "\"bridgeOrigins\" entry \"" + item.string +
                             "\" is not \"*\", \"<scheme>://*\" or an "
                             "origin (scheme://host[:port]); ignoring it");
          continue;
        }
        config.bridge_origins.push_back(item.string);
      }
    } else if (key == "passkeyRpIds") {
      config.has_passkey_rp_ids = false;
      config.passkey_rp_ids.clear();
      if (value.type != JsonValue::Type::kArray) {
        Warn(warnings, std::string("\"passkeyRpIds\" must be an array of "
                                   "strings, not ") +
                           TypeName(value.type) + "; ignoring it");
        continue;
      }
      config.has_passkey_rp_ids = true;
      for (const JsonValue& item : value.array) {
        if (!CheckString("passkeyRpIds[]", item, warnings))
          continue;
        std::string rp_id = item.string;
        for (char& c : rp_id) {
          if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (!IsValidPasskeyRpId(rp_id)) {
          Warn(warnings, "\"passkeyRpIds\" entry \"" + item.string +
                             "\" is not a valid RP ID (a domain name, without "
                             "a scheme, port or path); ignoring it");
          continue;
        }
        config.passkey_rp_ids.push_back(rp_id);
      }
    } else {
      Warn(warnings, "unknown key \"" + key + "\"; ignoring it");
    }
  }
  return config;
}

std::string MacBundleResourcesDir(const std::string& exe_path) {
  // <B>.app/Contents/MacOS/<exe>
  std::string macos_dir = ParentOf(exe_path);
  if (!EndsWith(macos_dir, "/Contents/MacOS"))
    return std::string();
  std::string contents = ParentOf(macos_dir);
  std::string bundle = ParentOf(contents);
  if (!EndsWith(bundle, ".app"))
    return std::string();
  // A helper app (CEF's "<App> Helper.app") lives in the main app's
  // Contents/Frameworks; the launch file belongs to the main app.
  std::string frameworks = ParentOf(bundle);
  if (EndsWith(frameworks, "/Contents/Frameworks")) {
    std::string outer = ParentOf(ParentOf(frameworks));
    if (EndsWith(outer, ".app"))
      bundle = outer;
  }
  return bundle + "/Contents/Resources";
}

std::string LaunchConfigPathForExecutable(const std::string& exe_path) {
#ifdef __APPLE__
  std::string resources = MacBundleResourcesDir(exe_path);
  if (!resources.empty())
    return resources + "/" + kLaunchConfigFileName;
#endif
  size_t end = exe_path.size();
  while (end > 0 && !IsPathSeparator(exe_path[end - 1]))
    --end;
  if (end == 0)
    return std::string();
  // Keep the separator the path already uses.
  return exe_path.substr(0, end) + kLaunchConfigFileName;
}

std::string ExecutablePath() {
#ifdef _WIN32
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    DWORD n =
        GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
    if (n == 0)
      return std::string();
    if (n < buf.size()) {
      buf.resize(n);
      return WideToUtf8(buf);
    }
    if (buf.size() >= 32768)
      return std::string();
    buf.resize(buf.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string raw(size, '\0');
  if (_NSGetExecutablePath(&raw[0], &size) != 0)
    return std::string();
  raw.resize(std::strlen(raw.c_str()));
  char resolved[PATH_MAX];
  if (realpath(raw.c_str(), resolved))
    return resolved;
  return raw;
#else
  std::string buf(256, '\0');
  for (;;) {
    ssize_t n = readlink("/proc/self/exe", &buf[0], buf.size());
    if (n < 0)
      return std::string();
    if (static_cast<size_t>(n) < buf.size()) {
      buf.resize(static_cast<size_t>(n));
      return buf;
    }
    if (buf.size() >= 65536)
      return std::string();
    buf.resize(buf.size() * 2);
  }
#endif
}

std::string TrustedAppImagePath(
    const std::string& exe, const std::string& appimage,
    const std::string& appdir,
    const std::function<std::string(const std::string&)>& canonical) {
  if (exe.empty() || appimage.empty() || appimage[0] != '/' || appdir.empty())
    return std::string();
  std::string mount = canonical(appdir);
  if (mount.empty())
    return std::string();
  // Inside: the mount itself or below it, never a sibling that only shares
  // its name as a prefix (/tmp/.mount_ab vs /tmp/.mount_abc).
  if (mount.size() > 1 && mount.back() == '/')
    mount.pop_back();
  bool inside = exe == mount || (exe.size() > mount.size() &&
                                 exe.compare(0, mount.size(), mount) == 0 &&
                                 (mount == "/" || exe[mount.size()] == '/'));
  if (!inside)
    return std::string();
  return canonical(appimage);
}

std::string RelaunchExecutablePath() {
  std::string exe = ExecutablePath();
#ifndef _WIN32
  auto canonical = [](const std::string& path) {
    char resolved[PATH_MAX];
    return realpath(path.c_str(), resolved) ? std::string(resolved)
                                            : std::string();
  };
  const char* appimage = getenv("APPIMAGE");
  const char* appdir = getenv("APPDIR");
  std::string image = TrustedAppImagePath(
      exe, appimage ? appimage : "", appdir ? appdir : "", canonical);
  if (!image.empty())
    return image;
#endif
  return exe;
}

const LaunchConfig& ProcessLaunchConfig() {
  static const LaunchConfig config = LoadProcessLaunchConfig();
  return config;
}

std::string LaunchSettingFrom(const std::string& env_value, bool file_has,
                              const std::string& file_value) {
  if (!env_value.empty())
    return env_value;
  return file_has ? file_value : std::string();
}

std::string LaunchPinnedSettingFrom(const std::string& env_value,
                                    bool file_has,
                                    const std::string& file_value) {
  if (file_has)
    return file_value;
  return env_value;
}

std::string LaunchAppId() {
  const LaunchConfig& file = ProcessLaunchConfig();
  return LaunchPinnedSettingFrom(GetEnvUtf8("LAUFEY_APP_ID"), file.has_app_id,
                                 file.app_id);
}

std::string LaunchDataDirFrom(const std::string& env_value, bool app_id_pinned,
                              bool file_has, const std::string& file_value,
                              std::string* warning) {
  if (file_has || app_id_pinned) {
    if (warning && !env_value.empty()) {
      *warning = file_has ? std::string(
                                "LAUFEY_DATA_DIR is ignored: the "
                                "app's launch file sets its dataDir")
                          : PinnedEnvWarning("LAUFEY_DATA_DIR");
    }
    return file_has ? file_value : std::string();
  }
  return env_value;
}

std::string LaunchCustomSchemesFrom(const std::string& env_value,
                                    bool app_id_pinned, bool file_has,
                                    const std::string& file_value,
                                    std::string* warning) {
  if (!app_id_pinned)
    return LaunchSettingFrom(env_value, file_has, file_value);
  if (warning && !env_value.empty())
    *warning = PinnedEnvWarning("LAUFEY_CUSTOM_SCHEMES");
  return file_has ? file_value : std::string();
}

bool LaunchSingleInstanceFrom(const std::string& env_value, bool app_id_pinned,
                              bool file_has, bool file_value,
                              std::string* warning) {
  if (!app_id_pinned) {
    return LaunchBoolSettingFrom("LAUFEY_SINGLE_INSTANCE", env_value, file_has,
                                 file_value, warning);
  }
  if (warning && !env_value.empty())
    *warning = PinnedEnvWarning("LAUFEY_SINGLE_INSTANCE");
  return file_has && file_value;
}

bool LaunchAppIdPinned() {
  return ProcessLaunchConfig().has_app_id;
}

std::string LaunchDataDir() {
  static std::atomic<bool> reported{false};
  const LaunchConfig& file = ProcessLaunchConfig();
  std::string warning;
  std::string dir =
      LaunchDataDirFrom(GetEnvUtf8("LAUFEY_DATA_DIR"), file.has_app_id,
                        file.has_data_dir, file.data_dir, &warning);
  ReportOnce(&reported, warning);
  return dir;
}

bool LaunchBoolSettingFrom(const std::string& env_name,
                           const std::string& env_value, bool file_has,
                           bool file_value, std::string* warning) {
  if (!env_value.empty()) {
    if (env_value == "1" || env_value == "true")
      return true;
    if (env_value == "0" || env_value == "false")
      return false;
    if (warning)
      *warning = env_name + "=\"" + env_value +
                 "\" is not 1, 0, true or false; ignoring it";
  }
  return file_has && file_value;
}

bool LaunchInspectableFrom(const std::string& env_value, bool file_has,
                           bool file_value, std::string* warning) {
  if (file_has && !file_value) {
    if (warning && (env_value == "1" || env_value == "true"))
      *warning =
          "LAUFEY_INSPECTABLE=" + env_value +
          " is ignored: the app's launch file turns DevTools off";
    return false;
  }
  // Unlike singleInstance, the default is on: pass "the file says true"
  // when the file is silent.
  return LaunchBoolSettingFrom("LAUFEY_INSPECTABLE", env_value, true, true,
                               warning);
}

bool LaunchInspectable() {
  static const bool inspectable = [] {
    const LaunchConfig& file = ProcessLaunchConfig();
    std::string warning;
    bool on = LaunchInspectableFrom(GetEnvUtf8("LAUFEY_INSPECTABLE"),
                                    file.has_inspectable, file.inspectable,
                                    &warning);
    if (!warning.empty())
      std::cerr << "laufey: " << warning << std::endl;
    return on;
  }();
  return inspectable;
}

bool LaunchRequireSandboxFrom(const std::string& env_value, bool file_has,
                              bool file_value, std::string* warning) {
  if (file_has && file_value) {
    if (warning && (env_value == "0" || env_value == "false"))
      *warning = "LAUFEY_REQUIRE_SANDBOX=" + env_value +
                 " is ignored: the app's launch file requires the sandbox";
    return true;
  }
  return LaunchBoolSettingFrom("LAUFEY_REQUIRE_SANDBOX", env_value, file_has,
                               file_value, warning);
}

bool LaunchRequireSandbox() {
  const LaunchConfig& file = ProcessLaunchConfig();
  std::string warning;
  bool on = LaunchRequireSandboxFrom(GetEnvUtf8("LAUFEY_REQUIRE_SANDBOX"),
                                     file.has_require_sandbox,
                                     file.require_sandbox, &warning);
  if (!warning.empty())
    std::cerr << "laufey: " << warning << std::endl;
  return on;
}

bool LaunchSingleInstance() {
  static std::atomic<bool> reported{false};
  const LaunchConfig& file = ProcessLaunchConfig();
  std::string warning;
  bool on = LaunchSingleInstanceFrom(GetEnvUtf8("LAUFEY_SINGLE_INSTANCE"),
                                     file.has_app_id, file.has_single_instance,
                                     file.single_instance, &warning);
  ReportOnce(&reported, warning);
  return on;
}

std::string LaunchCustomSchemes() {
  static std::atomic<bool> reported{false};
  const LaunchConfig& file = ProcessLaunchConfig();
  std::string joined;
  for (const std::string& scheme : file.custom_schemes) {
    if (!joined.empty())
      joined.push_back(',');
    joined += scheme;
  }
  std::string warning;
  std::string schemes = LaunchCustomSchemesFrom(
      GetEnvUtf8("LAUFEY_CUSTOM_SCHEMES"), file.has_app_id,
      file.has_custom_schemes, joined, &warning);
  ReportOnce(&reported, warning);
  return schemes;
}

}  // namespace laufey_common
