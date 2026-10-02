// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Windows window state, size constraints, screens, display-change watching
// and the DWM backdrop on an HWND, shared by the WebView2 and CEF backends.
// See laufey_window.h.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>

#include "laufey_backend_common.h"
#include "laufey_window.h"

// Newer than some SDKs.
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
// Windows 11 21H2 (22000) only, undocumented: Mica on / off.
#define LAUFEY_DWMWA_MICA_EFFECT 1029

namespace laufey_common {

namespace {

// DWM_SYSTEMBACKDROP_TYPE values (22621+).
constexpr int kDwmsbtAuto = 0;
constexpr int kDwmsbtMainWindow = 2;       // Mica
constexpr int kDwmsbtTransientWindow = 3;  // Acrylic
constexpr int kDwmsbtTabbedWindow = 4;     // Mica Alt

struct FullscreenSave {
  LONG_PTR style = 0;
  LONG_PTR ex_style = 0;
  WINDOWPLACEMENT placement = {};
};

std::mutex g_fs_mutex;
std::map<uint32_t, FullscreenSave> g_fullscreen;

HWND g_display_watcher = nullptr;

struct Subclass {
  WNDPROC original = nullptr;
  uint32_t window_id = 0;
  void (*on_change)(uint32_t) = nullptr;
};
std::mutex g_subclass_mutex;
std::map<HWND, Subclass> g_subclasses;

LRESULT CALLBACK StateSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Subclass sc;
  {
    std::lock_guard<std::mutex> lock(g_subclass_mutex);
    auto it = g_subclasses.find(hwnd);
    if (it == g_subclasses.end())
      return DefWindowProcW(hwnd, msg, wp, lp);
    sc = it->second;
  }
  LRESULT result = CallWindowProcW(sc.original, hwnd, msg, wp, lp);
  if (msg == WM_SIZE && sc.on_change)
    sc.on_change(sc.window_id);
  if (msg == WM_NCDESTROY) {
    std::lock_guard<std::mutex> lock(g_subclass_mutex);
    g_subclasses.erase(hwnd);
  }
  return result;
}

LRESULT CALLBACK DisplayWatcherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_DISPLAYCHANGE:
      NotifyDisplayChanged();
      return 0;
    case WM_SETTINGCHANGE:
      // The work area changed (taskbar moved / resized / auto-hide).
      if (wp == SPI_SETWORKAREA)
        NotifyDisplayChanged();
      return 0;
    case WM_DPICHANGED:
      NotifyDisplayChanged();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto* out = reinterpret_cast<std::vector<laufey_screen_t>*>(data);
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi))
    return TRUE;
  laufey_screen_t s = {};
  std::string name = WideToUtf8(mi.szDevice);
  s.id = HashDisplayName(name.data(), name.size());
  s.x = mi.rcMonitor.left;
  s.y = mi.rcMonitor.top;
  s.width = mi.rcMonitor.right - mi.rcMonitor.left;
  s.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
  s.work_x = mi.rcWork.left;
  s.work_y = mi.rcWork.top;
  s.work_width = mi.rcWork.right - mi.rcWork.left;
  s.work_height = mi.rcWork.bottom - mi.rcWork.top;
  UINT dpi_x = 96, dpi_y = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)))
    dpi_x = 96;
  s.scale_factor = dpi_x / 96.0;
  s.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  out->push_back(s);
  return TRUE;
}

}  // namespace

