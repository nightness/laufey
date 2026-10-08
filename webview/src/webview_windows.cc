// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_platform_features.h"
#include "laufey_launch_args.h"
#include "laufey_launch_config.h"
#include "laufey_system.h"
#include "laufey_single_instance.h"
#include "laufey_bridge_origin.h"
#include "laufey_json.h"
#include "laufey_passkey.h"
#include "laufey_scheme_registry.h"
#include "laufey_ui_tasks.h"
#include "laufey_window.h"
#include "init_script.h"
#include "wv2_scheme_stream.h"
#include <win32_menu.h>
#include "laufey_notifications.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <wrl.h>

// windows.h defines CreateWindow as a macro which conflicts with
// LaufeyBackend::CreateWindow
#undef CreateWindow

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

// WebView2 headers
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

#include <shlwapi.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <map>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <thread>
#include <algorithm>
#include <cmath>

using namespace Microsoft::WRL;

namespace keyboard {

// VK → W3C key/code mapping lives in backend-common
// (laufey_common::VkToKey / VkToCode). These thin wrappers extract
// the Windows-only state (GetKeyState / lParam scancode) and forward.
inline std::string VirtualKeyToKey(WPARAM vk, LPARAM /*lParam*/) {
  bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  bool caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
  return laufey_common::VkToKey(static_cast<int>(vk), 0, shift, caps);
}

inline std::string VirtualKeyToCode(WPARAM vk, LPARAM lParam) {
  bool is_extended = (lParam & (1 << 24)) != 0;
  uint32_t scancode = static_cast<uint32_t>((lParam >> 16) & 0xFF);
  return laufey_common::VkToCode(static_cast<int>(vk), is_extended, scancode);
}

inline uint32_t GetLaufeyModifiers() {
  uint32_t modifiers = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000)
    modifiers |= LAUFEY_MOD_SHIFT;
  if (GetKeyState(VK_CONTROL) & 0x8000)
    modifiers |= LAUFEY_MOD_CONTROL;
  if (GetKeyState(VK_MENU) & 0x8000)
    modifiers |= LAUFEY_MOD_ALT;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
    modifiers |= LAUFEY_MOD_META;
  return modifiers;
}

}  // namespace keyboard

using laufey_common::Utf8ToWide;
using laufey_common::WideToUtf8;

// HWND → laufey_id mapping
static std::map<HWND, uint32_t> g_hwnd_to_laufey_id;
static std::recursive_mutex g_hwnd_mutex;

static uint32_t LaufeyIdForHwnd(HWND hwnd) {
  if (!hwnd)
    return 0;
  std::lock_guard<std::recursive_mutex> lock(g_hwnd_mutex);
  auto it = g_hwnd_to_laufey_id.find(hwnd);
  return it != g_hwnd_to_laufey_id.end() ? it->second : 0;
}

// Per-window state
struct WinWindowState {
  uint32_t window_id;
  HWND hwnd;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  bool webview_ready = false;
  std::wstring pending_url;
  std::wstring pending_title;
  // LAUFEY_BACKDROP_* behind the page (API 38); not NONE means the client
  // area is left unpainted and the web view background is transparent.
  int backdrop = LAUFEY_BACKDROP_NONE;
  // DevTools (API 40). WebView2 opens them in a top-level window of its
  // browser process and has no API to close them or ask whether they are
  // open, so the window open_devtools brought up is tracked.
  UINT32 browser_pid = 0;
  HWND devtools_hwnd = nullptr;
  // open_devtools calls whose DevTools window hasn't been found yet (it
  // appears asynchronously), and whether a close_devtools came meanwhile:
  // that window is closed as soon as it is found, and the DevTools read as
  // closed until then. UI thread / the finder threads, under windows_mutex_.
  int devtools_opening = 0;
  bool devtools_close_pending = false;
};

// Custom window message for UI tasks
#define WM_UI_TASK (WM_USER + 1)

struct UiTaskData {
  void (*task)(void*);
  void* data;
};

// ============================================================================
// Window geometry units
// ============================================================================
//
// Sizes, positions and size constraints cross the C ABI in DIP (CSS pixels at
// the page's zoom 1), as on the other backends, and a window's size is its
// client (page) area; get_outer_size is the whole frame. HWNDs work in
// physical pixels (the process is per-monitor DPI aware), so a window's
// geometry converts with its own DPI and a screen's with its monitor's.

namespace {

double WinWindowScale(HWND hwnd) {
  UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
  return dpi ? dpi / 96.0 : 1.0;
}

double WinMonitorScale(HMONITOR monitor) {
  UINT dpi_x = 96, dpi_y = 96;
  if (!monitor ||
      FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) ||
      dpi_x == 0)
    return 1.0;
  return dpi_x / 96.0;
}

int WinToDip(LONG px, double scale) {
  return static_cast<int>(std::lround(px / scale));
}

LONG WinToPx(int dip, double scale) {
  return static_cast<LONG>(std::lround(dip * scale));
}

// The frame's physical size around a client area of `width` x `height`
// physical pixels: the window's current frame (title bar, borders, a menu
// bar however many rows it wraps to), or the style's frame while there is
// no client area to measure (minimized).
SIZE WinFrameForClient(HWND hwnd, LONG width, LONG height) {
  RECT window_rect, client;
  if (!IsIconic(hwnd) && GetWindowRect(hwnd, &window_rect) &&
      GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0) {
    return {width + (window_rect.right - window_rect.left) - client.right,
            height + (window_rect.bottom - window_rect.top) - client.bottom};
  }
  RECT r = {0, 0, width, height};
  AdjustWindowRectExForDpi(
      &r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
      GetMenu(hwnd) != nullptr,
      static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
      GetDpiForWindow(hwnd));
  return {r.right - r.left, r.bottom - r.top};
}

// Resizes `hwnd` so its client area is `width` x `height` DIP.
void WinSetClientSize(HWND hwnd, int width, int height, UINT extra_flags) {
  double scale = WinWindowScale(hwnd);
  LONG cw = WinToPx(width, scale), ch = WinToPx(height, scale);
  SIZE frame = WinFrameForClient(hwnd, cw, ch);
  SetWindowPos(hwnd, nullptr, 0, 0, frame.cx, frame.cy,
               SWP_NOMOVE | SWP_NOZORDER | extra_flags);
  // A menu bar can wrap to another row at the new width; correct once.
  RECT client;
  if (!IsIconic(hwnd) && GetClientRect(hwnd, &client) &&
      (client.right != cw || client.bottom != ch)) {
    frame = WinFrameForClient(hwnd, cw, ch);
    SetWindowPos(hwnd, nullptr, 0, 0, frame.cx, frame.cy,
                 SWP_NOMOVE | SWP_NOZORDER | extra_flags);
  }
}

// The frame (physical) around the client area while the window was last in
// the normal state, for normal bounds of a maximized / fullscreen window.
// Noted and forgotten on the UI thread, read from the runtime's thread
// (get_window_normal_bounds): every access holds WinNormalFramesMutex().
std::map<HWND, SIZE>& WinNormalFrames() {
  static std::map<HWND, SIZE> frames;
  return frames;
}

std::mutex& WinNormalFramesMutex() {
  static std::mutex mutex;
  return mutex;
}

void WinNoteNormalFrame(HWND hwnd) {
  RECT window_rect, client;
  if (GetWindowRect(hwnd, &window_rect) && GetClientRect(hwnd, &client) &&
      client.right > 0 && client.bottom > 0) {
    std::lock_guard<std::mutex> lock(WinNormalFramesMutex());
    WinNormalFrames()[hwnd] = {
        (window_rect.right - window_rect.left) - client.right,
        (window_rect.bottom - window_rect.top) - client.bottom};
  }
}

void WinForgetNormalFrame(HWND hwnd) {
  std::lock_guard<std::mutex> lock(WinNormalFramesMutex());
  WinNormalFrames().erase(hwnd);
}

// The noted normal frame of `hwnd`, if any.
bool WinNormalFrame(HWND hwnd, SIZE* frame) {
  std::lock_guard<std::mutex> lock(WinNormalFramesMutex());
  auto it = WinNormalFrames().find(hwnd);
  if (it == WinNormalFrames().end())
    return false;
  *frame = it->second;
  return true;
}

// A screen rectangle in its monitor's DIP.
void WinScreenRectToDip(int* x, int* y, int* width, int* height) {
  RECT r = {*x, *y, *x + *width, *y + *height};
  double scale = WinMonitorScale(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST));
  *x = WinToDip(r.left, scale);
  *y = WinToDip(r.top, scale);
  *width = WinToDip(r.right - r.left, scale);
  *height = WinToDip(r.bottom - r.top, scale);
}

}  // namespace

// ============================================================================
// Custom URL scheme handling (in-process transport)
// ============================================================================
//
// Serves "app" and every scheme the embedder registered through
// register_scheme_handler. WebView2 learns custom schemes from
// CoreWebView2CustomSchemeRegistration entries on the *environment options*,
// so the set is fixed when the first window's environment is created (see
// SchemesForEnvironment); each is TreatAsSecure (secure context,
// `<scheme>://<host>` origin, per-origin storage) with an authority component.

namespace {

std::wstring SchemeUtf8ToWide(const std::string& s) {
  if (s.empty())
    return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                              nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0],
                      n);
  return w;
}

// The exchange itself (buffered, or streamed through the page shim) lives in
// wv2_scheme_stream.cc.

// WebView2 reads a few environment variables of its own when an environment
// is created: extra Chromium switches, another browser binary, another
// profile, a script debugger. A packaged app with DevTools off clears them
// from its process environment first, once, so whoever starts it can't reopen
// what it closed (laufey_common::WebView2EnvironmentOverridesToClear).
void ClearWebView2EnvironmentOverrides() {
  static const bool done = [] {
    const bool packaged =
        laufey_common::IsPackagedLaunch(!LaufeyFindColocatedRuntime().empty());
    for (const std::string& name :
         laufey_common::WebView2EnvironmentOverridesToClear(
             packaged, laufey_common::LaunchInspectable())) {
      std::wstring wname = SchemeUtf8ToWide(name);
      if (GetEnvironmentVariableW(wname.c_str(), nullptr, 0) == 0)
        continue;  // not set
      std::cerr << "laufey: ignoring " << name
                << " (a packaged app with DevTools off)" << std::endl;
      SetEnvironmentVariableW(wname.c_str(), nullptr);
    }
    return true;
  }();
  (void)done;
}

}  // namespace

// ============================================================================
// WebView2 Backend
// ============================================================================

class WebView2Backend;
static WebView2Backend* g_win_backend = nullptr;

class WebView2Backend : public LaufeyBackend {
 public:
  WebView2Backend();
  ~WebView2Backend() override;

  void CreateWindow(uint32_t window_id, int width, int height) override;
  void CreateWindowEx(uint32_t window_id, int width, int height,
                      uint32_t flags) override;
  void CloseWindow(uint32_t window_id) override;

  void Navigate(uint32_t window_id, const std::string& url) override;
  // Record a scheme the embedder registered so the WebView2 environment
  // registers it as a secure custom scheme, next to "app". Must be called
  // before the first window: WebView2 fixes the set per environment.
  void RegisterSchemeHandler(const std::string& scheme) override;
  void OpenExternalURL(const std::string& url) override;
  void SetTitle(uint32_t window_id, const std::string& title) override;
  void ExecuteJs(uint32_t window_id, const std::string& script,
                 laufey_js_result_fn callback, void* callback_data) override;
  void Quit() override;
  void SetWindowSize(uint32_t window_id, int width, int height) override;
  void GetWindowSize(uint32_t window_id, int* width, int* height) override;
  void GetWindowOuterSize(uint32_t window_id, int* width, int* height) override;
  double GetWindowScaleFactor(uint32_t window_id) override;
  void SetWindowPosition(uint32_t window_id, int x, int y) override;
  void GetWindowPosition(uint32_t window_id, int* x, int* y) override;
  void GetWindowInnerPosition(uint32_t window_id, int* x, int* y) override;
  void SetResizable(uint32_t window_id, bool resizable) override;
  bool IsResizable(uint32_t window_id) override;
  void SetAlwaysOnTop(uint32_t window_id, bool always_on_top) override;
  bool IsAlwaysOnTop(uint32_t window_id) override;
  void SetWindowOpacity(uint32_t window_id, double opacity) override;
  double GetWindowOpacity(uint32_t window_id) override;
  void SetClickPassthrough(uint32_t window_id, bool enabled) override;
  bool IsClickPassthrough(uint32_t window_id) override;
  bool IsVisible(uint32_t window_id) override;
  void Show(uint32_t window_id) override;
  void Hide(uint32_t window_id) override;
  void Focus(uint32_t window_id) override;

