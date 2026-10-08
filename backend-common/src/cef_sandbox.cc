// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_cef_sandbox.h"

#include <sys/stat.h>

#if defined(__linux__)
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#endif

namespace laufey_common {

LinuxSandboxDecision DecideLinuxSandbox(const LinuxSandboxFacts& facts) {
  LinuxSandboxDecision decision;
  if (facts.running_as_root) {
    // Chromium exits rather than start sandboxed as root.
    decision.reason =
        "running as root (Chromium does not support its sandbox for root)";
    return decision;
  }
  if (facts.user_namespaces == UserNamespaceProbe::kAvailable) {
    decision.mode = LinuxSandboxMode::kNamespace;
    decision.reason = "unprivileged user namespaces";
    return decision;
  }
  if (facts.helper.usable) {
    decision.mode = LinuxSandboxMode::kSetuid;
    decision.reason =
        facts.user_namespaces == UserNamespaceProbe::kFailed
            ? "the chrome-sandbox helper next to the executable (the "
              "user-namespace probe failed: " +
                  facts.probe_problem + ")"
            : "the chrome-sandbox helper next to the executable";
    return decision;
  }
  if (facts.user_namespaces == UserNamespaceProbe::kFailed) {
    // Unknown is not "no user namespaces": a transient failure (EAGAIN at
    // RLIMIT_NPROC) must not turn the sandbox off. Chromium probes again
    // and chooses.
    decision.mode = LinuxSandboxMode::kChromium;
    decision.reason = "the user-namespace probe failed (" +
                      facts.probe_problem + "); Chromium chooses";
    return decision;
  }
  std::string reason =
      facts.apparmor_restricts_user_namespaces
          ? "unprivileged user namespaces are restricted by AppArmor "
            "(kernel.apparmor_restrict_unprivileged_userns=1)"
          : "unprivileged user namespaces are not available";
  if (facts.helper.present) {
    reason += ", and the chrome-sandbox helper is not usable (" +
              facts.helper.problem + ")";
  } else {
    reason += ", and there is no chrome-sandbox helper next to the executable";
  }
  reason +=
      "; install the app from its .deb or .rpm package to run web content "
      "sandboxed";
  decision.reason = reason;
  return decision;
}

const char* LinuxSandboxModeName(LinuxSandboxMode mode) {
  switch (mode) {
    case LinuxSandboxMode::kNamespace:
      return "namespace";
    case LinuxSandboxMode::kSetuid:
      return "setuid";
    case LinuxSandboxMode::kChromium:
      return "chromium";
    case LinuxSandboxMode::kOff:
      break;
  }
  return "off";
}

bool RefuseUnsandboxedStart(const LinuxSandboxDecision& decision,
                            bool require_sandbox, std::string* message) {
  if (!require_sandbox || decision.enabled())
    return false;
  if (message) {
    *message =
        "laufey: this app requires the Chromium sandbox (requireSandbox), "
        "which is not available here: " +
        decision.reason;
  }
  return true;
}

SetuidHelperState InspectSetuidHelper(const std::string& path) {
  SetuidHelperState state;
#if !defined(_WIN32)
  struct stat st;
  if (path.empty() || stat(path.c_str(), &st) != 0) {
    return state;
  }
  state.present = true;
  if (!S_ISREG(st.st_mode)) {
    state.problem = "not a regular file";
  } else if (st.st_uid != 0) {
    state.problem = "not owned by root";
  } else if (!(st.st_mode & S_ISUID)) {
    state.problem = "not setuid";
  } else if (!(st.st_mode & S_IXOTH)) {
    state.problem = "not executable";
  } else {
    state.usable = true;
  }
#else
  (void)path;
#endif
  return state;
}

void ApplySetuidHelperBlockers(SetuidHelperState* helper, bool no_new_privs,
                               bool nosuid_mount) {
  if (!helper->usable) {
    return;
  }
  if (no_new_privs) {
    helper->usable = false;
    helper->problem =
        "this process runs with no_new_privs, so a setuid helper can't gain "
        "root";
  } else if (nosuid_mount) {
    helper->usable = false;
    helper->problem = "the executable's file system is mounted nosuid";
  }
}

#if defined(__linux__)

namespace {

// Async-signal-safe write of a short string to a /proc file.
bool WriteProcFile(const char* path, const char* data) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  size_t len = strlen(data);
  ssize_t written = write(fd, data, len);
  close(fd);
  return written == static_cast<ssize_t>(len);
}

// Formats "<id> <id> 1\n" without the allocator or stdio locks (this runs in
// a forked child of a threaded process).
void FormatIdMap(char* out, size_t size, unsigned id) {
  char digits[16];
  int n = 0;
  do {
    digits[n++] = static_cast<char>('0' + id % 10);
    id /= 10;
  } while (id && n < 15);
  size_t pos = 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = n - 1; i >= 0 && pos + 1 < size; --i) {
      out[pos++] = digits[i];
    }
    if (pos + 1 < size) {
      out[pos++] = ' ';
    }
  }
  const char tail[] = "1\n";
  for (size_t i = 0; tail[i] && pos + 1 < size; ++i) {
    out[pos++] = tail[i];
  }
  out[pos] = '\0';
}

}  // namespace