void WinSetWindowState(void* hwnd_ptr, uint32_t window_id, int action) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd || !IsWindow(hwnd))
    return;
  bool fullscreen = WinIsFullscreen(window_id);
  switch (action) {
    case LAUFEY_WINDOW_ACTION_MAXIMIZE:
      if (!fullscreen)
        ShowWindow(hwnd, SW_MAXIMIZE);
      break;
    case LAUFEY_WINDOW_ACTION_UNMAXIMIZE:
      if (!fullscreen && IsZoomed(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
      break;
    case LAUFEY_WINDOW_ACTION_MINIMIZE:
      if (!IsIconic(hwnd))
        ShowWindow(hwnd, SW_MINIMIZE);
      break;
    case LAUFEY_WINDOW_ACTION_RESTORE:
      if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
      break;
    case LAUFEY_WINDOW_ACTION_ENTER_FULLSCREEN: {
      if (fullscreen)
        break;
      if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
      FullscreenSave save;
      save.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      save.ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
      save.placement.length = sizeof(save.placement);
      GetWindowPlacement(hwnd, &save.placement);
      MONITORINFO mi = {};
      mi.cbSize = sizeof(mi);
      if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                           &mi))
        break;
      {
        std::lock_guard<std::mutex> lock(g_fs_mutex);
        g_fullscreen[window_id] = save;
      }
      // Borderless and covering the whole monitor, taskbar included. The
      // flag is recorded first so the WM_SIZE this causes reports it.
      SetWindowLongPtrW(hwnd, GWL_STYLE,
                        save.style & ~(WS_CAPTION | WS_THICKFRAME));
      SetWindowLongPtrW(
          hwnd, GWL_EXSTYLE,
          save.ex_style & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                            WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
      SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top,
                   SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
      break;
    }
    case LAUFEY_WINDOW_ACTION_LEAVE_FULLSCREEN: {
      FullscreenSave save;
      {
        std::lock_guard<std::mutex> lock(g_fs_mutex);
        auto it = g_fullscreen.find(window_id);
        if (it == g_fullscreen.end())
          break;
        save = it->second;
        g_fullscreen.erase(it);
      }
      SetWindowLongPtrW(hwnd, GWL_STYLE, save.style);
      SetWindowLongPtrW(hwnd, GWL_EXSTYLE, save.ex_style);
      SetWindowPlacement(hwnd, &save.placement);
      SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                       SWP_FRAMECHANGED);
      break;
    }
    default:
      return;
  }
  // Report even when the OS sent no WM_SIZE (a hidden window, a no-op).
  ReportWindowState(window_id, WinGetWindowState(hwnd, window_id));
}

uint32_t WinGetWindowState(void* hwnd_ptr, uint32_t window_id) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd || !IsWindow(hwnd))
    return 0;
  uint32_t state = 0;
  if (WinIsFullscreen(window_id))
    state |= LAUFEY_WINDOW_STATE_FULLSCREEN;
  else if (IsZoomed(hwnd))
    state |= LAUFEY_WINDOW_STATE_MAXIMIZED;
  if (IsIconic(hwnd))
    state |= LAUFEY_WINDOW_STATE_MINIMIZED;
  return state;
}

bool WinIsFullscreen(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_fs_mutex);
  return g_fullscreen.count(window_id) > 0;
}

bool WinApplyMinMaxInfo(void* minmaxinfo, const SizeConstraints& c,
                        double units_to_pixels, int chrome_width,
                        int chrome_height) {
  auto* mmi = static_cast<MINMAXINFO*>(minmaxinfo);
  if (!mmi)
    return false;
  bool applied = false;
  auto px = [&](int v) { return static_cast<LONG>(v * units_to_pixels + 0.5); };
  if (c.min_width > 0) {
    mmi->ptMinTrackSize.x = px(c.min_width) + chrome_width;
    applied = true;
  }
  if (c.min_height > 0) {
    mmi->ptMinTrackSize.y = px(c.min_height) + chrome_height;
    applied = true;
  }
  if (c.max_width > 0) {
    mmi->ptMaxTrackSize.x = px(c.max_width) + chrome_width;
    applied = true;
  }
  if (c.max_height > 0) {
    mmi->ptMaxTrackSize.y = px(c.max_height) + chrome_height;
    applied = true;
  }
  return applied;
}

bool WinGetNormalRect(void* hwnd_ptr, uint32_t window_id, int* x, int* y,
                      int* width, int* height) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd || !IsWindow(hwnd))
    return false;
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof(wp);
  bool have = false;
  {
    std::lock_guard<std::mutex> lock(g_fs_mutex);
    auto it = g_fullscreen.find(window_id);
    if (it != g_fullscreen.end()) {
      wp = it->second.placement;
      have = true;
    }
  }
  if (!have && !GetWindowPlacement(hwnd, &wp))
    return false;
  RECT r = wp.rcNormalPosition;
  // rcNormalPosition is in workspace coordinates (relative to the work area
  // of the monitor); screen coordinates differ by the taskbar when it is on
  // the top or left.
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) {
    LONG dx = mi.rcWork.left - mi.rcMonitor.left;
    LONG dy = mi.rcWork.top - mi.rcMonitor.top;
    OffsetRect(&r, dx, dy);
  }
  if (x)
    *x = r.left;
  if (y)
    *y = r.top;
  if (width)
    *width = r.right - r.left;
  if (height)
    *height = r.bottom - r.top;
  return true;
}

std::vector<laufey_screen_t> WinGetScreens() {
  std::vector<laufey_screen_t> screens;
  EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                      reinterpret_cast<LPARAM>(&screens));
  std::stable_partition(screens.begin(), screens.end(),
                        [](const laufey_screen_t& s) { return s.is_primary; });
  return screens;
}