  // Window state, constraints, screens and backdrop (API >= 38).
  uint32_t WindowCapabilities() override;
  void SetWindowState(uint32_t window_id, int action) override;
  uint32_t GetWindowState(uint32_t window_id) override;
  void SetWindowStateHandler(laufey_window_state_fn handler,
                             void* user_data) override {
    laufey_common::SetWindowStateHandler(handler, user_data);
  }
  void SetWindowSizeConstraints(uint32_t window_id, int min_width,
                                int min_height, int max_width,
                                int max_height) override;
  void GetWindowSizeConstraints(uint32_t window_id, int* min_width,
                                int* min_height, int* max_width,
                                int* max_height) override;
  size_t GetScreens(laufey_screen_t* out, size_t capacity) override {
    // In each monitor's DIP (see "Window geometry units").
    std::vector<laufey_screen_t> screens = laufey_common::WinGetScreens();
    for (auto& s : screens) {
      double k = s.scale_factor > 0 ? s.scale_factor : 1.0;
      s.x = WinToDip(s.x, k);
      s.y = WinToDip(s.y, k);
      s.width = WinToDip(s.width, k);
      s.height = WinToDip(s.height, k);
      s.work_x = WinToDip(s.work_x, k);
      s.work_y = WinToDip(s.work_y, k);
      s.work_width = WinToDip(s.work_width, k);
      s.work_height = WinToDip(s.work_height, k);
    }
    return laufey_common::CopyScreens(screens, out, capacity);
  }
  int64_t GetWindowScreen(uint32_t window_id) override;
  void SetDisplayChangedHandler(laufey_display_changed_fn handler,
                                void* user_data) override;
  bool SetWindowBackdrop(uint32_t window_id, int backdrop,
                         int material) override;
  bool GetWindowNormalBounds(uint32_t window_id, int* x, int* y, int* width,
                             int* height) override;
  void SetQuitOnLastWindowClosed(bool quit) override {
    laufey_common::SetQuitOnLastWindowClosed(quit);
  }
  bool PostUiTask(void (*task)(void*), void* data) override;
  void SetSecondInstanceHandler(laufey_second_instance_fn handler,
                                void* user_data) override {
    laufey_common::SetSecondInstanceHandler(handler, user_data);
  }

  void InvokeJsCallback(uint32_t window_id, uint64_t callback_id,
                        laufey::ValuePtr args) override;
  void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) override;
  void RespondToJsCall(uint32_t window_id, uint64_t call_id,
                       laufey::ValuePtr result,
                       laufey::ValuePtr error) override;

  void Run() override;

  void SetApplicationMenu(uint32_t window_id, laufey_value_t* menu_template,
                          const laufey_backend_api_t* api,
                          laufey_menu_click_fn on_click,
                          void* on_click_data) override;

  void ShowContextMenu(uint32_t window_id, int x, int y,
                       laufey_value_t* menu_template,
                       const laufey_backend_api_t* api,
                       laufey_menu_click_fn on_click,
                       void* on_click_data) override;

  void OpenDevTools(uint32_t window_id) override;

  void PrintToPdf(uint32_t window_id, laufey_pdf_result_fn callback,
                  void* callback_data) override;

  uint32_t PasskeyCapabilities() override {
    return laufey_common::PasskeyCapabilitiesWin();
  }
  void PasskeyRequest(uint32_t window_id, uint32_t kind,
                      const char* options_json,
                      laufey_passkey_result_fn callback,
                      void* user_data) override;

  int ShowDialog(uint32_t window_id, int dialog_type, const std::string& title,
                 const std::string& message, const std::string& default_value,
                 char** out_input_value) override;

  char* ReadClipboardText() override {
    return laufey_common::ClipboardReadTextWin();
  }
  void WriteClipboardText(const std::string& text) override {
    laufey_common::ClipboardWriteTextWin(text);
  }

  // Drag and drop, file dialogs, rich clipboard (API >= 39).
  void SetFileDropHandler(laufey_file_drop_fn handler,
                          void* user_data) override {
    laufey_common::SetFileDropHandler(handler, user_data);
  }
  bool TestTriggerFileDrop(uint32_t window_id, int phase, double x, double y,
                           const char* const* paths, size_t count) override {
    // The page's drop messages are dispatched on the UI thread; so is this.
    bool delivered = false;
    RunOnUiThreadSync([&] {
      delivered = laufey_common::TestTriggerFileDrop(window_id, phase, x, y,
                                                     paths, count);
    });
    return delivered;
  }
  void StartFileDrag(uint32_t window_id, const char* const* paths, size_t count,
                     const uint8_t* icon_png, size_t icon_len,
                     laufey_drag_result_fn callback, void* user_data) override;
  uint32_t ShowFileDialog(uint32_t window_id,
                          const laufey_file_dialog_options_t* options,
                          laufey_file_dialog_result_fn callback,
                          void* user_data) override;
  bool CancelFileDialog(uint32_t dialog_id) override {
    return laufey_common::CancelFileDialogWin(dialog_id);
  }
  bool TestFileDialogRespond(int action, const char* path) override {
    return laufey_common::TestFileDialogRespondWin(action, path);
  }
  uint32_t ClipboardCapabilities() override {
    return laufey_common::ClipboardCapabilitiesWin();
  }
  char* ReadClipboardHtml() override {
    return laufey_common::ClipboardReadHtmlWin();
  }
  bool WriteClipboardHtml(const std::string& html,
                          const char* text_or_null) override {
    return laufey_common::ClipboardWriteHtmlWin(html, text_or_null);
  }
  uint8_t* ReadClipboardImage(size_t* len_out) override {
    return laufey_common::ClipboardReadImageWin(len_out);
  }
  bool WriteClipboardImage(const uint8_t* png, size_t len) override {
    return laufey_common::ClipboardWriteImageWin(png, len);
  }
  char* ReadClipboardFormats() override {
    return laufey_common::ClipboardReadFormatsWin();
  }
  void SetClipboardChangeHandler(laufey_clipboard_change_fn handler,
                                 void* user_data) override {
    laufey_common::SetClipboardChangeHandler(handler, user_data);
  }

  // Global shortcuts, launch at login, DevTools (API >= 40). RegisterHotKey
  // runs on the UI thread (its hidden window lives there); the platform is
  // installed on first use.
  void EnsureShortcuts() {
    std::call_once(shortcuts_once_, [this] {
      laufey_common::InstallShortcutPlatform(
          laufey_common::CreateShortcutPlatformWin(
              [this](std::function<void()> task) {
                RunOnUiThread(std::move(task));
              }));
    });
  }
  uint32_t SystemCapabilities() override {
    EnsureShortcuts();
    uint32_t caps =
        laufey_common::ShortcutCapabilities() | LAUFEY_SYSTEM_CAP_DEVTOOLS;
    if (laufey_common::GetLaunchAtLogin() != LAUFEY_LOGIN_ITEM_NOT_SUPPORTED)
      caps |= LAUFEY_SYSTEM_CAP_LAUNCH_AT_LOGIN;
    return caps;
  }
  void SetShortcutHandler(laufey_shortcut_fn handler,
                          void* user_data) override {
    laufey_common::SetShortcutHandler(handler, user_data);
  }
  void RegisterShortcut(const char* accelerator,
                        laufey_shortcut_result_fn callback,
                        void* user_data) override {
    EnsureShortcuts();
    laufey_common::RegisterShortcut(accelerator, callback, user_data);
  }
  bool UnregisterShortcut(const char* accelerator) override {
    return laufey_common::UnregisterShortcut(accelerator);
  }
  void UnregisterAllShortcuts() override {
    laufey_common::UnregisterAllShortcuts();
  }
  char* ListShortcuts() override {
    return laufey_common::ListShortcuts();
  }
  char* PlatformFeatures() override {
    return laufey_common::PlatformFeaturesJsonForAbi();
  }
  char* CanonicalizeAccelerator(const char* accelerator) override {
    return laufey_common::CanonicalizeAccelerator(accelerator);
  }
  bool TestTriggerShortcut(const char* accelerator) override {
    return laufey_common::TestTriggerShortcut(accelerator);
  }
  int GetLaunchAtLogin() override {
    return laufey_common::GetLaunchAtLogin();
  }
  int SetLaunchAtLogin(bool enabled, std::string* error) override {
    return laufey_common::SetLaunchAtLogin(enabled, error);
  }
  void CloseDevTools(uint32_t window_id) override;
  bool IsDevToolsOpen(uint32_t window_id) override;
  bool IsDevToolsEnabled(uint32_t window_id) override;

  void BounceDock(int type) override;
  void SetDockBadge(const char* badge_or_null) override;

  uint32_t CreateTrayIcon() override;
  void DestroyTrayIcon(uint32_t tray_id) override;
  void SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                   size_t len) override;
  void SetTrayTooltip(uint32_t tray_id, const char* tooltip_or_null) override;
  void SetTrayMenu(uint32_t tray_id, laufey_value_t* menu_template,
                   const laufey_backend_api_t* api,
                   laufey_menu_click_fn on_click, void* on_click_data) override;
  void SetTrayClickHandler(uint32_t tray_id, laufey_tray_click_fn handler,
                           void* user_data) override;
  void SetTrayDoubleClickHandler(uint32_t tray_id, laufey_tray_click_fn handler,
                                 void* user_data) override;
  void SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                       size_t len) override;
  bool GetTrayIconBounds(uint32_t tray_id, int* x, int* y, int* width,
                         int* height) override;

  uint32_t ShowNotification(laufey_value_t* options,
                            const laufey_backend_api_t* api,
                            laufey_notification_event_fn on_event,
                            void* user_data) override;
  void CloseNotification(uint32_t notification_id) override;

  // Toasts: the user's notification setting for the app (no prompt).
  void QueryPermission(int kind, laufey_permission_callback_fn cb,
                       void* user_data) override {
    laufey_common::QueryNotificationPermission(kind, cb, user_data);
  }
  void RequestPermission(int kind, laufey_permission_callback_fn cb,
                         void* user_data) override {
    laufey_common::RequestNotificationPermission(kind, cb, user_data);
  }

  // Notifications and menus (API >= 41): backend-common.
  uint32_t NotificationCapabilities() override {
    return laufey_common::NotificationCapabilities();
  }
  void SetNotificationResponseHandler(laufey_notification_response_fn handler,
                                      void* user_data) override {
    laufey_common::SetNotificationResponseHandler(handler, user_data);
  }
  void ListScheduledNotifications(laufey_notification_list_fn cb,
                                  void* user_data) override {
    laufey_common::ListScheduledNotifications(cb, user_data);
  }
  void CancelNotification(const char* tag) override {
    laufey_common::CancelNotification(tag);
  }
  bool TestNotificationRespond(const char* tag,
                               const char* action_id) override {
    return laufey_common::TestNotificationRespond(tag, action_id);
  }
  uint32_t MenuCapabilities() override {
    return LAUFEY_MENU_CAP_APP_MENU | LAUFEY_MENU_CAP_ACCELERATORS |
           LAUFEY_MENU_CAP_CONTEXT_MENU | LAUFEY_MENU_CAP_CONTEXT_CLOSED |
           LAUFEY_MENU_CAP_ICONS;
  }
  void ShowContextMenuEx(uint32_t window_id, int x, int y,
                         laufey_value_t* menu_template,
                         const laufey_backend_api_t* api,
                         laufey_menu_click_fn on_click, void* on_click_data,
                         laufey_menu_closed_fn on_closed,
                         void* on_closed_data) override;
  bool TestDismissContextMenu() override {
    return laufey_common::DismissOpenContextMenu();
  }
  bool TestTriggerMenuAccelerator(uint32_t window_id,
                                  const char* accelerator) override;

  void HandleJsMessage(uint32_t window_id, const std::wstring& json,
                       const std::string& origin);
  // A message from the injected file-drop observer (see
  // BuildFileDropScript): true if `message` was one (handled or refused),
  // false if it is for the JS bridge.
  bool HandleFileDropMessage(uint32_t window_id, const wchar_t* message,
                             ICoreWebView2WebMessageReceivedEventArgs* args);

  // Drops the state of a window that is being destroyed (WM_DESTROY): the
  // user closed it, or CloseWindow() destroyed it (which erases the entry
  // itself as well). UI thread.
  void ForgetDestroyedWindow(uint32_t window_id);

 private:
  WinWindowState* GetWindow(uint32_t window_id);
  void InitializeWebViewForWindow(uint32_t window_id, HWND hwnd);
  // The custom schemes to register on a WebView2 environment. Every
  // environment this process creates shares one user data folder, and
  // WebView2 requires identical custom-scheme registrations across them
  // (creation fails otherwise), so the registered-scheme set is frozen on the
  // first call and reused for every later window. A RegisterSchemeHandler
  // after that point is logged and has no effect — hence the contract to
  // register schemes before the first window.
  std::vector<std::string> SchemesForEnvironment();
  // Creates the WebView2 environment for a window, registering `schemes`
  // ("app" plus the embedder's) as secure custom schemes. If that makes
  // environment creation fail it retries once with no custom schemes so the
  // window still opens (only the in-process scheme transport is lost).
  void CreateEnvironmentForWindow(uint32_t window_id, HWND hwnd,
                                  std::vector<std::string> schemes);
  // Wires up the controller, init script, message + scheme handlers once the
  // environment is ready. `schemes` are the custom schemes actually
  // registered on the environment (empty after the fallback retry).
  void OnEnvironmentReady(uint32_t window_id, HWND hwnd,
                          ICoreWebView2Environment* env,
                          std::vector<std::string> schemes);
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam);

  // Marshal `task` onto the UI thread — the one that ran CoInitializeEx and
  // pumps the message loop in Run(). WebView2 is single-threaded-apartment:
  // every CreateCoreWebView2* call and every webview/controller method MUST
  // run there, and its async completion callbacks are only delivered while
  // that thread pumps messages. The runtime (and thus the C-ABI calls that
  // reach this backend) runs on a separate thread, so without this the
  // WebView2 environment fails to initialize (CO_E_NOTINITIALIZED) or its
  // controller callback never fires and the window stays blank. Runs `task`
  // inline when already on the UI thread.
  void RunOnUiThread(std::function<void()> task);

  // Like RunOnUiThread, but blocks until the task has run. For calls whose
  // arguments only outlive the call itself (e.g. a caller-owned
  // laufey_value_t*). Callers must not hold locks that UI-thread message
  // handlers also take. False when the task did not run because the loop
  // had ended (the caller answers with its defaults).
  bool RunOnUiThreadSync(std::function<void()> task);

  std::map<uint32_t, WinWindowState> windows_;
  std::once_flag shortcuts_once_;
  // The window's DevTools window (tracked or, with one window per browser
  // process, found by title), or null. Any thread.
  HWND FindDevToolsWindow(uint32_t window_id);
  std::recursive_mutex windows_mutex_;
  bool class_registered_ = false;
  // See SchemesForEnvironment.
  std::mutex schemes_mutex_;
  bool schemes_frozen_ = false;
  std::vector<std::string> frozen_schemes_;
  // Thread that constructs the backend (== WinMain / the message-loop thread).
  DWORD ui_thread_id_ = 0;
  // Message-only window owned by the UI thread used to receive marshaled
  // tasks even before the first real window exists.
  HWND dispatcher_hwnd_ = nullptr;
  // Random per process; the file-drop observer script carries it in its
  // closure and the host drops observer-shaped messages without it.
  std::string file_drop_token_;
};

