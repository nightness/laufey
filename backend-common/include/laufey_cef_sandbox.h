// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Chromium sandbox of the CEF backend on Linux: whether the host can turn
// it on, and with which layer-1 sandbox. See docs/backends.md, "The Chromium
// sandbox".
//
// Chromium's renderer, GPU and utility processes run under seccomp-bpf plus a
// layer-1 sandbox that puts them in their own namespaces. Layer 1 is either
// the namespace sandbox (unprivileged user namespaces) or the setuid helper
// `chrome-sandbox` next to the executable (root-owned, mode 4755, which only a
// system package install can arrange). With neither, Chromium aborts at start
// ("No usable sandbox!"), and as root it refuses to start sandboxed at all.
// Ubuntu 23.10 and later restrict unprivileged user namespaces with AppArmor
// (kernel.apparmor_restrict_unprivileged_userns), so a tarball or AppImage run
// by the user there has neither.
//
// The host therefore probes what Chromium would find, the same way Chromium
// does, before CefInitialize: with a usable sandbox it runs sandboxed, and
// without one it turns the sandbox off, instead of failing to start. Either
// way it logs one line, `laufey: sandbox: <mode> (<reason>)`. The decision
// depends only on the machine and the installed files, never on the command
// line, so a deep link can't influence it (Chromium's own
// --no-sandbox switch is one of the switches a deep-link launch drops; see
// laufey_launch_args.h).

#ifndef LAUFEY_CEF_SANDBOX_H_
#define LAUFEY_CEF_SANDBOX_H_

#include <string>

namespace laufey_common {

// The setuid sandbox helper (`chrome-sandbox` next to the executable).
struct SetuidHelperState {
  bool present = false;
  // Owned by root, setuid, executable: what Chromium requires before it uses
  // the helper. A helper that is present but not usable makes Chromium abort
  // when it has to fall back to it, so it counts as absent.
  bool usable = false;
  // Why a present helper is not usable ("" when usable or absent).
  std::string problem;
};

// What the user-namespace probe found.
enum class UserNamespaceProbe {
  kUnavailable,  // the kernel or AppArmor refused (Chromium would too)
  kAvailable,    // a child created a user namespace and a nested one
  kFailed,       // no answer: the probe child couldn't be started or reaped
};

// What the host learned about the machine.
struct LinuxSandboxFacts {
  bool running_as_root = false;
  // A child could create a user namespace, map its ids and create a nested
  // one: Chromium's CanCreateProcessInNewUserNS.
  UserNamespaceProbe user_namespaces = UserNamespaceProbe::kUnavailable;
  // Why the probe failed (kFailed only).
  std::string probe_problem;
  // kernel.apparmor_restrict_unprivileged_userns is 1 (only used to explain a
  // missing namespace sandbox).
  bool apparmor_restricts_user_namespaces = false;
  SetuidHelperState helper;
};

enum class LinuxSandboxMode {
  kNamespace,  // unprivileged user namespaces
  kSetuid,     // the chrome-sandbox helper
  kChromium,   // the probe failed: sandbox on, Chromium picks the layer
  kOff,        // CefSettings::no_sandbox
};

struct LinuxSandboxDecision {
  LinuxSandboxMode mode = LinuxSandboxMode::kOff;
  // Why this mode: what the host found. Logged as
  // `laufey: sandbox: <mode> (<reason>)`.
  std::string reason;

  bool enabled() const {
    return mode != LinuxSandboxMode::kOff;
  }
};

// Chromium's choice, made ahead of it: the namespace sandbox when user
// namespaces work, else the setuid helper when it is usable, else off. Root
// is always off. A probe that couldn't run decides nothing: the sandbox stays
// on and Chromium chooses (it may then abort, as it would without laufey).
LinuxSandboxDecision DecideLinuxSandbox(const LinuxSandboxFacts& facts);

// "namespace", "setuid", "chromium" or "off".
const char* LinuxSandboxModeName(LinuxSandboxMode mode);

// Whether the host refuses to start: the sandbox would be off and the app
// requires it (LaunchRequireSandbox: "requireSandbox" in the launch file, or
// LAUFEY_REQUIRE_SANDBOX). `message` gets the line to print. A machine where
// the sandbox is on ("chromium" included) always starts.
bool RefuseUnsandboxedStart(const LinuxSandboxDecision& decision,
                            bool require_sandbox, std::string* message);
// The exit code of that refusal (sysexits' EX_CONFIG: the machine's
// configuration, not the app, is the problem).
constexpr int kSandboxRequiredExitCode = 78;

// Checks `path` as Chromium checks its setuid helper.
SetuidHelperState InspectSetuidHelper(const std::string& path);

// A helper that passes InspectSetuidHelper still can't gain root when this
// process has no_new_privs set (a container, a systemd unit with
// NoNewPrivileges=, a parent sandbox) or the executable's file system is
// mounted nosuid (an AppImage's FUSE mount, a nosuid /home): marks such a
// helper not usable, with the reason.
void ApplySetuidHelperBlockers(SetuidHelperState* helper, bool no_new_privs,
                               bool nosuid_mount);

#if defined(__linux__)
// Starts a child in a new user namespace (raw clone(CLONE_NEWUSER), as
// Chromium's CanCreateProcessInNewUserNS does, so no atfork handlers run)
// that maps its ids and creates a nested one. Safe to call from a threaded
// process: the child only makes system calls and exits. `problem` gets the
// reason of a kFailed result.
UserNamespaceProbe ProbeUserNamespaces(std::string* problem);

// Probes the running machine; `exe_dir` is the directory of the executable
// (where Chromium looks for chrome-sandbox).
LinuxSandboxFacts ProbeLinuxSandbox(const std::string& exe_dir);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_CEF_SANDBOX_H_
