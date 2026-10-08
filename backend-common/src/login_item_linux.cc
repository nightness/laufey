// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Launch at login on Linux (API 40): an XDG autostart entry,
// $XDG_CONFIG_HOME/autostart/<name>.desktop (~/.config/autostart by
// default), per the XDG Autostart specification, which GNOME, KDE, Xfce and
// most other desktops honor. It starts the running executable ($APPIMAGE for
// an AppImage, trusted only while this executable is inside its mount,
// $APPDIR). An entry the user turned off (Hidden=true, or GNOME's
// X-GNOME-Autostart-enabled=false) reports DISABLED; enabling rewrites it.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "laufey_backend_common.h"
#include "laufey_launch_config.h"
#include "laufey_system.h"

namespace laufey_common {

namespace {

std::string AutostartDir() {
  std::string base = GetEnvUtf8("XDG_CONFIG_HOME");
  if (base.empty() || base[0] != '/') {
    std::string home = GetEnvUtf8("HOME");
    if (home.empty() || home[0] != '/')
      return "";
    base = home + "/.config";
  }
  return base + "/autostart";
}

std::string EntryPath() {
  std::string name = LoginItemName();
  std::string dir = AutostartDir();
  if (name.empty() || dir.empty() || !IsSafeAppId(name))
    return "";
  return dir + "/" + name + ".desktop";
}

// What the entry starts: the AppImage (the mounted executable disappears
// when it exits), trusted only from inside its mount, else this executable
// (RelaunchExecutablePath).
std::string ExecPath() {
  return RelaunchExecutablePath();
}

bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

// Writes `text` to `path` through a temporary file and rename(2), so a
// crash never leaves a half-written entry.
bool WriteFileAtomic(const std::string& path, const std::string& text,
                     std::string* error) {
  std::string tmp = path + ".tmp." + std::to_string(getpid());
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    *error = "cannot create " + tmp + ": " + strerror(errno);
    return false;
  }
  size_t done = 0;
  while (done < text.size()) {
    ssize_t n = write(fd, text.data() + done, text.size() - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      *error = "cannot write " + tmp + ": " + strerror(errno);
      close(fd);
      unlink(tmp.c_str());
      return false;
    }
    done += static_cast<size_t>(n);
  }
  if (fsync(fd) != 0 || close(fd) != 0) {
    *error = "cannot write " + tmp + ": " + strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    *error = "cannot rename to " + path + ": " + strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace

int GetLaunchAtLogin() {
  std::string path = EntryPath();
  if (path.empty())
    return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
  std::string text;
  if (!ReadFile(path, &text))
    return LAUFEY_LOGIN_ITEM_DISABLED;
  return AutostartEntryEnabled(text) ? LAUFEY_LOGIN_ITEM_ENABLED
                                     : LAUFEY_LOGIN_ITEM_DISABLED;
}

int SetLaunchAtLogin(bool enabled, std::string* error) {
  std::string path = EntryPath();
  std::string exec = ExecPath();
  if (path.empty() || exec.empty())
    return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
  std::string err;
  if (enabled) {
    std::string name = GetEnvUtf8("LAUFEY_APP_NAME");
    if (name.empty())
      name = LoginItemName();
    if (!EnsureDirectory(AutostartDir())) {
      err = "cannot create " + AutostartDir();
    } else if (WriteFileAtomic(path, BuildAutostartEntry(name, exec), &err)) {
      return GetLaunchAtLogin();
    }
  } else {
    if (unlink(path.c_str()) == 0 || errno == ENOENT)
      return GetLaunchAtLogin();
    err = "cannot remove " + path + ": " + strerror(errno);
  }
  if (error)
    *error = err;
  return LAUFEY_LOGIN_ITEM_FAILED;
}

}  // namespace laufey_common