LRESULT CALLBACK WebView2Backend::WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                             LPARAM lParam) {
  uint32_t wid = LaufeyIdForHwnd(hwnd);

  switch (msg) {
    case WM_SIZE: {
      if (g_win_backend && wid > 0) {
        std::lock_guard<std::recursive_mutex> lock(
            g_win_backend->windows_mutex_);
        auto* state = g_win_backend->GetWindow(wid);
        if (state && state->controller) {
          RECT bounds;
          GetClientRect(hwnd, &bounds);
          state->controller->put_Bounds(bounds);
        }
      }
      if (wid > 0) {
        if (wParam == SIZE_RESTORED && !laufey_common::WinIsFullscreen(wid))
          WinNoteNormalFrame(hwnd);
        RECT rect;
        GetClientRect(hwnd, &rect);
        double scale = WinWindowScale(hwnd);
        RuntimeLoader::GetInstance()->DispatchResizeEvent(
            wid, WinToDip(rect.right - rect.left, scale),
            WinToDip(rect.bottom - rect.top, scale));
        // SIZE_MAXIMIZED / SIZE_MINIMIZED / SIZE_RESTORED: one source of the
        // window-state events (API 38); duplicates are dropped there.
        laufey_common::ReportWindowState(
            wid, laufey_common::WinGetWindowState(hwnd, wid));
      }
      return 0;
    }
    case WM_GETMINMAXINFO:
      // Size constraints (API 38) are client sizes in DIP, like
      // set_window_size; Windows tracks the frame in physical pixels.
      if (wid > 0) {
        SIZE frame = WinFrameForClient(hwnd, 0, 0);
        if (laufey_common::WinApplyMinMaxInfo(
                reinterpret_cast<void*>(lParam),
                laufey_common::GetSizeConstraints(wid), WinWindowScale(hwnd),
                frame.cx, frame.cy)) {
          return 0;
        }
      }
      break;
    case WM_DPICHANGED: {
      // Moved to a monitor with another scale: take the size Windows
      // suggests, which keeps the window's DIP size.
      const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
      if (suggested) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
      return 0;
    }
    case WM_ERASEBKGND:
      // With a backdrop the client area must stay unpainted (black is
      // transparent over an extended DWM frame) for it to show through.
      if (g_win_backend && wid > 0) {
        std::lock_guard<std::recursive_mutex> lock(
            g_win_backend->windows_mutex_);
        auto* state = g_win_backend->GetWindow(wid);
        if (state && state->backdrop != LAUFEY_BACKDROP_NONE) {
          RECT rect;
          GetClientRect(hwnd, &rect);
          FillRect(reinterpret_cast<HDC>(wParam), &rect,
                   static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
          return 1;
        }
      }
      break;
    case WM_MOVE:
      if (wid > 0) {
        double scale = WinWindowScale(hwnd);
        RuntimeLoader::GetInstance()->DispatchMoveEvent(
            wid, WinToDip((short)LOWORD(lParam), scale),
            WinToDip((short)HIWORD(lParam), scale));
      }
      return 0;
    case WM_SETFOCUS:
      if (wid > 0)
        RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 1);
      return 0;
    case WM_KILLFOCUS:
      if (wid > 0)
        RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 0);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
      if (wid == 0)
        break;
      int state = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
                   msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)
                      ? LAUFEY_MOUSE_PRESSED
                      : LAUFEY_MOUSE_RELEASED;
      int button;
      switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
          button = LAUFEY_MOUSE_BUTTON_LEFT;
          break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
          button = LAUFEY_MOUSE_BUTTON_RIGHT;
          break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
          button = LAUFEY_MOUSE_BUTTON_MIDDLE;
          break;
        default:
          button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1)
                       ? LAUFEY_MOUSE_BUTTON_BACK
                       : LAUFEY_MOUSE_BUTTON_FORWARD;
          break;
      }
      double scale = WinWindowScale(hwnd);
      double x = GET_X_LPARAM(lParam) / scale;
      double y = GET_Y_LPARAM(lParam) / scale;
      uint32_t modifiers = keyboard::GetLaufeyModifiers();
      RuntimeLoader::GetInstance()->DispatchMouseClickEvent(wid, state, button,
                                                            x, y, modifiers, 1);
      break;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if (wid == 0)
        break;
      std::string key = keyboard::VirtualKeyToKey(wParam, lParam);
      std::string code = keyboard::VirtualKeyToCode(wParam, lParam);
      uint32_t modifiers = keyboard::GetLaufeyModifiers();
      bool repeat = (lParam & (1 << 30)) != 0;
      RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
          wid, LAUFEY_KEY_PRESSED, key.c_str(), code.c_str(), modifiers,
          repeat);
      break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      if (wid == 0)
        break;
      std::string key = keyboard::VirtualKeyToKey(wParam, lParam);
      std::string code = keyboard::VirtualKeyToCode(wParam, lParam);
      uint32_t modifiers = keyboard::GetLaufeyModifiers();
      RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
          wid, LAUFEY_KEY_RELEASED, key.c_str(), code.c_str(), modifiers,
          false);
      break;
    }
    case WM_CLOSE: {
      bool proceed = true;
      if (wid > 0) {
        proceed =
            RuntimeLoader::GetInstance()->DispatchCloseRequestedEvent(wid);
      }
      if (!proceed) {
        // A close-requested handler deferred the close: leave the window open.
        // Not calling DestroyWindow is the standard Win32 idiom for this.
        return 0;
      }
      // Unregistration and the last-window quit check live in WM_DESTROY,
      // which every destroy path hits (this one, and CloseWindow's direct
      // DestroyWindow when a deferred close is later resolved).
      DestroyWindow(hwnd);
      return 0;
    }
    case WM_COMMAND:
      if (win32_menu::HandleMenuCommand(hwnd, wParam))
        return 0;
      break;
    case WM_DESTROY: {
      // A passkey dialog owned by this window ends with it (`cancelled`).
      // Before the lock below: the result callback may re-enter the backend.
      laufey_common::PasskeyWindowClosing(hwnd);
      // Single exit point for window teardown: fires for WM_CLOSE-initiated
      // closes and for CloseWindow()'s direct DestroyWindow alike, so a
      // deferred close resolved via close_window still quits the message
      // loop when the last window goes away.
      if (wid > 0) {
        // A window the user closed (WM_CLOSE -> DestroyWindow) still has its
        // entry: drop it, or every later call naming the id would reach the
        // destroyed HWND and its web view. Lock order windows_mutex_ ->
        // g_hwnd_mutex, as in ~WebView2Backend.
        if (g_win_backend)
          g_win_backend->ForgetDestroyedWindow(wid);
        laufey_common::ForgetWindow(wid);
        laufey_wv2::CancelStreamsForWindow(wid);
      }
      WinForgetNormalFrame(hwnd);
      win32_menu::ForgetWindow(hwnd);
      std::lock_guard<std::recursive_mutex> lock(g_hwnd_mutex);
      // A tray app can keep running with no window
      // (set_quit_on_last_window_closed(false)); quit() ends it anyway.
      if (g_hwnd_to_laufey_id.erase(hwnd) > 0 && g_hwnd_to_laufey_id.empty() &&
          laufey_common::ShouldEndLoopAfterLastWindow()) {
        PostQuitMessage(0);
      }
      return 0;
    }
    case WM_UI_TASK: {
      UiTaskData* taskData = reinterpret_cast<UiTaskData*>(lParam);
      if (taskData) {
        taskData->task(taskData->data);
        delete taskData;
      }
      return 0;
    }
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

WebView2Backend::WebView2Backend() {
  g_win_backend = this;

  // The backend is constructed on the UI thread (WinMain, before the runtime
  // thread is spawned). Remember it: all WebView2 work is marshaled here.
  ui_thread_id_ = GetCurrentThreadId();

  // Register window class
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = L"LaufeyWebView2";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClassExW(&wc);
  class_registered_ = true;

  // A message-only window lets RunOnUiThread deliver work to this thread even
  // before the first real window is created (the bootstrapping create_window
  // call itself is marshaled through here). It shares the class above, so its
  // WM_UI_TASK messages are handled by WindowProc.
  dispatcher_hwnd_ =
      CreateWindowExW(0, L"LaufeyWebView2", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                      nullptr, GetModuleHandle(nullptr), nullptr);

  laufey_wv2::StreamHooks stream_hooks;
  stream_hooks.run_on_ui = [this](std::function<void()> task) {
    RunOnUiThread(std::move(task));
  };
  stream_hooks.post_json = [this](uint32_t window_id,
                                  const std::wstring& json) {
    std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    return state && state->webview &&
           SUCCEEDED(state->webview->PostWebMessageAsJson(json.c_str()));
  };
  laufey_wv2::InitSchemeStreams(std::move(stream_hooks));

  // backend-common's I/O thread (file dialogs, drag-out, clipboard change
  // events; its own STA thread, see laufey_io.h).
  laufey_common::WinIoInit();

  // 256 random bits from two v4 GUIDs (CoCreateGuid draws them from the
  // system RNG).
  for (int i = 0; i < 2; i++) {
    GUID g = {};
    CoCreateGuid(&g);
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&g);
    static const char kHex[] = "0123456789abcdef";
    for (size_t k = 0; k < sizeof(g); k++) {
      file_drop_token_ += kHex[b[k] >> 4];
      file_drop_token_ += kHex[b[k] & 15];
    }
  }
}

void WebView2Backend::RunOnUiThread(std::function<void()> task) {
  if (GetCurrentThreadId() == ui_thread_id_) {
    task();
    return;
  }
  // Heap-box the closure and a trampoline that invokes then frees it; the
  // existing WM_UI_TASK handler in WindowProc runs `task(data)` and deletes
  // the UiTaskData.
  auto* boxed = new std::function<void()>(std::move(task));
  auto* td = new UiTaskData{[](void* p) {
                              auto* fn = static_cast<std::function<void()>*>(p);
                              (*fn)();
                              delete fn;
                            },
                            boxed};
  if (!PostMessageW(dispatcher_hwnd_, WM_UI_TASK, 0,
                    reinterpret_cast<LPARAM>(td))) {
    // Posting failed (no dispatcher / queue full): don't leak, run inline as a
    // last resort even though we're off the UI thread.
    td->task(td->data);
    delete td;
  }
}

bool WebView2Backend::RunOnUiThreadSync(std::function<void()> task) {
  if (GetCurrentThreadId() == ui_thread_id_) {
    task();
    return true;
  }
  // Through the UI task dispatcher (the same WM_UI_TASK queue), so the wait
  // ends with the loop instead of outliving it.
  return laufey_common::RunOnUiThreadAndWait(task);
}

