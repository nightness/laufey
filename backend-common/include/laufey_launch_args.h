// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The host executable's own command line: laufey's options, the end of
// options, and what a deep-link launch may carry.
//
// A registered URL scheme starts the app with the link as an argument. On
// Windows the shell substitutes the link for "%1" in the registered command
// without escaping it, so a link containing a '"' can close the quotes and
// add arguments of its own (Electron's CVE-2018-1000006). Two defences live
// here:
//
//  * "--" ends the options. Registrations run `"<exe>" -- "%1"`, and nothing
//    after "--" is ever read as an option: not by laufey (ParseHostOptions),
//    not by Chromium (its command line parser has the same terminator), not
//    by the runtime.
//  * A packaged app ignores `--runtime`: its runtime library is the one next
//    to the executable (or named by LAUFEY_RUNTIME_PATH), never one a command
//    line names. IsPackagedLaunch decides.
//
// For CEF there is a third: a launch with a URL positional argument (a deep
// link) drops every Chromium switch that came from the process's own
// command line (DeepLinkSwitchesToStrip), so an old registration without the
// "--" still can't pass Chromium switches through a link.

#ifndef LAUFEY_LAUNCH_ARGS_H_
#define LAUFEY_LAUNCH_ARGS_H_

#include <string>
#include <vector>

namespace laufey_common {

// laufey's options, read from argv[1..].
struct HostOptions {
  // From `--runtime <path>` / `--runtime=<path>`; "" if not given (or
  // ignored).
  std::string runtime_path;
};

// Read laufey's options from `args` (argv without the program), stopping at
// the first "--". `packaged`: ignore `--runtime` (see IsPackagedLaunch).
HostOptions ParseHostOptions(const std::vector<std::string>& args,
                             bool packaged);

// Whether `arg` is an absolute URL with a scheme other than `file`: an RFC
// 3986 scheme (a letter, then letters, digits, '+', '-', '.') of at least two
// characters followed by ':'. A Windows drive path ("C:\x") is not one.
bool IsUrlArgument(const std::string& arg);

// Whether `args` (argv without the program) carry a deep link: a positional
// argument (anything after "--", or before it an argument not starting with
// '-') for which IsUrlArgument holds.
bool IsDeepLinkLaunch(const std::vector<std::string>& args);

// The names of the Chromium switches in `args` (argv without the program),
// as Chromium's parser would see them: arguments before "--" that start with
// "--" or "-" (and "/" on Windows), the name ending at '='; lower-cased on
// Windows, where Chromium lower-cases switch names. Each name once.
std::vector<std::string> CommandLineSwitchNames(
    const std::vector<std::string>& args);

// What a CEF browser process removes from its command line before Chromium
// reads it: empty unless IsDeepLinkLaunch(args), else every switch name from
// CommandLineSwitchNames(args) not in `allowed` (an allow-list: a link-started
// app needs no switch from its command line).
std::vector<std::string> DeepLinkSwitchesToStrip(
    const std::vector<std::string>& args,
    const std::vector<std::string>& allowed);

// Whether the running executable is a packaged app: its launch file
// (laufey-launch.json) exists, or `has_colocated_runtime` (the backend found
// a runtime library next to the executable).
bool IsPackagedLaunch(bool has_colocated_runtime);

// The process's own arguments (argv without the program, UTF-8), recorded by
// the host's main() before anything parses them (SetProcessArgs) and read by
// the CEF command line hook (ProcessArgs). Not thread-safe to set: main()
// sets them once, first.
void SetProcessArgs(std::vector<std::string> args);
const std::vector<std::string>& ProcessArgs();

}  // namespace laufey_common

#endif  // LAUFEY_LAUNCH_ARGS_H_
