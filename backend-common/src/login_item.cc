// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The portable half of launch at login (API 40): the entry's name and the
// XDG autostart file format. See laufey_system.h and
// docs/launch-at-login.md.

#include <sstream>

#include "laufey_launch_config.h"
#include "laufey_system.h"

namespace laufey_common {

std::string LoginItemName() {
  std::string id = LaunchAppId();
  if (!id.empty())
    return id;
  std::string exe = ExecutablePath();
  size_t slash = exe.find_last_of("/\\");
  std::string base = slash == std::string::npos ? exe : exe.substr(slash + 1);
#ifdef _WIN32
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos && dot > 0)
    base = base.substr(0, dot);
#endif
  return base;
}

std::string DesktopExecQuote(const std::string& path) {
  // Desktop Entry spec, "The Exec key": inside double quotes, '"', '`', '$'
  // and '\' are escaped with a backslash; a literal '%' is "%%". The value
  // is then a string value, whose own escape rules turn each '\' into "\\".
  std::string quoted = "\"";
  for (char c : path) {
    if (c == '"' || c == '`' || c == '$' || c == '\\')
      quoted += '\\';
    if (c == '%')
      quoted += '%';
    quoted += c;
  }
  quoted += '"';
  std::string value;
  for (char c : quoted) {
    if (c == '\\')
      value += '\\';
    value += c;
  }
  return value;
}

std::string BuildAutostartEntry(const std::string& name,
                                const std::string& exec_path) {
  // A display name can't span lines in a desktop entry.
  std::string display;
  for (char c : name)
    display += (c == '\n' || c == '\r') ? ' ' : c;
  std::ostringstream out;
  out << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Version=1.0\n"
      << "Name=" << display << "\n"
      << "Exec=" << DesktopExecQuote(exec_path) << "\n"
      << "Terminal=false\n"
      << "X-GNOME-Autostart-enabled=true\n";
  return out.str();
}

bool AutostartEntryEnabled(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  bool in_entry = false;
  bool enabled = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty() && line[0] == '[') {
      in_entry = line == "[Desktop Entry]";
      continue;
    }
    if (!in_entry)
      continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    while (!key.empty() && key.back() == ' ')
      key.pop_back();
    size_t b = value.find_first_not_of(' ');
    value = b == std::string::npos ? "" : value.substr(b);
    if (key == "Hidden" && value == "true")
      enabled = false;
    if (key == "X-GNOME-Autostart-enabled" && value == "false")
      enabled = false;
  }
  return enabled;
}

}  // namespace laufey_common