WebView2Backend::~WebView2Backend() {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (state.controller)
      state.controller->Close();
    {
      std::lock_guard<std::recursive_mutex> hlock(g_hwnd_mutex);
      g_hwnd_to_laufey_id.erase(state.hwnd);
    }
  }
  windows_.clear();
  g_win_backend = nullptr;
}

void WebView2Backend::ForgetDestroyedWindow(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return;
  // WM_DESTROY reaches the parent before its children are destroyed: the
  // controller can still close cleanly here.
  if (state->controller)
    state->controller->Close();
  windows_.erase(window_id);
}

WinWindowState* WebView2Backend::GetWindow(uint32_t window_id) {
  auto it = windows_.find(window_id);
  return it != windows_.end() ? &it->second : nullptr;
}

void WebView2Backend::CreateWindow(uint32_t window_id, int width, int height) {
  CreateWindowEx(window_id, width, height, 0);
}

void WebView2Backend::CreateWindowEx(uint32_t window_id, int width, int height,
                                     uint32_t flags) {
  // Window + WebView2 creation must happen on the UI thread. This is called
  // from the runtime thread, so marshal it (the window_id is already allocated
  // by the caller, so nothing here needs to return synchronously).
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, width, height, flags] {
      CreateWindowEx(window_id, width, height, flags);
    });
    return;
  }

  DWORD style = WS_OVERLAPPEDWINDOW;
  DWORD ex_style = 0;
  if (flags & LAUFEY_WINDOW_FLAG_FRAMELESS) {
    // Borderless popup: no caption / sizing frame.
    style = WS_POPUP;
  }
  if (flags & LAUFEY_WINDOW_FLAG_NO_ACTIVATE) {
    // Don't steal foreground/focus; keep out of the taskbar and Alt-Tab.
    ex_style |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
  }

  HWND hwnd = CreateWindowExW(
      ex_style, L"LaufeyWebView2", L"", style, CW_USEDEFAULT, CW_USEDEFAULT,
      width, height, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

  {
    std::lock_guard<std::recursive_mutex> lock(g_hwnd_mutex);
    g_hwnd_to_laufey_id[hwnd] = window_id;
  }

  WinWindowState state;
  state.window_id = window_id;
  state.hwnd = hwnd;

  {
    std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
    windows_[window_id] = state;
  }

  InitializeWebViewForWindow(window_id, hwnd);

  // A hidden window is created but not shown; the embedder reveals it later
  // (typically from a page-load handler) so the empty initial frame is never
  // seen. WebView2 still loads and fires NavigationCompleted while hidden.
  if (!(flags & LAUFEY_WINDOW_FLAG_HIDDEN)) {
    // Showing a non-activating panel must not take foreground from the user's
    // active window.
    ShowWindow(hwnd, (flags & LAUFEY_WINDOW_FLAG_NO_ACTIVATE)
                         ? SW_SHOWNOACTIVATE
                         : SW_SHOW);
    UpdateWindow(hwnd);
  }
}

void WebView2Backend::RegisterSchemeHandler(const std::string& scheme) {
  // Any thread (the runtime registers from its own thread).
  if (!laufey_common::IsValidSchemeName(scheme)) {
    std::cerr << "laufey: ignoring invalid URL scheme name \"" << scheme
              << "\" passed to register_scheme_handler" << std::endl;
    return;
  }
  bool added = laufey_common::SchemeRegistry::GetInstance()->Add(scheme);
  std::lock_guard<std::mutex> lock(schemes_mutex_);
  if (added && schemes_frozen_) {
    std::cerr << "laufey: scheme \"" << scheme
              << "\" was registered after the first window was created; "
                 "WebView2 fixes custom schemes when the environment is "
                 "created, so it will not be served (register schemes "
                 "before the first window)"
              << std::endl;
  }
}

std::vector<std::string> WebView2Backend::SchemesForEnvironment() {
  std::lock_guard<std::mutex> lock(schemes_mutex_);
  if (!schemes_frozen_) {
    frozen_schemes_ = laufey_common::SchemeRegistry::GetInstance()->Snapshot();
    schemes_frozen_ = true;
  }
  return frozen_schemes_;
}

void WebView2Backend::InitializeWebViewForWindow(uint32_t window_id,
                                                 HWND hwnd) {
  // Try with the in-process custom schemes ("app" plus any the embedder
  // registered) first. If registering them makes environment creation fail,
  // CreateEnvironmentForWindow retries without them so the window still opens
  // for ordinary http(s)/TCP navigations.
  CreateEnvironmentForWindow(window_id, hwnd, SchemesForEnvironment());
}

void WebView2Backend::CreateEnvironmentForWindow(
    uint32_t window_id, HWND hwnd, std::vector<std::string> schemes) {
  ComPtr<ICoreWebView2EnvironmentOptions> options;
  // Keep the registration objects alive until the options have consumed
  // them; SetCustomSchemeRegistrations takes an array of raw pointers.
  std::vector<ComPtr<CoreWebView2CustomSchemeRegistration>> registrations;
  if (!schemes.empty()) {
    // Register each scheme as a secure custom scheme with an authority
    // component so the in-process scheme handler can serve top-level
    // navigations to <scheme>://<host>/ like a normal https origin:
    // TreatAsSecure gives isSecureContext / crypto.subtle / per-origin
    // storage, HasAuthorityComponent makes `<scheme>://<host>` the origin.
    // AllowedOrigins is left empty: no other origin may fetch the custom
    // scheme (pages served over it fetching http(s) origins are governed by
    // those origins' CORS headers, as with any secure origin).
    auto opts = Make<CoreWebView2EnvironmentOptions>();
    ComPtr<ICoreWebView2EnvironmentOptions4> options4;
    if (SUCCEEDED(opts.As(&options4)) && options4) {
      std::vector<ICoreWebView2CustomSchemeRegistration*> raw;
      for (const std::string& scheme : schemes) {
        std::wstring name = SchemeUtf8ToWide(scheme);
        auto registration =
            Make<CoreWebView2CustomSchemeRegistration>(name.c_str());
        registration->put_TreatAsSecure(TRUE);
        registration->put_HasAuthorityComponent(TRUE);
        raw.push_back(registration.Get());
        registrations.push_back(registration);
      }
      options4->SetCustomSchemeRegistrations(static_cast<UINT32>(raw.size()),
                                             raw.data());
    }
    options = opts;
  }

  // Per-app user data folder (LAUFEY_DATA_DIR / LAUFEY_APP_ID). Without one,
  // pass nullptr to keep WebView2's default "<exe>.WebView2" next to the exe.
  // Every window's environment must use the same folder.
  static const std::wstring user_data_folder =
      laufey_common::Utf8ToWide(laufey_common::AppDataSubdir("WebView2"));

  ClearWebView2EnvironmentOverrides();
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data_folder.empty() ? nullptr : user_data_folder.c_str(),
      options.Get(),
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this, window_id, hwnd, schemes](
              HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
              if (!schemes.empty()) {
                // Registering custom schemes can make environment creation
                // fail outright — e.g. when the (shared, exe-derived) user
                // data folder was previously initialized with a different set
                // of custom schemes, WebView2 returns an error instead of
                // opening. Retry once without them so the window still
                // appears; only the in-process scheme transport is lost.
                std::cerr << "WebView2 environment creation failed with the "
                             "custom schemes (hr=0x"
                          << std::hex << result << std::dec
                          << "), retrying without them" << std::endl;
                CreateEnvironmentForWindow(window_id, hwnd, {});
                return S_OK;
              }
              std::cerr << "Failed to create WebView2 environment (hr=0x"
                        << std::hex << result << std::dec << ")" << std::endl;
              return result;
            }
            OnEnvironmentReady(window_id, hwnd, env, schemes);
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    std::cerr << "CreateCoreWebView2EnvironmentWithOptions failed to start "
                 "(hr=0x"
              << std::hex << hr << std::dec << ")" << std::endl;
  }
}

