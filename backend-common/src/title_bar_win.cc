// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Title bar preferences (API 47) on Windows: the caption buttons are always
// on the right (minimize, maximize, close) and a double click on a caption
// maximizes or restores; the colour scheme is the app colour mode
// (Personalize\AppsUseLightTheme) and the accent colour DWM's AccentColor.
// The change handler fires on a watcher thread of its own when either
// registry key changes and the answer differs from the last one reported.

#include "laufey_title_bar.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace laufey_common {

namespace {

constexpr wchar_t kPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kDwmKey[] = L"Software\\Microsoft\\Windows\\DWM";

bool ReadDword(const wchar_t* key, const wchar_t* name, DWORD* out) {
  DWORD size = sizeof(*out);
  return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr,
                      out, &size) == ERROR_SUCCESS;
}

std::string ColorScheme() {
  DWORD light = 1;
  if (!ReadDword(kPersonalizeKey, L"AppsUseLightTheme", &light))
    return "light";  // the default when the key was never written
  return light ? "light" : "dark";
}

std::string AccentColor() {
  DWORD abgr = 0;  // 0xAABBGGRR
  if (!ReadDword(kDwmKey, L"AccentColor", &abgr))
    return "";
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                static_cast<unsigned>(abgr & 0xff),
                static_cast<unsigned>((abgr >> 8) & 0xff),
                static_cast<unsigned>((abgr >> 16) & 0xff));
  return buf;
}

std::mutex g_mutex;
void (*g_handler)(void*) = nullptr;
void* g_user_data = nullptr;
std::string g_last_json;
bool g_watching = false;

// Waits on both keys and reports a change that alters the answer. The
// thread lives as long as the process (a handler can be set again).
void Watch() {
  HKEY keys[2] = {nullptr, nullptr};
  RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_NOTIFY, &keys[0]);
  RegOpenKeyExW(HKEY_CURRENT_USER, kDwmKey, 0, KEY_NOTIFY, &keys[1]);
  HANDLE events[2] = {CreateEventW(nullptr, FALSE, FALSE, nullptr),
                      CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  for (;;) {
    DWORD count = 0;
    HANDLE waits[2];
    for (int i = 0; i < 2; ++i) {
      if (keys[i] && events[i] &&
          RegNotifyChangeKeyValue(keys[i], FALSE, REG_NOTIFY_CHANGE_LAST_SET,
                                  events[i], TRUE) == ERROR_SUCCESS) {
        waits[count++] = events[i];
      }
    }
    if (count == 0)
      return;  // nothing to follow
    WaitForMultipleObjects(count, waits, FALSE, INFINITE);
    std::string json = TitleBarPreferencesToJson(ProbeTitleBarPreferences());
    void (*handler)(void*) = nullptr;
    void* user_data = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (json == g_last_json)
        continue;
      g_last_json = json;
      handler = g_handler;
      user_data = g_user_data;
    }
    if (handler)
      handler(user_data);
  }
}

}  // namespace

TitleBarPreferences ProbeTitleBarPreferences() {
  TitleBarPreferences p;
  p.right = {"minimize", "maximize", "close"};
  p.double_click = "maximize";
  p.color_scheme = ColorScheme();
  p.accent_color = AccentColor();
  p.source = "os";
  return p;
}

void SetTitleBarPreferencesChangedHandler(void (*handler)(void* user_data),
                                          void* user_data) {
  bool start = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handler = handler;
    g_user_data = user_data;
    start = handler && !g_watching;
    if (start)
      g_watching = true;
  }
  if (!start)
    return;
  std::string json = TitleBarPreferencesToJson(ProbeTitleBarPreferences());
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_last_json = json;
  }
  std::thread(Watch).detach();
}

void ResetTitleBarPreferencesForTesting() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handler = nullptr;
  g_user_data = nullptr;
}

}  // namespace laufey_common
