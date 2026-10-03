// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Launch configuration: per-app settings the host executable reads at process
// start, before the runtime library is loaded.
//
// Some settings must be known before the embedder's runtime runs: CEF reads
// its custom schemes and profile directory while it initializes (in every
// process), and WebKitGTK's Wayland app_id is set in main(). The embedder
// can pass them as environment variables (LAUFEY_APP_ID, LAUFEY_DATA_DIR,
// LAUFEY_CUSTOM_SCHEMES), but a packaged app is often started directly
// (Explorer, a Start menu or desktop shortcut, exec) with no launcher to set
// them. It can ship them in a JSON file next to the executable instead:
//
//   Windows, Linux:  <directory of the executable>/laufey-launch.json
//   macOS bundle:    <App>.app/Contents/Resources/laufey-launch.json
//                    (CEF helper apps inside <App>.app/Contents/Frameworks
//                    read the outer app's file)
//
//   { "appId": "com.example.app",
//     "customSchemes": ["myapp"],
//     "dataDir": "/absolute/path",
//     "singleInstance": true,
//     "inspectable": false }
//
// Every key is optional. Each key stands in for its environment variable: an
// environment variable that is set (non-empty) wins over the file, key by
// key, so a launcher can still force a value. Except for the keys that
// isolate or lock down the installed app, where the shipped file wins:
// "appId" and "dataDir" (an environment inherited from another app, e.g. one
// that launched this one, must not move this app into its profile), and an
// "inspectable": false (the environment can't turn DevTools back on). The file is located from the
// executable's real path, never from the working directory, and is trusted
// like the executable itself (it belongs to the installed, possibly signed,
// app). A missing file is not an error; a malformed file, an unknown key, or
// a value of the wrong type is reported on stderr and ignored. See
// docs/launch-config.md.

#ifndef LAUFEY_LAUNCH_CONFIG_H_
#define LAUFEY_LAUNCH_CONFIG_H_

#include <string>
#include <vector>

namespace laufey_common {

// "laufey-launch.json".
extern const char kLaunchConfigFileName[];

// The validated contents of a launch file. A `has_*` flag is set only when
// the file contains that key with a valid value.
struct LaunchConfig {
  bool has_app_id = false;
  std::string app_id;
  bool has_data_dir = false;
  std::string data_dir;
  bool has_custom_schemes = false;
  std::vector<std::string> custom_schemes;
  bool has_single_instance = false;
  bool single_instance = false;
  bool has_inspectable = false;
  bool inspectable = true;
};

// Parses and validates the text of a launch file. Reads nothing from the
// environment or filesystem. Malformed JSON (or a top level that is not an
// object) yields an empty config; an unknown key or an invalid value is
// skipped. Values are held to the same rules as the environment variables:
// IsSafeAppId for "appId", IsAbsolutePath for "dataDir", IsValidSchemeName
// for each "customSchemes" entry. Appends a message to `warnings` (if
// non-null) for each problem.
LaunchConfig ParseLaunchConfig(const std::string& text,
                               std::vector<std::string>* warnings);

// For an executable at `exe_path` inside a macOS bundle
// (<B>.app/Contents/MacOS/<exe>), the bundle's Resources directory; for a
// helper bundle nested in another app's Contents/Frameworks, the outer app's
// Resources directory. "" if `exe_path` is not inside a bundle. Pure string
// handling ('/' separators), usable on any platform.
std::string MacBundleResourcesDir(const std::string& exe_path);

// Where the launch file for the executable at `exe_path` is: the macOS
// bundle's Resources directory (see MacBundleResourcesDir) when built for
// macOS and the executable is in a bundle, else the executable's directory.
// "" if `exe_path` has no directory component.
std::string LaunchConfigPathForExecutable(const std::string& exe_path);

// The running executable's real path (symlinks resolved where the platform
// allows), or "" if it can't be determined.
std::string ExecutablePath();

// This process's launch file, read and validated once (thread-safe); problems
// are reported on stderr the first time. Empty when there is no file.
const LaunchConfig& ProcessLaunchConfig();

// Precedence step behind the accessors below, exposed for tests: `env_value`
// if non-empty, else `file_value` if `file_has`, else "".
std::string LaunchSettingFrom(const std::string& env_value, bool file_has,
                              const std::string& file_value);

// Precedence step behind LaunchAppId and LaunchDataDir, exposed for tests:
// the file's value if `file_has`, else `env_value` (possibly "").
std::string LaunchPinnedSettingFrom(const std::string& env_value,
                                    bool file_has,
                                    const std::string& file_value);

// The effective settings. App id and data dir: the launch file's value if it
// has one, else the environment variable, else "" (LaunchPinnedSettingFrom).
// Custom schemes: the environment variable if set (non-empty), else the
// launch file's value, else "". Each has the environment variable's format,
// so callers treat both sources alike.
std::string LaunchAppId();          // LAUFEY_APP_ID  / "appId"
std::string LaunchDataDir();        // LAUFEY_DATA_DIR / "dataDir"
std::string LaunchCustomSchemes();  // LAUFEY_CUSTOM_SCHEMES / "customSchemes"
                                    // (comma-separated)

// Precedence step behind LaunchSingleInstance, exposed for tests: a non-empty
// `env_value` of "1"/"true" or "0"/"false" wins; any other non-empty value is
// reported in `warning` (naming `env_name`) and ignored; then the file's
// value if `file_has`; else false.
bool LaunchBoolSettingFrom(const std::string& env_name,
                           const std::string& env_value, bool file_has,
                           bool file_value, std::string* warning);

// Whether single-instance mode is on: LAUFEY_SINGLE_INSTANCE ("1"/"0",
// also "true"/"false") if set, else the launch file's "singleInstance", else
// false. An invalid environment value is reported on stderr and ignored.
bool LaunchSingleInstance();

// Precedence step behind LaunchInspectable, exposed for tests: false when the
// file says false (an environment value of "1"/"true" is then reported in
// `warning`); otherwise LaunchBoolSettingFrom with the default on.
bool LaunchInspectableFrom(const std::string& env_value, bool file_has,
                           bool file_value, std::string* warning);

// Whether the web engine's DevTools may open (API 40): off when the launch
// file says "inspectable": false (whatever the environment says), else
// LAUFEY_INSPECTABLE ("1"/"0", also "true"/"false") if set, else true. An
// invalid environment value is reported on stderr and ignored. Read once per
// process (the engines take it at startup).
bool LaunchInspectable();

}  // namespace laufey_common

#endif  // LAUFEY_LAUNCH_CONFIG_H_