void WebView2Backend::OnEnvironmentReady(uint32_t window_id, HWND hwnd,
                                         ICoreWebView2Environment* env,
                                         std::vector<std::string> schemes) {
  env->CreateCoreWebView2Controller(
      hwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [this, window_id, hwnd, env, schemes](
              HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (FAILED(result) || !controller) {
              std::cerr << "Failed to create WebView2 controller" << std::endl;
              return result;
            }

            std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
            auto* state = GetWindow(window_id);
            if (!state) {
              // The window closed while the web view was being created.
              controller->Close();
              return S_OK;
            }

            state->controller = controller;
            controller->get_CoreWebView2(&state->webview);
            if (state->webview) {
              state->webview->get_BrowserProcessId(&state->browser_pid);
              // DevTools (API 40): F12, the context menu's Inspect and
              // OpenDevToolsWindow all follow AreDevToolsEnabled, which
              // follows LAUFEY_INSPECTABLE / "inspectable" (default on).
              ComPtr<ICoreWebView2Settings> settings;
              if (SUCCEEDED(state->webview->get_Settings(&settings)) &&
                  settings) {
                settings->put_AreDevToolsEnabled(
                    laufey_common::LaunchInspectable() ? TRUE : FALSE);
              }
            }

            RECT bounds;
            GetClientRect(hwnd, &bounds);
            controller->put_Bounds(bounds);
            controller->put_IsVisible(TRUE);

            // App menu accelerators while the page has the focus: its keys
            // go to the browser process, never through Run()'s message loop,
            // so the menu's accelerator table is matched here (API 41).
            controller->add_AcceleratorKeyPressed(
                Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                    [hwnd](ICoreWebView2Controller*,
                           ICoreWebView2AcceleratorKeyPressedEventArgs* args)
                        -> HRESULT {
                      COREWEBVIEW2_KEY_EVENT_KIND kind;
                      UINT key = 0;
                      if (FAILED(args->get_KeyEventKind(&kind)) ||
                          FAILED(args->get_VirtualKey(&key)))
                        return S_OK;
                      if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                          kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                        return S_OK;
                      if (win32_menu::HandleAcceleratorKey(hwnd, key))
                        args->put_Handled(TRUE);
                      return S_OK;
                    })
                    .Get(),
                nullptr);

            std::string initScript = BuildInitScript(
                RuntimeLoader::GetInstance()->GetJsNamespace(),
                "window.chrome.webview.postMessage(JSON.stringify({\n"
                "            callId: callId,\n"
                "            method: path.join('.'),\n"
                "            args: processedArgs\n"
                "          }));",
                "", RuntimeLoader::GetInstance()->BridgeGuardJs());
            std::wstring wInitScript(initScript.begin(), initScript.end());
            state->webview->AddScriptToExecuteOnDocumentCreated(
                wInitScript.c_str(), nullptr);

            // The file-drop observer (API 39): reports file drags over the
            // page to the host, the drop with its File objects, whose native
            // paths WebView2 reveals to the host only (ICoreWebView2File).
            // Its `send` posts with a per-process token kept in the closure,
            // through intrinsics bound before any page script runs.
            std::string dropScript =
                laufey_common::BuildWebView2FileDropScript(file_drop_token_);
            std::wstring wDropScript(dropScript.begin(), dropScript.end());
            state->webview->AddScriptToExecuteOnDocumentCreated(
                wDropScript.c_str(), nullptr);

            uint32_t wid = window_id;
            state->webview->add_WebMessageReceived(
                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                    [this, wid](ICoreWebView2* sender,
                                ICoreWebView2WebMessageReceivedEventArgs* args)
                        -> HRESULT {
                      // The injected bridge script runs in every frame
                      // (AddScriptToExecuteOnDocumentCreated cannot be scoped
                      // to the main frame), so validate the message source
                      // here: only accept messages whose document URI matches
                      // the top-level document. This stops cross-origin/sub
                      // frames from invoking bindings that run with the host
                      // process's permissions.
                      LPWSTR msgSource = nullptr;
                      LPWSTR topSource = nullptr;
                      args->get_Source(&msgSource);
                      sender->get_Source(&topSource);
                      bool from_main_frame = msgSource && topSource &&
                                             wcscmp(msgSource, topSource) == 0;
                      // API 44: the calling document's origin.
                      std::string origin = msgSource
                                               ? laufey_common::OriginOfUrl(
                                                     WideToUtf8(msgSource))
                                               : laufey_common::kOpaqueOrigin;
                      if (msgSource) {
                        CoTaskMemFree(msgSource);
                      }
                      if (topSource) {
                        CoTaskMemFree(topSource);
                      }
                      if (!from_main_frame) {
                        return S_OK;
                      }

                      LPWSTR messageRaw = nullptr;
                      args->TryGetWebMessageAsString(&messageRaw);
                      if (messageRaw) {
                        LPWSTR source = nullptr;
                        args->get_Source(&source);
                        if (!laufey_wv2::HandleStreamMessage(wid, messageRaw,
                                                             source) &&
                            !HandleFileDropMessage(wid, messageRaw, args))
                          HandleJsMessage(wid, messageRaw, origin);
                        if (source)
                          CoTaskMemFree(source);
                        CoTaskMemFree(messageRaw);
                      }
                      return S_OK;
                    })
                    .Get(),
                nullptr);

            // `target="_blank"` / `window.open()` request a new window, which
            // the Navigation API interceptor never sees. WebView2 would spawn a
            // popup webview; instead route http(s) destinations to the OS
            // browser when a user action started the request
            // (laufey_external_links.h), and mark the request handled so no
            // popup is created.
            state->webview->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [this](ICoreWebView2* sender,
                           ICoreWebView2NewWindowRequestedEventArgs* args)
                        -> HRESULT {
                      LPWSTR uriRaw = nullptr;
                      args->get_Uri(&uriRaw);
                      BOOL userInitiated = FALSE;
                      if (FAILED(args->get_IsUserInitiated(&userInitiated)))
                        userInitiated = FALSE;
                      if (uriRaw) {
                        std::string url = laufey_common::WideToUtf8(uriRaw);
                        CoTaskMemFree(uriRaw);
                        switch (
                            DecideLaufeyPopup(url, userInitiated != FALSE)) {
                          case LaufeyPopupDecision::kOpenInBrowser:
                            OpenExternalURL(url);
                            break;
                          case LaufeyPopupDecision::kBlockedNoGesture:
                            std::cerr << "laufey: not opening " << url
                                      << " in the browser: the page asked "
                                         "without a user gesture"
                                      << std::endl;
                            break;
                          case LaufeyPopupDecision::kIgnored:
                            break;
                        }
                      }
                      args->put_Handled(TRUE);
                      return S_OK;
                    })
                    .Get(),
                nullptr);

            // In-process custom schemes: intercept requests for each
            // registered scheme and bridge them to the runtime's memory
            // transport (one handler; the runtime dispatches on the URL).
            // Only wired up for schemes actually registered on the
            // environment — otherwise the filter would never fire and the
            // handler is dead weight.
            if (!schemes.empty()) {
              // Streams fetch / EventSource / XHR responses on these schemes
              // to the page; see wv2_scheme_stream.h.
              std::wstring shim = SchemeUtf8ToWide(
                  laufey_wv2::BuildSchemeStreamShimScript(schemes));
              state->webview->AddScriptToExecuteOnDocumentCreated(shim.c_str(),
                                                                  nullptr);
              ComPtr<ICoreWebView2Environment> envPtr = env;
              for (const std::string& scheme : schemes) {
                std::wstring filter = SchemeUtf8ToWide(scheme) + L"://*";
                state->webview->AddWebResourceRequestedFilter(
                    filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
              }
              EventRegistrationToken schemeToken;
              state->webview->add_WebResourceRequested(
                  Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                      [envPtr, wid](ICoreWebView2* sender,
                                    ICoreWebView2WebResourceRequestedEventArgs*
                                        args) -> HRESULT {
                        return laufey_wv2::HandleSchemeRequest(envPtr.Get(),
                                                               args, wid);
                      })
                      .Get(),
                  &schemeToken);
              // The streams of a document end with it: at the commit of the
              // main-frame navigation that replaces it, or with its renderer.
              state->webview->add_NavigationStarting(
                  Callback<ICoreWebView2NavigationStartingEventHandler>(
                      [wid](ICoreWebView2*,
                            ICoreWebView2NavigationStartingEventArgs* args)
                          -> HRESULT {
                        UINT64 nav = 0;
                        args->get_NavigationId(&nav);
                        laufey_wv2::OnNavigationStarting(wid, nav);
                        return S_OK;
                      })
                      .Get(),
                  nullptr);
              state->webview->add_ContentLoading(
                  Callback<ICoreWebView2ContentLoadingEventHandler>(
                      [wid](ICoreWebView2*,
                            ICoreWebView2ContentLoadingEventArgs* args)
                          -> HRESULT {
                        UINT64 nav = 0;
                        args->get_NavigationId(&nav);
                        laufey_wv2::OnNavigationCommitted(wid, nav);
                        return S_OK;
                      })
                      .Get(),
                  nullptr);
              state->webview->add_ProcessFailed(
                  Callback<ICoreWebView2ProcessFailedEventHandler>(
                      [wid](ICoreWebView2*,
                            ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
                        laufey_wv2::CancelStreamsForWindow(wid);
                        return S_OK;
                      })
                      .Get(),
                  nullptr);
            }

            state->webview->add_ScriptDialogOpening(
                Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
                    [hwnd](ICoreWebView2* sender,
                           ICoreWebView2ScriptDialogOpeningEventArgs* args)
                        -> HRESULT {
                      COREWEBVIEW2_SCRIPT_DIALOG_KIND kind;
                      args->get_Kind(&kind);

                      LPWSTR messageRaw = nullptr;
                      args->get_Message(&messageRaw);
                      std::wstring message = messageRaw ? messageRaw : L"";
                      if (messageRaw)
                        CoTaskMemFree(messageRaw);

                      // The page's text is shown as text (ShowDialogWin
                      // never hands it to a shell), modal to this window.
                      std::string msg = laufey_common::WideToUtf8(message);
                      if (kind == COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT) {
                        laufey_common::ShowDialogWin(LAUFEY_DIALOG_ALERT,
                                                     "Alert", msg, "", nullptr,
                                                     hwnd);
                        args->Accept();
                      } else if (kind ==
                                 COREWEBVIEW2_SCRIPT_DIALOG_KIND_CONFIRM) {
                        if (laufey_common::ShowDialogWin(LAUFEY_DIALOG_CONFIRM,
                                                         "Confirm", msg, "",
                                                         nullptr, hwnd)) {
                          args->Accept();
                        }
                      } else if (kind ==
                                 COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT) {
                        LPWSTR defaultTextRaw = nullptr;
                        args->get_DefaultText(&defaultTextRaw);
                        std::wstring defaultText =
                            defaultTextRaw ? defaultTextRaw : L"";
                        if (defaultTextRaw)
                          CoTaskMemFree(defaultTextRaw);
                        char* input = nullptr;
                        if (laufey_common::ShowDialogWin(
                                LAUFEY_DIALOG_PROMPT, "Prompt", msg,
                                laufey_common::WideToUtf8(defaultText), &input,
                                hwnd)) {
                          std::wstring result =
                              laufey_common::Utf8ToWide(input ? input : "");
                          free(input);
                          args->put_ResultText(result.c_str());
                          args->Accept();
                        }
                      } else if (kind ==
                                 COREWEBVIEW2_SCRIPT_DIALOG_KIND_BEFOREUNLOAD) {
                        args->Accept();
                      }

                      return S_OK;
                    })
                    .Get(),
                nullptr);

            // Reveal-on-load signal: fired when a navigation finishes. Used to
            // show a window created with LAUFEY_WINDOW_FLAG_HIDDEN only once it
            // has real content, so the empty initial frame is never seen.
            state->webview->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                    [wid](ICoreWebView2* sender,
                          ICoreWebView2NavigationCompletedEventArgs* args)
                        -> HRESULT {
                      RuntimeLoader::GetInstance()->DispatchPageLoadEvent(wid);
                      return S_OK;
                    })
                    .Get(),
                nullptr);

            state->webview_ready = true;

            if (!state->pending_url.empty()) {
              state->webview->Navigate(state->pending_url.c_str());
              state->pending_url.clear();
            }
            if (!state->pending_title.empty()) {
              SetWindowTextW(hwnd, state->pending_title.c_str());
              state->pending_title.clear();
            }

            return S_OK;
          })
          .Get());
}

void WebView2Backend::CloseWindow(uint32_t window_id) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id] { CloseWindow(window_id); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    if (state->controller) {
      state->controller->Close();
      state->controller.Reset();
    }
    // Deliberately not erased from g_hwnd_to_laufey_id here: WM_DESTROY
    // (sent synchronously by DestroyWindow) owns unregistration and the
    // last-window quit check, for this path and the WM_CLOSE path alike.
    DestroyWindow(state->hwnd);
    windows_.erase(window_id);
  }
}

void WebView2Backend::Navigate(uint32_t window_id, const std::string& url) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, url] { Navigate(window_id, url); });
    return;
  }
  std::wstring wurl = Utf8ToWide(url);
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return;
  if (state->webview_ready && state->webview) {
    state->webview->Navigate(wurl.c_str());
  } else {
    state->pending_url = wurl;
  }
}

void WebView2Backend::OpenExternalURL(const std::string& url) {
  std::wstring wurl = Utf8ToWide(url);
  ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

void WebView2Backend::SetTitle(uint32_t window_id, const std::string& title) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, title] { SetTitle(window_id, title); });
    return;
  }
  std::wstring wtitle = Utf8ToWide(title);
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return;
  if (state->webview_ready) {
    SetWindowTextW(state->hwnd, wtitle.c_str());
  } else {
    state->pending_title = wtitle;
  }
}

void WebView2Backend::ExecuteJs(uint32_t window_id, const std::string& script,
                                laufey_js_result_fn callback,
                                void* callback_data) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, script, callback, callback_data] {
      ExecuteJs(window_id, script, callback, callback_data);
    });
    return;
  }
  std::wstring wscript = Utf8ToWide(script);
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state || !state->webview_ready || !state->webview) {
    if (callback)
      callback(nullptr, nullptr, callback_data);
    return;
  }
  if (!callback) {
    state->webview->ExecuteScript(wscript.c_str(), nullptr);
  } else {
    state->webview->ExecuteScript(
        wscript.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [callback, callback_data](HRESULT hr,
                                      LPCWSTR resultJson) -> HRESULT {
              if (FAILED(hr)) {
                auto errVal = laufey::Value::String("ExecuteScript failed");
                laufey_value errLaufey(errVal);
                callback(nullptr, &errLaufey, callback_data);
                return S_OK;
              }
              if (!resultJson) {
                callback(nullptr, nullptr, callback_data);
                return S_OK;
              }
              // WebView2 returns the result as JSON in UTF-16. Convert it
              // properly (a surrogate pair is one code point, an unpaired
              // surrogate U+FFFD); it used to be narrowed code unit by code
              // unit, which garbled every non-ASCII character.
              std::string result = laufey_common::WideToUtf8(resultJson);
              auto val = json::ParseJson(result);
              if (!val)
                val = laufey::Value::Null();
              laufey_value laufey(val);
              callback(&laufey, nullptr, callback_data);
              return S_OK;
            })
            .Get());
  }
}

void WebView2Backend::Quit() {
  laufey_common::MarkQuitting();
  // PostQuitMessage posts to the CALLING thread's queue; the loop to end is
  // the UI thread's.
  if (GetCurrentThreadId() != ui_thread_id_) {
    PostThreadMessageW(ui_thread_id_, WM_QUIT, 0, 0);
    return;
  }
  PostQuitMessage(0);
}

// Window-state mutators run on the UI thread. Win32 setters like SetWindowPos
// and ShowWindow deliver messages (WM_SIZE, WM_SHOWWINDOW, ...) to the owning
// thread SYNCHRONOUSLY, and WindowProc's handlers take windows_mutex_ -- so
// calling them from another thread while holding that mutex deadlocks: the
// caller waits for the UI thread to process the sent message, the UI thread
// waits for the caller's mutex. On the UI thread the recursive_mutex makes the
// WindowProc re-entry safe.
void WebView2Backend::SetWindowSize(uint32_t window_id, int width, int height) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, width, height] {
      SetWindowSize(window_id, width, height);
    });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    // SetWindowPos ignores WM_GETMINMAXINFO; clamp to the constraints here.
    laufey_common::ClampSizeForWindow(window_id, &width, &height);
    WinSetClientSize(state->hwnd, width, height, 0);
  }
}

double WebView2Backend::GetWindowScaleFactor(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return 1.0;
  UINT dpi = GetDpiForWindow(state->hwnd);
  return dpi > 0 ? dpi / 96.0 : 1.0;
}