UserNamespaceProbe ProbeUserNamespaces(std::string* problem) {
  struct stat st;
  if (stat("/proc/self/ns/user", &st) != 0) {
    return UserNamespaceProbe::kUnavailable;
  }
  const uid_t uid = getuid();
  const gid_t gid = getgid();
  char uid_map[48];
  char gid_map[48];
  FormatIdMap(uid_map, sizeof(uid_map), static_cast<unsigned>(uid));
  FormatIdMap(gid_map, sizeof(gid_map), static_cast<unsigned>(gid));

  // What Chromium's probe does: a child in a new user namespace (raw clone,
  // like Chromium's ForkWithFlags: no glibc atfork handlers run in it), the
  // caller's ids mapped into it, then a nested one (which AppArmor's
  // unprivileged_userns profile refuses, as some kernels refuse it by
  // sysctl).
  long pid = syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0);
  if (pid < 0) {
    const int err = errno;
    // The kernel's answers to "no user namespaces for you".
    if (err == EPERM || err == EINVAL || err == ENOSPC || err == EUSERS) {
      return UserNamespaceProbe::kUnavailable;
    }
    if (problem) {
      *problem = std::string("clone: ") + std::strerror(err);
    }
    return UserNamespaceProbe::kFailed;
  }
  if (pid == 0) {
    // setgroups must be denied before an unprivileged gid_map write; the
    // file is missing on kernels older than 3.19, where it isn't needed.
    int fd = open("/proc/self/setgroups", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      const char deny[] = "deny";
      ssize_t ignored = write(fd, deny, sizeof(deny) - 1);
      (void)ignored;
      close(fd);
    }
    if (!WriteProcFile("/proc/self/uid_map", uid_map) ||
        !WriteProcFile("/proc/self/gid_map", gid_map)) {
      _exit(1);
    }
    if (unshare(CLONE_NEWUSER) != 0) {
      _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  while (waitpid(static_cast<pid_t>(pid), &status, 0) < 0) {
    if (errno != EINTR) {
      if (problem) {
        *problem = std::string("waitpid: ") + std::strerror(errno);
      }
      return UserNamespaceProbe::kFailed;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status) == 0 ? UserNamespaceProbe::kAvailable
                                    : UserNamespaceProbe::kUnavailable;
  }
  if (problem) {
    *problem = "the probe child was killed by signal " +
               std::to_string(WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  }
  return UserNamespaceProbe::kFailed;
}

LinuxSandboxFacts ProbeLinuxSandbox(const std::string& exe_dir) {
  LinuxSandboxFacts facts;
  // Chromium's own check (the real uid, not the effective one).
  facts.running_as_root = getuid() == 0;
  if (facts.running_as_root) {
    return facts;
  }
  facts.user_namespaces = ProbeUserNamespaces(&facts.probe_problem);
  if (facts.user_namespaces == UserNamespaceProbe::kUnavailable) {
    if (FILE* f = std::fopen(
            "/proc/sys/kernel/apparmor_restrict_unprivileged_userns", "re")) {
      facts.apparmor_restricts_user_namespaces = std::fgetc(f) == '1';
      std::fclose(f);
    }
  }
  facts.helper = InspectSetuidHelper(
      exe_dir.empty() ? std::string() : exe_dir + "/chrome-sandbox");
  struct statvfs vfs;
  const bool nosuid = !exe_dir.empty() && statvfs(exe_dir.c_str(), &vfs) == 0 &&
                      (vfs.f_flag & ST_NOSUID) != 0;
  ApplySetuidHelperBlockers(
      &facts.helper, prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1, nosuid);
  return facts;
}

#endif  // defined(__linux__)

}  // namespace laufey_common
