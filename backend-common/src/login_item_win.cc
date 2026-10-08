// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Launch at login on Windows (API 40): a value under
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run named after the app
// (LoginItemName), holding the quoted path of the running executable. No
// administrator rights are needed. The user can turn an entry off in Task
// Manager / Settings > Apps > Startup without deleting it; Explorer records
// that under ...\Explorer\StartupApproved\Run (the first byte of the value is
// odd when disabled). Such an entry reports REQUIRES_APPROVAL, and enabling
// does not override the user's choice.

#include <windows.h>

#include <vector>

#include "laufey_backend_common.h"
#include "laufey_launch_config.h"
#include "laufey_system.h"

namespace laufey_common {

namespace {

const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved"
    L"\\Run";

std::wstring ExePath() {
  std::vector<wchar_t> buf(MAX_PATH);
  while (true) {
    DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                 static_cast<DWORD>(buf.size()));
    if (n == 0)
      return L"";
    if (n < buf.size())
      return std::wstring(buf.data(), n);
    if (buf.size() >= 32768)
      return L"";
    buf.resize(buf.size() * 2);
  }
}

std::wstring Command() {
  std::wstring exe = ExePath();
  return exe.empty() ? L"" : L"\"" + exe + L"\"";
}

// The Run value, or false when there is none.
bool ReadRunValue(const std::wstring& name, std::wstring* out) {
  DWORD type = 0, size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, name.c_str(),
                   RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type,
                   nullptr, &size) != ERROR_SUCCESS ||
      size == 0) {
    return false;
  }
  std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1);
  if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, name.c_str(),
                   RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type,
                   buf.data(), &size) != ERROR_SUCCESS) {
    return false;
  }
  *out = std::wstring(buf.data());
  return true;
}

// Whether the user turned the entry off (StartupApproved\Run, first byte
// odd: 0x03 / 0x01 off, 0x02 / 0x06 on).
bool DisabledByUser(const std::wstring& name) {
  BYTE data[16] = {};
  DWORD size = sizeof(data);
  if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, name.c_str(),
                   RRF_RT_REG_BINARY, nullptr, data,
                   &size) != ERROR_SUCCESS ||
      size == 0) {
    return false;
  }
  return (data[0] & 1) != 0;
}

bool SameCommand(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) ==
         CSTR_EQUAL;
}

}  // namespace

int GetLaunchAtLogin() {
  std::wstring name = Utf8ToWide(LoginItemName());
  std::wstring command = Command();
  if (name.empty() || command.empty())
    return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
  std::wstring value;
  // A value under our name that starts another executable isn't ours to
  // report (an older install elsewhere); enabling replaces it.
  if (!ReadRunValue(name, &value) || !SameCommand(value, command))
    return LAUFEY_LOGIN_ITEM_DISABLED;
  return DisabledByUser(name) ? LAUFEY_LOGIN_ITEM_REQUIRES_APPROVAL
                              : LAUFEY_LOGIN_ITEM_ENABLED;
}

int SetLaunchAtLogin(bool enabled, std::string* error) {
  std::wstring name = Utf8ToWide(LoginItemName());
  std::wstring command = Command();
  if (name.empty() || command.empty())
    return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
  if (enabled) {
    LSTATUS st = RegSetKeyValueW(
        HKEY_CURRENT_USER, kRunKey, name.c_str(), REG_SZ, command.c_str(),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (st != ERROR_SUCCESS) {
      if (error)
        *error = "RegSetKeyValue(HKCU\\...\\Run) failed: error " +
                 std::to_string(st);
      return LAUFEY_LOGIN_ITEM_FAILED;
    }
  } else {
    std::wstring value;
    // Leave a value that starts another executable alone.
    if (ReadRunValue(name, &value) && SameCommand(value, command)) {
      LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, name.c_str());
      if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
        if (error)
          *error = "RegDeleteKeyValue(HKCU\\...\\Run) failed: error " +
                   std::to_string(st);
        return LAUFEY_LOGIN_ITEM_FAILED;
      }
    }
  }
  return GetLaunchAtLogin();
}

}  // namespace laufey_common