void WebView2Backend::GetWindowSize(uint32_t window_id, int* width,
                                    int* height) {
  // Answered on the UI thread, after the calls queued there before it. The
  // window's creation and its geometry setters are queued there from other
  // threads, so a direct read could land between them: Window::new queues
  // the HWND's creation at a default size and then the resize to the size
  // asked for, and the first window's creation holds the UI thread for
  // seconds while WebView2 creates its environment, so a get_size right
  // after the constructor read the default's client area (784x561).
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThreadSync([&] { GetWindowSize(window_id, width, height); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    RECT rect;
    if (GetClientRect(state->hwnd, &rect)) {
      double scale = WinWindowScale(state->hwnd);
      if (width)
        *width = WinToDip(rect.right, scale);
      if (height)
        *height = WinToDip(rect.bottom, scale);
    }
  }
}

void WebView2Backend::GetWindowOuterSize(uint32_t window_id, int* width,
                                         int* height) {
  // On the UI thread, ordered after the calls queued there (GetWindowSize).
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThreadSync([&] { GetWindowOuterSize(window_id, width, height); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    RECT rect;
    if (GetWindowRect(state->hwnd, &rect)) {
      double scale = WinWindowScale(state->hwnd);
      if (width)
        *width = WinToDip(rect.right - rect.left, scale);
      if (height)
        *height = WinToDip(rect.bottom - rect.top, scale);
    }
  }
}

void WebView2Backend::SetWindowPosition(uint32_t window_id, int x, int y) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread(
        [this, window_id, x, y] { SetWindowPosition(window_id, x, y); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    double scale = WinWindowScale(state->hwnd);
    SetWindowPos(state->hwnd, nullptr, WinToPx(x, scale), WinToPx(y, scale), 0,
                 0, SWP_NOSIZE | SWP_NOZORDER);
  }
}

void WebView2Backend::GetWindowInnerPosition(uint32_t window_id, int* x,
                                             int* y) {
  // On the UI thread, ordered after the calls queued there (GetWindowSize).
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThreadSync([&] { GetWindowInnerPosition(window_id, x, y); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return;
  POINT pt = {0, 0};
  if (ClientToScreen(state->hwnd, &pt)) {
    double scale = WinWindowScale(state->hwnd);
    if (x)
      *x = WinToDip(pt.x, scale);
    if (y)
      *y = WinToDip(pt.y, scale);
  }
}

void WebView2Backend::GetWindowPosition(uint32_t window_id, int* x, int* y) {
  // On the UI thread, ordered after the calls queued there (GetWindowSize).
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThreadSync([&] { GetWindowPosition(window_id, x, y); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    RECT rect;
    if (GetWindowRect(state->hwnd, &rect)) {
      double scale = WinWindowScale(state->hwnd);
      if (x)
        *x = WinToDip(rect.left, scale);
      if (y)
        *y = WinToDip(rect.top, scale);
    }
  }
}

void WebView2Backend::SetResizable(uint32_t window_id, bool resizable) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread(
        [this, window_id, resizable] { SetResizable(window_id, resizable); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    LONG style = GetWindowLong(state->hwnd, GWL_STYLE);
    if (resizable) {
      style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    } else {
      style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    SetWindowLong(state->hwnd, GWL_STYLE, style);
  }
}

bool WebView2Backend::IsResizable(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? (GetWindowLong(state->hwnd, GWL_STYLE) & WS_THICKFRAME) != 0
               : false;
}

void WebView2Backend::SetAlwaysOnTop(uint32_t window_id, bool always_on_top) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, always_on_top] {
      SetAlwaysOnTop(window_id, always_on_top);
    });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    SetWindowPos(state->hwnd, always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0,
                 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  }
}

bool WebView2Backend::IsAlwaysOnTop(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? (GetWindowLong(state->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0
               : false;
}

void WebView2Backend::SetWindowOpacity(uint32_t window_id, double opacity) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread(
        [this, window_id, opacity] { SetWindowOpacity(window_id, opacity); });
    return;
  }
  if (opacity < 0.0)
    opacity = 0.0;
  if (opacity > 1.0)
    opacity = 1.0;
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return;
  LONG ex = GetWindowLong(state->hwnd, GWL_EXSTYLE);
  if (opacity >= 1.0) {
    // Fully opaque: drop the layered style so the window renders on the normal
    // (non-redirected) path with no per-window alpha overhead. Exception:
    // click passthrough (WS_EX_TRANSPARENT) only works on a layered window,
    // so keep the style and just reset the alpha while it's active.
    if (ex & WS_EX_LAYERED) {
      if (ex & WS_EX_TRANSPARENT) {
        SetLayeredWindowAttributes(state->hwnd, 0, 255, LWA_ALPHA);
      } else {
        SetWindowLong(state->hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        RedrawWindow(state->hwnd, nullptr, nullptr,
                     RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
      }
    }
    return;
  }
  if (!(ex & WS_EX_LAYERED)) {
    SetWindowLong(state->hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
  }
  SetLayeredWindowAttributes(state->hwnd, 0, (BYTE)(opacity * 255.0 + 0.5),
                             LWA_ALPHA);
}

double WebView2Backend::GetWindowOpacity(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return 1.0;
  if (!(GetWindowLong(state->hwnd, GWL_EXSTYLE) & WS_EX_LAYERED))
    return 1.0;
  BYTE alpha = 255;
  DWORD flags = 0;
  if (GetLayeredWindowAttributes(state->hwnd, nullptr, &alpha, &flags) &&
      (flags & LWA_ALPHA)) {
    return alpha / 255.0;
  }
  return 1.0;
}

void WebView2Backend::SetClickPassthrough(uint32_t window_id, bool enabled) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, enabled] {
      SetClickPassthrough(window_id, enabled);
    });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return;
  LONG ex = GetWindowLong(state->hwnd, GWL_EXSTYLE);
  if (enabled) {
    // WS_EX_TRANSPARENT excludes the whole top-level window (children
    // included, so also the WebView2 host) from mouse hit-testing, but only
    // takes effect on a layered window.
    bool newly_layered = !(ex & WS_EX_LAYERED);
    SetWindowLong(state->hwnd, GWL_EXSTYLE,
                  ex | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    if (newly_layered) {
      // A window that just became layered renders nothing until its
      // transparency attributes are set; fully opaque keeps it visually
      // unchanged.
      SetLayeredWindowAttributes(state->hwnd, 0, 255, LWA_ALPHA);
    }
  } else {
    LONG new_ex = ex & ~WS_EX_TRANSPARENT;
    // Drop the layered style too unless a window opacity < 1.0 still needs it.
    BYTE alpha = 255;
    DWORD flags = 0;
    bool has_alpha =
        (ex & WS_EX_LAYERED) &&
        GetLayeredWindowAttributes(state->hwnd, nullptr, &alpha, &flags) &&
        (flags & LWA_ALPHA) && alpha < 255;
    if (!has_alpha)
      new_ex &= ~WS_EX_LAYERED;
    if (new_ex != ex) {
      SetWindowLong(state->hwnd, GWL_EXSTYLE, new_ex);
      if ((ex & WS_EX_LAYERED) && !(new_ex & WS_EX_LAYERED)) {
        RedrawWindow(state->hwnd, nullptr, nullptr,
                     RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
      }
    }
  }
}

bool WebView2Backend::IsClickPassthrough(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? (GetWindowLong(state->hwnd, GWL_EXSTYLE) &
                  WS_EX_TRANSPARENT) != 0
               : false;
}

bool WebView2Backend::IsVisible(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? IsWindowVisible(state->hwnd) != FALSE : false;
}

void WebView2Backend::Show(uint32_t window_id) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id] { Show(window_id); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state)
    ShowWindow(state->hwnd, SW_SHOW);
}

void WebView2Backend::Hide(uint32_t window_id) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id] { Hide(window_id); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state)
    ShowWindow(state->hwnd, SW_HIDE);
}

void WebView2Backend::Focus(uint32_t window_id) {
  // Also needs the UI thread for correctness, not just deadlock avoidance:
  // SetFocus only works on windows owned by the calling thread.
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id] { Focus(window_id); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    ShowWindow(state->hwnd, SW_SHOW);
    SetForegroundWindow(state->hwnd);
    SetFocus(state->hwnd);
  }
}

// --- Window state, constraints, screens and backdrop (API >= 38) ---
//
// The HWND work is laufey_common's (window_win.cc, shared with CEF). Sizes
// and positions here are this backend's: outer window pixels.

uint32_t WebView2Backend::WindowCapabilities() {
  return LAUFEY_WINDOW_CAP_STATE | LAUFEY_WINDOW_CAP_STATE_EVENTS |
         LAUFEY_WINDOW_CAP_SIZE_CONSTRAINTS | LAUFEY_WINDOW_CAP_SCREENS |
         LAUFEY_WINDOW_CAP_DISPLAY_EVENTS | LAUFEY_WINDOW_CAP_NORMAL_BOUNDS |
         LAUFEY_WINDOW_CAP_KEEP_ALIVE | LAUFEY_WINDOW_CAP_SET_POSITION |
         laufey_common::WinBackdropCapabilities() |
         // API 39. WebView2 shows the host the dropped files' paths only on
         // the drop (ICoreWebView2File), so ENTER / OVER carry the count.
         LAUFEY_WINDOW_CAP_FILE_DROP | LAUFEY_WINDOW_CAP_FILE_DRAG_OUT |
         LAUFEY_WINDOW_CAP_FILE_DIALOGS | LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL;
}

// ---------------------------------------------------------------------------
// Drag and drop, file dialogs (API >= 39)
// ---------------------------------------------------------------------------

namespace {

// Reads `"key":<number>` from the observer's message (which the script
// builds itself, so the shape is fixed).
bool ReadNumberField(const wchar_t* msg, const wchar_t* key, double* out) {
  const wchar_t* at = wcsstr(msg, key);
  if (!at)
    return false;
  at += wcslen(key);
  wchar_t* end = nullptr;
  double v = wcstod(at, &end);
  if (end == at)
    return false;
  *out = v;
  return true;
}

}  // namespace

bool WebView2Backend::HandleFileDropMessage(
    uint32_t window_id, const wchar_t* message,
    ICoreWebView2WebMessageReceivedEventArgs* args) {
  static const wchar_t kPrefix[] = L"{\"__laufeyFileDrop\":\"";
  size_t prefix_len = wcslen(kPrefix);
  if (wcsncmp(message, kPrefix, prefix_len) != 0)
    return false;
  // Refuse (but swallow) anything without this process's token: page script
  // can post observer-shaped messages, but can't read the token.
  std::wstring token(file_drop_token_.begin(), file_drop_token_.end());
  const wchar_t* t = message + prefix_len;
  if (wcsncmp(t, token.c_str(), token.size()) != 0 || t[token.size()] != L'"')
    return true;
  double phase = -1, x = 0, y = 0, n = 0;
  if (!ReadNumberField(message, L"\"p\":", &phase) ||
      !ReadNumberField(message, L"\"x\":", &x) ||
      !ReadNumberField(message, L"\"y\":", &y) ||
      !ReadNumberField(message, L"\"n\":", &n))
    return true;
  int p = static_cast<int>(phase);
  if (p < LAUFEY_DRAG_ENTER || p > LAUFEY_DRAG_DROP || n < 0)
    return true;
  std::vector<std::string> paths;
  if (p == LAUFEY_DRAG_DROP) {
    // The dropped File objects, as WebView2 hands them to the host with their
    // native paths.
    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
    ComPtr<ICoreWebView2ObjectCollectionView> objects;
    if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))) &&
        SUCCEEDED(args2->get_AdditionalObjects(&objects)) && objects) {
      UINT32 count = 0;
      objects->get_Count(&count);
      for (UINT32 i = 0; i < count && paths.size() < LAUFEY_MAX_DROP_PATHS;
           i++) {
        ComPtr<IUnknown> item;
        ComPtr<ICoreWebView2File> file;
        if (FAILED(objects->GetValueAtIndex(i, &item)) || !item ||
            FAILED(item.As(&file)))
          continue;
        LPWSTR path = nullptr;
        if (SUCCEEDED(file->get_Path(&path)) && path) {
          if (path[0])
            paths.push_back(laufey_common::WideToUtf8(path));
          CoTaskMemFree(path);
        }
      }
    }
  }
  laufey_common::DispatchFileDrop(window_id, p, x, y, paths,
                                  static_cast<size_t>(n));
  return true;
}

void WebView2Backend::StartFileDrag(uint32_t window_id,
                                    const char* const* paths, size_t count,
                                    const uint8_t* icon_png, size_t icon_len,
                                    laufey_drag_result_fn callback,
                                    void* user_data) {
  auto* req = new laufey_common::DragOutRequest();
  req->callback = callback;
  req->user_data = user_data;
  HWND hwnd = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
    if (auto* state = GetWindow(window_id))
      hwnd = state->hwnd;
  }
  if (!hwnd || !laufey_common::ValidateDragPaths(paths, count, &req->paths)) {
    req->Finish(LAUFEY_DRAG_RESULT_FAILED);
    delete req;
    return;
  }
  if (icon_png && icon_len > 0)
    req->icon_png.assign(icon_png, icon_png + icon_len);
  // RunDrag re-checks the HWND (IsWindow) on the UI thread.
  laufey_common::StartFileDragWin(hwnd, req);
}

uint32_t WebView2Backend::ShowFileDialog(
    uint32_t window_id, const laufey_file_dialog_options_t* options,
    laufey_file_dialog_result_fn callback, void* user_data) {
  laufey_common::ParentResolver parent;
  if (window_id != 0) {
    parent = [this, window_id]() -> void* {
      std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      return state ? state->hwnd : nullptr;
    };
  }
  return laufey_common::ShowFileDialogWin(std::move(parent), options, callback,
                                          user_data);
}

void WebView2Backend::SetWindowState(uint32_t window_id, int action) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread(
        [this, window_id, action] { SetWindowState(window_id, action); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state)
    laufey_common::WinSetWindowState(state->hwnd, window_id, action);
}