int64_t WinScreenForWindow(void* hwnd_ptr) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd || !IsWindow(hwnd))
    return 0;
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
    return 0;
  std::string name = WideToUtf8(mi.szDevice);
  return HashDisplayName(name.data(), name.size());
}

void WinInstallDisplayWatcher() {
  if (g_display_watcher)
    return;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DisplayWatcherProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"LaufeyDisplayWatcher";
  RegisterClassExW(&wc);
  // A top-level window (never shown): WM_DISPLAYCHANGE and WM_SETTINGCHANGE
  // are broadcast to top-level windows only, so a message-only window would
  // miss them.
  g_display_watcher =
      CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0,
                      0, 0, nullptr, nullptr, wc.hInstance, nullptr);
}

uint32_t WinBuildNumber() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  static uint32_t build = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
      return 0u;
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn)
      return 0u;
    RTL_OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn(&info) != 0)
      return 0u;
    return static_cast<uint32_t>(info.dwBuildNumber);
  }();
  return build;
}

uint32_t WinBackdropCapabilities() {
  uint32_t build = WinBuildNumber();
  if (build >= 22621) {
    return LAUFEY_WINDOW_CAP_BACKDROP_MICA |
           LAUFEY_WINDOW_CAP_BACKDROP_ACRYLIC |
           LAUFEY_WINDOW_CAP_BACKDROP_MICA_ALT;
  }
  if (build >= 22000)
    return LAUFEY_WINDOW_CAP_BACKDROP_MICA;
  return 0;
}

bool WinSetBackdrop(void* hwnd_ptr, int backdrop) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd || !IsWindow(hwnd))
    return false;
  uint32_t caps = WinBackdropCapabilities();
  uint32_t build = WinBuildNumber();
  int type = kDwmsbtAuto;
  switch (backdrop) {
    case LAUFEY_BACKDROP_NONE:
      break;
    case LAUFEY_BACKDROP_MICA:
      if (!(caps & LAUFEY_WINDOW_CAP_BACKDROP_MICA))
        return false;
      type = kDwmsbtMainWindow;
      break;
    case LAUFEY_BACKDROP_ACRYLIC:
      if (!(caps & LAUFEY_WINDOW_CAP_BACKDROP_ACRYLIC))
        return false;
      type = kDwmsbtTransientWindow;
      break;
    case LAUFEY_BACKDROP_MICA_ALT:
      if (!(caps & LAUFEY_WINDOW_CAP_BACKDROP_MICA_ALT))
        return false;
      type = kDwmsbtTabbedWindow;
      break;
    default:
      return false;
  }
  if (backdrop != LAUFEY_BACKDROP_NONE && caps == 0)
    return false;
  // The backdrop shows through the client area only where the frame is
  // extended into it and nothing opaque is painted on top.
  MARGINS margins = {0, 0, 0, 0};
  if (backdrop != LAUFEY_BACKDROP_NONE)
    margins = {-1, -1, -1, -1};
  HRESULT hr = S_OK;
  if (build >= 22621) {
    hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type,
                               sizeof(type));
  } else if (build >= 22000) {
    BOOL on = backdrop == LAUFEY_BACKDROP_MICA ? TRUE : FALSE;
    hr = DwmSetWindowAttribute(hwnd, LAUFEY_DWMWA_MICA_EFFECT, &on, sizeof(on));
  } else if (backdrop == LAUFEY_BACKDROP_NONE) {
    return true;
  }
  if (FAILED(hr))
    return false;
  DwmExtendFrameIntoClientArea(hwnd, &margins);
  InvalidateRect(hwnd, nullptr, TRUE);
  return true;
}

void WinSubclassForStateChanges(void* hwnd_ptr, uint32_t window_id,
                                void (*on_change)(uint32_t window_id)) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd || !on_change)
    return;
  std::lock_guard<std::mutex> lock(g_subclass_mutex);
  if (g_subclasses.count(hwnd))
    return;
  Subclass sc;
  sc.window_id = window_id;
  sc.on_change = on_change;
  sc.original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(StateSubclassProc)));
  if (sc.original)
    g_subclasses[hwnd] = sc;
}

void WinUnsubclassForStateChanges(void* hwnd_ptr) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  std::lock_guard<std::mutex> lock(g_subclass_mutex);
  auto it = g_subclasses.find(hwnd);
  if (it == g_subclasses.end())
    return;
  // Only put the original back if nobody subclassed on top of us since.
  if (IsWindow(hwnd) && GetWindowLongPtrW(hwnd, GWLP_WNDPROC) ==
                            reinterpret_cast<LONG_PTR>(StateSubclassProc)) {
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(it->second.original));
  }
  g_subclasses.erase(it);
}

}  // namespace laufey_common