uint32_t WebView2Backend::GetWindowState(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? laufey_common::WinGetWindowState(state->hwnd, window_id) : 0;
}

void WebView2Backend::SetWindowSizeConstraints(uint32_t window_id,
                                               int min_width, int min_height,
                                               int max_width, int max_height) {
  laufey_common::SetSizeConstraints(window_id, min_width, min_height, max_width,
                                    max_height);
  RunOnUiThread([this, window_id] {
    std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state || IsZoomed(state->hwnd) || IsIconic(state->hwnd) ||
        laufey_common::WinIsFullscreen(window_id))
      return;
    RECT rect;
    if (!GetClientRect(state->hwnd, &rect))
      return;
    double scale = WinWindowScale(state->hwnd);
    int w = WinToDip(rect.right, scale);
    int h = WinToDip(rect.bottom, scale);
    // A window outside its new range is brought into it.
    if (laufey_common::ClampSizeForWindow(window_id, &w, &h))
      WinSetClientSize(state->hwnd, w, h, SWP_NOACTIVATE);
  });
}

void WebView2Backend::GetWindowSizeConstraints(uint32_t window_id,
                                               int* min_width, int* min_height,
                                               int* max_width,
                                               int* max_height) {
  laufey_common::SizeConstraints c =
      laufey_common::GetSizeConstraints(window_id);
  if (min_width)
    *min_width = c.min_width;
  if (min_height)
    *min_height = c.min_height;
  if (max_width)
    *max_width = c.max_width;
  if (max_height)
    *max_height = c.max_height;
}

int64_t WebView2Backend::GetWindowScreen(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? laufey_common::WinScreenForWindow(state->hwnd) : 0;
}

void WebView2Backend::SetDisplayChangedHandler(
    laufey_display_changed_fn handler, void* user_data) {
  laufey_common::SetDisplayChangedHandler(handler, user_data);
  if (handler)
    RunOnUiThread([] { laufey_common::WinInstallDisplayWatcher(); });
}

bool WebView2Backend::SetWindowBackdrop(uint32_t window_id, int backdrop,
                                        int /*material*/) {
  bool ok = false;
  RunOnUiThreadSync([&] {
    std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state || !laufey_common::WinSetBackdrop(state->hwnd, backdrop))
      return;
    state->backdrop = backdrop;
    // The web view paints white by default; a transparent default lets a
    // page with a transparent background show the backdrop.
    ComPtr<ICoreWebView2Controller2> controller2;
    if (state->controller && SUCCEEDED(state->controller.As(&controller2)) &&
        controller2) {
      COREWEBVIEW2_COLOR color = {255, 255, 255, 255};
      if (backdrop != LAUFEY_BACKDROP_NONE)
        color = {0, 0, 0, 0};
      controller2->put_DefaultBackgroundColor(color);
    }
    ok = true;
  });
  return ok;
}

bool WebView2Backend::GetWindowNormalBounds(uint32_t window_id, int* x, int* y,
                                            int* width, int* height) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  int px = 0, py = 0, pw = 0, ph = 0;
  if (!state || !laufey_common::WinGetNormalRect(state->hwnd, window_id, &px,
                                                 &py, &pw, &ph))
    return false;
  // The frame's origin (get_position) and the client size inside it
  // (get_size), the frame being the one the window had while normal.
  SIZE frame;
  if (!WinNormalFrame(state->hwnd, &frame)) {
    RECT r = {0, 0, 0, 0};
    AdjustWindowRectExForDpi(
        &r, static_cast<DWORD>(GetWindowLongPtrW(state->hwnd, GWL_STYLE)),
        GetMenu(state->hwnd) != nullptr,
        static_cast<DWORD>(GetWindowLongPtrW(state->hwnd, GWL_EXSTYLE)),
        GetDpiForWindow(state->hwnd));
    frame = {r.right - r.left, r.bottom - r.top};
  }
  double scale = WinWindowScale(state->hwnd);
  if (x)
    *x = WinToDip(px, scale);
  if (y)
    *y = WinToDip(py, scale);
  if (width)
    *width = WinToDip((std::max)(0L, pw - frame.cx), scale);
  if (height)
    *height = WinToDip((std::max)(0L, ph - frame.cy), scale);
  return true;
}

void WebView2Backend::PasskeyRequest(uint32_t window_id, uint32_t kind,
                                     const char* options_json,
                                     laufey_passkey_result_fn callback,
                                     void* user_data) {
  // Any thread. Refusals (no webauthn.dll, invalid options, busy) answer
  // here, synchronously; a started ceremony resolves its window on the UI
  // thread and then runs on its own worker thread (PasskeyStartWin).
  if (laufey_common::PasskeyCapabilitiesWin() == 0) {
    laufey_common::PasskeyReportNotSupported(callback, user_data);
    return;
  }
  std::shared_ptr<laufey_common::PasskeyCeremony> ceremony =
      laufey_common::PasskeyBegin(kind, options_json, callback, user_data);
  if (!ceremony)
    return;
  RunOnUiThread([this, window_id, ceremony] {
    HWND hwnd = nullptr;
    bool found = window_id == 0;
    if (window_id != 0) {
      std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
      if (auto* state = GetWindow(window_id)) {
        hwnd = state->hwnd;
        found = hwnd != nullptr;
      }
    }
    if (!found) {
      ceremony->Finish(laufey_common::PasskeyErrorEnvelope(
          laufey_common::kPasskeyUnknown,
          "window " + std::to_string(window_id) + " not found"));
      return;
    }
    // nullptr: the foreground window when it is ours, else our first visible
    // window (see PasskeyStartWin).
    laufey_common::PasskeyStartWin(ceremony, hwnd);
  });
}

bool WebView2Backend::PostUiTask(void (*task)(void*), void* data) {
  // Deliverable from construction on: the message-only dispatcher window
  // exists by then, so this works even before the first real window is
  // created (the old "post to the first window" path silently dropped the
  // task then). Fails once the window is gone or the queue is full.
  auto* td = new UiTaskData{task, data};
  if (!PostMessageW(dispatcher_hwnd_, WM_UI_TASK, 0,
                    reinterpret_cast<LPARAM>(td))) {
    delete td;
    return false;
  }
  return true;
}

void WebView2Backend::InvokeJsCallback(uint32_t window_id, uint64_t callback_id,
                                       laufey::ValuePtr args) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, callback_id, args] {
      InvokeJsCallback(window_id, callback_id, args);
    });
    return;
  }
  std::string argsJson = json::Serialize(args);
  std::wstring wscript =
      Utf8ToWide(BuildInvokeCallbackScript(callback_id, argsJson));
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      if (state.webview_ready && state.webview) {
        state.webview->ExecuteScript(wscript.c_str(), nullptr);
      }
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state && state->webview_ready && state->webview) {
      state->webview->ExecuteScript(wscript.c_str(), nullptr);
    }
  }
}

void WebView2Backend::ReleaseJsCallback(uint32_t window_id,
                                        uint64_t callback_id) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, callback_id] {
      ReleaseJsCallback(window_id, callback_id);
    });
    return;
  }
  std::wstring wscript = Utf8ToWide(BuildReleaseCallbackScript(callback_id));
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      if (state.webview_ready && state.webview) {
        state.webview->ExecuteScript(wscript.c_str(), nullptr);
      }
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state && state->webview_ready && state->webview) {
      state->webview->ExecuteScript(wscript.c_str(), nullptr);
    }
  }
}

void WebView2Backend::RespondToJsCall(uint32_t window_id, uint64_t call_id,
                                      laufey::ValuePtr result,
                                      laufey::ValuePtr error) {
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, call_id, result, error] {
      RespondToJsCall(window_id, call_id, result, error);
    });
    return;
  }
  std::string resultJson = json::Serialize(result);
  std::string errorJson = error ? json::Serialize(error) : "null";
  std::wstring wscript = Utf8ToWide(BuildRespondScript(
      call_id, resultJson, errorJson, static_cast<bool>(error)));
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state && state->webview_ready && state->webview) {
    state->webview->ExecuteScript(wscript.c_str(), nullptr);
  }
}

void WebView2Backend::Run() {
  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) {
    // App menu accelerators while a host window (not the page) has the
    // focus; the page's keys arrive through AcceleratorKeyPressed.
    if (win32_menu::TranslateWindowAccelerator(&msg))
      continue;
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

void WebView2Backend::HandleJsMessage(uint32_t window_id,
                                      const std::wstring& json,
                                      const std::string& origin) {
  std::string jsonStr = WideToUtf8(json);
  laufey::ValuePtr msg = json::ParseJson(jsonStr);
  if (!msg || !msg->IsDict())
    return;

  const auto& dict = msg->GetDict();

  auto callIdIt = dict.find("callId");
  auto methodIt = dict.find("method");
  auto argsIt = dict.find("args");

  if (callIdIt == dict.end() || methodIt == dict.end())
    return;

  // The page's own number for the call, echoed back with the answer. A
  // number the bridge script can't have made (negative, fractional, past
  // 2^53) is not a call.
  uint64_t call_id = 0;
  if (callIdIt->second->IsInt() && callIdIt->second->GetInt() >= 0) {
    call_id = static_cast<uint64_t>(callIdIt->second->GetInt());
  } else if (callIdIt->second->IsDouble()) {
    double d = callIdIt->second->GetDouble();
    if (!(d >= 0 && d <= 9007199254740992.0) ||
        d != static_cast<double>(static_cast<uint64_t>(d)))
      return;
    call_id = static_cast<uint64_t>(d);
  } else {
    return;
  }

  std::string method =
      methodIt->second->IsString() ? methodIt->second->GetString() : "";
  laufey::ValuePtr args =
      (argsIt != dict.end()) ? argsIt->second : laufey::Value::List();

  RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args,
                                         origin);
}

// ============================================================================
// Application Menu
// ============================================================================

void WebView2Backend::SetApplicationMenu(uint32_t window_id,
                                         laufey_value_t* menu_template,
                                         const laufey_backend_api_t* api,
                                         laufey_menu_click_fn on_click,
                                         void* on_click_data) {
  if (!menu_template)
    return;
  // Parsed here: `menu_template` is caller-owned and only guaranteed to
  // outlive this call. SetMenu/DrawMenuBar message the window's owning (UI)
  // thread synchronously (deadlock if called here while holding
  // windows_mutex_ — see the window-state mutators above).
  auto entries = std::make_shared<std::vector<laufey_common::MenuEntry>>(
      laufey_common::ParseMenuTemplate(menu_template, api, false));
  RunOnUiThreadSync([&] {
    HWND hwnd = nullptr;
    {
      std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state)
        hwnd = state->hwnd;
    }
    if (hwnd) {
      win32_menu::SetApplicationMenu(hwnd, *entries, on_click, on_click_data,
                                     window_id);
    }
  });
}

// ============================================================================
// Context Menu
// ============================================================================

void WebView2Backend::ShowContextMenu(uint32_t window_id, int x, int y,
                                      laufey_value_t* menu_template,
                                      const laufey_backend_api_t* api,
                                      laufey_menu_click_fn on_click,
                                      void* on_click_data) {
  if (!menu_template)
    return;
  ShowContextMenuEx(window_id, x, y, menu_template, api, on_click,
                    on_click_data, nullptr, nullptr);
}

void WebView2Backend::ShowContextMenuEx(uint32_t window_id, int x, int y,
                                        laufey_value_t* menu_template,
                                        const laufey_backend_api_t* api,
                                        laufey_menu_click_fn on_click,
                                        void* on_click_data,
                                        laufey_menu_closed_fn on_closed,
                                        void* on_closed_data) {
  // Parsed here (the template is the caller's only for this call), shown on
  // the UI thread: TrackPopupMenu runs a modal loop there until the menu
  // closes, which this call doesn't wait for.
  auto entries = std::make_shared<std::vector<laufey_common::MenuEntry>>(
      laufey_common::ParseMenuTemplate(menu_template, api, false));
  RunOnUiThread([this, window_id, x, y, entries, on_click, on_click_data,
                 on_closed, on_closed_data] {
    HWND hwnd = nullptr;
    {
      std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state)
        hwnd = state->hwnd;
    }
    // (x, y) is in window (client) DIP; the menu takes client pixels.
    double scale = hwnd ? WinWindowScale(hwnd) : 1.0;
    win32_menu::ShowContextMenu(hwnd, WinToPx(x, scale), WinToPx(y, scale),
                                *entries, on_click, on_click_data, window_id,
                                on_closed, on_closed_data);
  });
}

bool WebView2Backend::TestTriggerMenuAccelerator(uint32_t window_id,
                                                 const char* accelerator) {
  bool fired = false;
  RunOnUiThreadSync([&] {
    HWND hwnd = nullptr;
    {
      std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state)
        hwnd = state->hwnd;
    }
    fired = win32_menu::TestTriggerAccelerator(hwnd, accelerator);
  });
  return fired;
}

// ============================================================================
// DevTools
// ============================================================================

namespace {

// The visible top-level windows of process `pid` titled "DevTools..." (the
// title Chromium gives a DevTools window: "DevTools - <page>").
std::vector<HWND> DevToolsWindowsOf(DWORD pid) {
  struct Scan {
    DWORD pid;
    std::vector<HWND> found;
  } scan{pid, {}};
  if (!pid)
    return scan.found;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* sc = reinterpret_cast<Scan*>(lp);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner != sc->pid || !IsWindowVisible(hwnd))
          return TRUE;
        wchar_t title[16] = {};
        int n = GetWindowTextW(hwnd, title, 16);
        if (n >= 8 && wcsncmp(title, L"DevTools", 8) == 0)
          sc->found.push_back(hwnd);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&scan));
  return scan.found;
}

bool IsDevToolsWindowOf(HWND hwnd, DWORD pid) {
  if (!hwnd || !IsWindow(hwnd))
    return false;
  for (HWND h : DevToolsWindowsOf(pid)) {
    if (h == hwnd)
      return true;
  }
  return false;
}

}  // namespace

void WebView2Backend::OpenDevTools(uint32_t window_id) {
  if (!laufey_common::LaunchInspectable())
    return;
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id] { OpenDevTools(window_id); });
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state || !state->webview)
    return;
  DWORD pid = static_cast<DWORD>(state->browser_pid);
  std::vector<HWND> before = DevToolsWindowsOf(pid);
  state->devtools_opening++;
  state->devtools_close_pending = false;
  state->webview->OpenDevToolsWindow();
  // The window appears asynchronously (or, when this view's DevTools are
  // already open, the existing one comes to the front): find it and remember
  // it, for close_devtools / is_devtools_open.
  std::thread([this, window_id, pid, before] {
    HWND found = nullptr;
    for (int i = 0; i < 100 && !found; i++) {
      Sleep(50);
      for (HWND h : DevToolsWindowsOf(pid)) {
        if (std::find(before.begin(), before.end(), h) == before.end()) {
          found = h;
          break;
        }
      }
      if (!found && i >= 10) {
        HWND fg = GetForegroundWindow();
        if (IsDevToolsWindowOf(fg, pid))
          found = fg;
      }
    }
    std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
    auto* st = GetWindow(window_id);
    if (!st)
      return;
    st->devtools_opening--;
    if (!found) {
      if (st->devtools_opening == 0)
        st->devtools_close_pending = false;
      return;
    }
    if (st->devtools_close_pending) {
      // Closed while still opening: close what has now opened.
      st->devtools_close_pending = false;
      st->devtools_hwnd = nullptr;
      PostMessageW(found, WM_CLOSE, 0, 0);
      return;
    }
    st->devtools_hwnd = found;
  }).detach();
}

HWND WebView2Backend::FindDevToolsWindow(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state || !state->browser_pid)
    return nullptr;
  DWORD pid = static_cast<DWORD>(state->browser_pid);
  if (IsDevToolsWindowOf(state->devtools_hwnd, pid))
    return state->devtools_hwnd;
  state->devtools_hwnd = nullptr;
  // Opened some other way (F12, the context menu): attributable to this
  // view only when no other window shares its browser process.
  int sharing = 0;
  for (auto& [wid, other] : windows_) {
    if (other.browser_pid == state->browser_pid)
      sharing++;
  }
  if (sharing != 1)
    return nullptr;
  std::vector<HWND> found = DevToolsWindowsOf(pid);
  return found.empty() ? nullptr : found.front();
}

void WebView2Backend::CloseDevTools(uint32_t window_id) {
  // After an open_devtools queued before it.
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id] { CloseDevTools(window_id); });
    return;
  }
  HWND hwnd = FindDevToolsWindow(window_id);
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!hwnd) {
    // Still opening: the window doesn't exist yet and would open after this
    // close. Close it once it has been found.
    if (state && state->devtools_opening > 0)
      state->devtools_close_pending = true;
    return;
  }
  // The DevTools window belongs to the browser process; closing it is what
  // its own close button does.
  PostMessageW(hwnd, WM_CLOSE, 0, 0);
  if (state)
    state->devtools_hwnd = nullptr;
}

bool WebView2Backend::IsDevToolsOpen(uint32_t window_id) {
  bool open = false;
  // After the open / close calls queued before it.
  RunOnUiThreadSync([&] {
    std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    open = FindDevToolsWindow(window_id) != nullptr &&
           !(state && state->devtools_close_pending);
  });
  return open;
}

bool WebView2Backend::IsDevToolsEnabled(uint32_t window_id) {
  if (window_id == 0)
    return laufey_common::LaunchInspectable();
  BOOL enabled = FALSE;
  RunOnUiThreadSync([&] {
    std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state || !state->webview)
      return;
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(state->webview->get_Settings(&settings)) && settings)
      settings->get_AreDevToolsEnabled(&enabled);
  });
  return enabled != FALSE;
}

void WebView2Backend::PrintToPdf(uint32_t window_id,
                                 laufey_pdf_result_fn callback,
                                 void* callback_data) {
  if (!callback)
    return;
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, window_id, callback, callback_data] {
      PrintToPdf(window_id, callback, callback_data);
    });
    return;
  }
  ComPtr<ICoreWebView2> webview;
  {
    std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state || !state->webview_ready || !state->webview) {
      callback(nullptr, 0, "window not found", callback_data);
      return;
    }
    webview = state->webview;
  }
  ComPtr<ICoreWebView2_16> webview16;
  if (FAILED(webview.As(&webview16)) || !webview16) {
    callback(nullptr, 0,
             "print_to_pdf requires a newer WebView2 runtime "
             "(ICoreWebView2_16)",
             callback_data);
    return;
  }
  HRESULT hr = webview16->PrintToPdfStream(
      nullptr,
      Callback<ICoreWebView2PrintToPdfStreamCompletedHandler>(
          [callback, callback_data](HRESULT errorCode,
                                    IStream* pdfStream) -> HRESULT {
            if (FAILED(errorCode) || !pdfStream) {
              callback(nullptr, 0, "failed to create PDF", callback_data);
              return S_OK;
            }
            std::vector<uint8_t> buffer;
            uint8_t chunk[65536];
            ULONG bytesRead = 0;
            for (;;) {
              HRESULT rhr = pdfStream->Read(chunk, sizeof(chunk), &bytesRead);
              if (FAILED(rhr)) {
                callback(nullptr, 0, "failed to read PDF stream",
                         callback_data);
                return S_OK;
              }
              if (bytesRead == 0)
                break;
              buffer.insert(buffer.end(), chunk, chunk + bytesRead);
            }
            callback(buffer.empty() ? nullptr : buffer.data(), buffer.size(),
                     nullptr, callback_data);
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    callback(nullptr, 0, "failed to start PDF print", callback_data);
  }
}

// ============================================================================
// Dialog
// ============================================================================

int WebView2Backend::ShowDialog(uint32_t /*window_id*/, int dialog_type,
                                const std::string& title,
                                const std::string& message,
                                const std::string& default_value,
                                char** out_input_value) {
  return laufey_common::ShowDialogWin(dialog_type, title, message,
                                      default_value, out_input_value);
}

// ============================================================================
// Dock / taskbar (Windows)
// ============================================================================

void WebView2Backend::BounceDock(int type) {
  std::lock_guard<std::recursive_mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (!state.hwnd)
      continue;
    FLASHWINFO fi = {sizeof(FLASHWINFO), state.hwnd, 0, 0, 0};
    if (type == LAUFEY_DOCK_BOUNCE_CRITICAL) {
      fi.dwFlags = FLASHW_ALL | FLASHW_TIMER;
      fi.uCount = 0;
    } else {
      fi.dwFlags = FLASHW_TIMERNOFG;
      fi.uCount = 3;
    }
    FlashWindowEx(&fi);
  }
}

// Badge via title prefix. Saved-titles map lives in
// laufey_common::ApplyTitlePrefixBadge. Win32 titles are UTF-16 natively
// but ApplyTitlePrefixBadge works in UTF-8 — we round-trip through
// Utf8ToWide / WideToUtf8 here.
void WebView2Backend::SetDockBadge(const char* badge_or_null) {
  std::string badge =
      (badge_or_null && *badge_or_null) ? std::string(badge_or_null) : "";
  // GetWindowTextW/SetWindowTextW send WM_GETTEXT/WM_SETTEXT to the owning
  // (UI) thread synchronously; marshal like the window-state mutators above.
  // An empty badge means "clear", so re-passing badge.c_str() is lossless.
  if (GetCurrentThreadId() != ui_thread_id_) {
    RunOnUiThread([this, badge] { SetDockBadge(badge.c_str()); });
    return;
  }
  std::lock_guard<std::recursive_mutex> wlock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (!state.hwnd)
      continue;
    wchar_t buf[512];
    int n = GetWindowTextW(state.hwnd, buf, 512);
    std::wstring current_w(buf, n);
    // UTF-16 → UTF-8 for ApplyTitlePrefixBadge.
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, current_w.c_str(), n,
                                       nullptr, 0, nullptr, nullptr);
    std::string current_u8(utf8_len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, current_w.c_str(), n, current_u8.data(),
                        utf8_len, nullptr, nullptr);
    std::string next_u8 =
        laufey_common::ApplyTitlePrefixBadge(wid, current_u8, badge);
    std::wstring next_w = Utf8ToWide(next_u8);
    SetWindowTextW(state.hwnd, next_w.c_str());
  }
}

// ============================================================================
// Tray / status bar (Windows)
// ============================================================================
//
// Thin trampolines over backend-common/src/tray_win.cc.

uint32_t WebView2Backend::CreateTrayIcon() {
  uint32_t tray_id = laufey_common::CreateTrayIconWin();
  // The tray's hidden window must belong to the thread that pumps
  // messages (this backend's UI thread): created on the caller's thread --
  // the runtime's, which never pumps -- it received no clicks or menu
  // requests at all, which is what made a tray-only app (no visible window)
  // unusable (denoland/deno#36778). CEF already finalizes on its UI thread.
  RunOnUiThreadSync([tray_id] { laufey_common::FinalizeTrayIconWin(tray_id); });
  return tray_id;
}
void WebView2Backend::DestroyTrayIcon(uint32_t tray_id) {
  laufey_common::DestroyTrayIconWin(tray_id);
}
void WebView2Backend::SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                                  size_t len) {
  laufey_common::SetTrayIconWin(tray_id, png_bytes, len);
}
void WebView2Backend::SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                                      size_t len) {
  laufey_common::SetTrayIconDarkWin(tray_id, png_bytes, len);
}

bool WebView2Backend::GetTrayIconBounds(uint32_t tray_id, int* x, int* y,
                                        int* width, int* height) {
  int px = 0, py = 0, pw = 0, ph = 0;
  if (!laufey_common::GetTrayIconBoundsWin(tray_id, &px, &py, &pw, &ph))
    return false;
  WinScreenRectToDip(&px, &py, &pw, &ph);
  if (x)
    *x = px;
  if (y)
    *y = py;
  if (width)
    *width = pw;
  if (height)
    *height = ph;
  return true;
}
void WebView2Backend::SetTrayDoubleClickHandler(uint32_t tray_id,
                                                laufey_tray_click_fn handler,
                                                void* user_data) {
  laufey_common::SetTrayDoubleClickHandlerWin(tray_id, handler, user_data);
}
void WebView2Backend::SetTrayTooltip(uint32_t tray_id,
                                     const char* tooltip_or_null) {
  laufey_common::SetTrayTooltipWin(tray_id, tooltip_or_null);
}
void WebView2Backend::SetTrayMenu(uint32_t tray_id,
                                  laufey_value_t* menu_template,
                                  const laufey_backend_api_t* api,
                                  laufey_menu_click_fn on_click,
                                  void* on_click_data) {
  laufey_common::SetTrayMenuWin(tray_id, menu_template, api, on_click,
                                on_click_data);
}
void WebView2Backend::SetTrayClickHandler(uint32_t tray_id,
                                          laufey_tray_click_fn handler,
                                          void* user_data) {
  laufey_common::SetTrayClickHandlerWin(tray_id, handler, user_data);
}
// ============================================================================
// Notifications (WebView2 Windows)
// ============================================================================
//
// Thin trampolines over backend-common (laufey_notifications.h: toasts).

uint32_t WebView2Backend::ShowNotification(
    laufey_value_t* options, const laufey_backend_api_t* api,
    laufey_notification_event_fn on_event, void* user_data) {
  laufey_common::NotificationOptions opts =
      laufey_common::ParseNotificationOptions(options, api);
  return laufey_common::ShowNotification(opts, on_event, user_data);
}

void WebView2Backend::CloseNotification(uint32_t notification_id) {
  laufey_common::CloseNotification(notification_id);
}

// ============================================================================
// Factory Function
// ============================================================================

LaufeyBackend* CreateLaufeyBackend() {
  return new WebView2Backend();
}
