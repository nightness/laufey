// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"

#include "laufey_bridge_origin.h"
#include "app.h"
#include "custom_schemes.h"
#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_launch_config.h"
#include "laufey_menu.h"
#include "laufey_notifications.h"
#include "laufey_passkey.h"
#include "laufey_platform_features.h"
#include "laufey_title_bar.h"
#if defined(__linux__) || defined(__APPLE__)
#include "laufey_secret_store.h"
#endif
#include "laufey_auth_session.h"
#include "laufey_ui_tasks.h"
#include "laufey_scheme_registry.h"
#include "laufey_single_instance.h"
#include "laufey_sync_call.h"
#include "laufey_system.h"
#include "laufey_window.h"
#include "scheme_handler.h"

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef __linux__
#include <gtk/gtk.h>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_parser.h"
#include "include/cef_registration.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_display.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

RuntimeLoader* RuntimeLoader::instance_ = nullptr;

namespace {

// Absolute path of the running executable, or "" if it can't be determined.
std::string GetExecutablePath() {
#if defined(_WIN32)
  std::vector<wchar_t> buf(MAX_PATH);
  for (;;) {
    DWORD len =
        GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0)
      return "";
    if (len < buf.size()) {
      int size = WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, nullptr, 0,
                                     nullptr, nullptr);
      if (size <= 0)
        return "";
      std::string out(size - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, &out[0], size, nullptr,
                          nullptr);
      return out;
    }
    buf.resize(buf.size() * 2);  // truncated; grow and retry
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);  // query required length
  std::vector<char> buf(size);
  if (_NSGetExecutablePath(buf.data(), &size) != 0)
    return "";
  return std::string(buf.data());
#else
  std::vector<char> buf(4096);
  ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size());
  if (len <= 0)
    return "";
  return std::string(buf.data(), static_cast<size_t>(len));
#endif
}

bool PathExists(const std::string& path) {
#if defined(_WIN32)
  // Paths flow through this file as UTF-8 (see GetExecutablePath), so the
  // ANSI (*A) APIs would misread any non-ASCII characters in the active
  // codepage. Convert back to UTF-16 for the wide (*W) APIs.
  return GetFileAttributesW(laufey_common::Utf8ToWide(path).c_str()) !=
         INVALID_FILE_ATTRIBUTES;
#else
  return access(path.c_str(), F_OK) == 0;
#endif
}

}  // namespace

std::string LaufeyFindColocatedRuntime() {
  std::string exe = GetExecutablePath();
  if (exe.empty())
    return "";

  // Strip the extension from the filename component only (keep directory dots).
  size_t slash = exe.find_last_of("/\\");
  size_t file_start = (slash == std::string::npos) ? 0 : slash + 1;
  size_t dot = exe.find_last_of('.');
  std::string base =
      (dot != std::string::npos && dot > file_start) ? exe.substr(0, dot) : exe;

#if defined(_WIN32)
  // Not `<exe>.dll`: the executable is CEF's bootstrap, which loads the host
  // as `<exe>.dll` (docs/backends.md, "The Chromium sandbox"), so the runtime
  // takes the next name. The webview host, an executable of its own, keeps
  // `<exe>.dll`.
  std::string candidate = base + ".runtime.dll";
#elif defined(__APPLE__)
  std::string candidate = base + ".dylib";
#else
  std::string candidate = base + ".so";
#endif

  if (PathExists(candidate))
    return candidate;
  return "";
}

#ifdef _WIN32
void ConfigureWin32WindowAsPanel(void* hwnd_ptr) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd)
    return;
  // WS_EX_NOACTIVATE: showing the window doesn't steal focus / foreground
  // from the user's active app. WS_EX_TOOLWINDOW: keep it out of the taskbar
  // and Alt-Tab, matching a menu-bar / tray popover.
  LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
  ex |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
  SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
}
#endif

// Runs `fn` synchronously on the CEF UI thread (inline when already there).
// Through the UI task dispatcher (laufey_ui_tasks.h), so a call never
// outlives the loop: false when `fn` did not run because the loop had ended
// or CEF refused the task, and the caller answers with its defaults.
template <typename F>
static bool cef_invoke_sync(F&& fn) {
  if (CefCurrentlyOn(TID_UI)) {
    fn();
    return true;
  }
  return laufey_common::RunOnUiThreadAndWait(fn);
}

// --- Backend API functions (cross-platform, using CEF Views) ---

static void Backend_Navigate(void* data, uint32_t window_id, const char* url) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser && url) {
    std::string url_str(url);
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, std::string u) {
                              b->GetMainFrame()->LoadURL(u);
                            },
                            browser, url_str));
  }
}

static void Backend_SetTitle(void* data, uint32_t window_id,
                             const char* title) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  // The embedder set an explicit title; keep the page's document.title / URL
  // from overwriting it later (see LaufeyHandler::OnTitleChange).
  loader->MarkExplicitTitle(window_id);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser && title) {
    std::string title_str(title);
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, std::string t) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->SetTitle(t);
                                }
                              }
                            },
                            browser, title_str));
  }
}

static void Backend_ExecuteJs(void* data, uint32_t window_id,
                              const char* script, laufey_js_result_fn callback,
                              void* callback_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser || !script) {
    if (callback)
      callback(nullptr, nullptr, callback_data);
    return;
  }

  if (!callback) {
    // Fire-and-forget: use the simple path
    std::string script_str(script);
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, std::string s) {
                              b->GetMainFrame()->ExecuteJavaScript(s, "", 0);
                            },
                            browser, script_str));
    return;
  }

  // With callback: send IPC to renderer for eval with result
  uint64_t eval_id = loader->StoreEvalCallback(callback, callback_data);
  std::string script_str(script);
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](CefRefPtr<CefBrowser> b, uint64_t id, std::string s) {
                    CefRefPtr<CefProcessMessage> msg =
                        CefProcessMessage::Create("laufey_eval");
                    CefRefPtr<CefListValue> args = msg->GetArgumentList();
                    args->SetDouble(0, static_cast<double>(id));
                    args->SetString(1, s);
                    b->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
                  },
                  browser, eval_id, script_str));
}

// Ends the loop the way closing the last window does: every browser is
// closed (marked close-allowed, so no close-requested negotiation) and
// LaufeyHandler::OnBeforeClose ends the loop when the last one is gone. With
// no window open the loop ends right away. Quitting the loop directly with
// live browsers would leave CEF to shut down under them; and on macOS the
// loop is [NSApp run], which CefQuitMessageLoop does not stop.
static void Backend_Quit(void* data) {
  laufey_common::MarkQuitting();
#if defined(__linux__)
  // A GTK modal (an alert, a script dialog) is a nested loop on TID_UI that
  // would hold the end of the loop until someone dismissed it: end it, as a
  // cancel.
  laufey_common::CancelGtkDialogsForQuit();
#endif
  CefPostTask(TID_UI, base::BindOnce([]() {
                auto* loader = RuntimeLoader::GetInstance();
                std::vector<CefRefPtr<CefBrowser>> browsers =
                    loader->GetAllBrowsers();
                if (browsers.empty()) {
                  LaufeyQuitMainLoop();
                  return;
                }
                for (const auto& browser : browsers) {
                  uint32_t wid = loader->GetLaufeyIdForBrowser(browser);
                  if (wid > 0)
                    loader->MarkCloseAllowed(wid);
                  browser->GetHost()->CloseBrowser(true);
                }
              }));
}

// exit_app (API 46): quit() with an exit code. The host returns the code once
// CefShutdown has written the profile (on Windows through
// laufey_common::EndProcess), and RuntimeLoader::Shutdown doesn't wait for a
// runtime thread that may be blocked for good in its exit().
static void Backend_ExitApp(void* data, int exit_code) {
  laufey_common::MarkExitRequested(exit_code);
  Backend_Quit(data);
}

void LaufeyRequestQuit() {
  Backend_Quit(nullptr);
}

// Window sizes are the page area, the browser view (as window.innerWidth /
// innerHeight see it), in DIP; CefWindow's size is the whole window, so the
// frame around the page is added when resizing. UI thread.
static CefSize CefFrameAroundPage(CefRefPtr<CefBrowserView> browser_view) {
  CefRefPtr<CefWindow> window = browser_view->GetWindow();
  if (!window)
    return CefSize();
  CefSize outer = window->GetSize();
  CefSize page = browser_view->GetSize();
  if (page.width <= 0 || page.height <= 0) {
    // Not laid out yet: the client area is what the page will fill.
    CefRect client = window->GetClientAreaBoundsInScreen();
    page = CefSize(client.width, client.height);
  }
  if (page.width <= 0 || page.height <= 0)
    return CefSize();
  return CefSize((std::max)(0, outer.width - page.width),
                 (std::max)(0, outer.height - page.height));
}

static void CefSetPageSize(CefRefPtr<CefBrowserView> browser_view, int width,
                           int height) {
  CefRefPtr<CefWindow> window = browser_view->GetWindow();
  if (!window)
    return;
  CefSize frame = CefFrameAroundPage(browser_view);
  window->SetSize(CefSize(width + frame.width, height + frame.height));
}

static void Backend_SetWindowSize(void* data, uint32_t window_id, int width,
                                  int height) {
  // Programmatic resizes are clamped to the size constraints (API 38).
  laufey_common::ClampSizeForWindow(window_id, &width, &height);
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, int w, int h) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view)
                                CefSetPageSize(browser_view, w, h);
                            },
                            browser, width, height));
  }
}

static void Backend_GetWindowSize(void* data, uint32_t window_id, int* width,
                                  int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  int w = 0, h = 0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      auto window = browser_view ? browser_view->GetWindow() : nullptr;
      if (window) {
        CefSize outer = window->GetSize();
        CefSize frame = CefFrameAroundPage(browser_view);
        w = (std::max)(0, outer.width - frame.width);
        h = (std::max)(0, outer.height - frame.height);
      }
    });
  }
  if (width)
    *width = w;
  if (height)
    *height = h;
}

static void Backend_GetWindowOuterSize(void* data, uint32_t window_id,
                                       int* width, int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  int w = 0, h = 0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (!browser_view)
        return;
      auto window = browser_view->GetWindow();
      if (!window)
        return;
#ifdef _WIN32
      // The window rectangle in DIP, like CefWindow::GetPosition.
      HWND hwnd = window->GetWindowHandle();
      RECT rect;
      UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
      if (hwnd && dpi && GetWindowRect(hwnd, &rect)) {
        w = static_cast<int>(
            std::lround((rect.right - rect.left) * 96.0 / dpi));
        h = static_cast<int>(
            std::lround((rect.bottom - rect.top) * 96.0 / dpi));
        return;
      }
#elif defined(__APPLE__)
      if (GetNSWindowOuterSize(window->GetWindowHandle(), &w, &h))
        return;
#endif
      CefSize size = window->GetSize();
      w = size.width;
      h = size.height;
    });
  }
  if (width)
    *width = w;
  if (height)
    *height = h;
}

static void Backend_SetWindowPosition(void* data, uint32_t window_id, int x,
                                      int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, int px, int py) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->SetPosition(CefPoint(px, py));
                                }
                              }
                            },
                            browser, x, y));
  }
}

static void Backend_GetWindowPosition(void* data, uint32_t window_id, int* x,
                                      int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  int px = 0, py = 0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          CefPoint pos = window->GetPosition();
          px = pos.x;
          py = pos.y;
        }
      }
    });
  }
  if (x)
    *x = px;
  if (y)
    *y = py;
}

static void Backend_GetWindowInnerPosition(void* data, uint32_t window_id,
                                           int* x, int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  int px = 0, py = 0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        CefRect bounds = browser_view->GetBoundsInScreen();
        px = bounds.x;
        py = bounds.y;
      }
    });
  }
  if (x)
    *x = px;
  if (y)
    *y = py;
}

static void Backend_SetResizable(void* data, uint32_t window_id,
                                 bool resizable) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, bool r) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (!browser_view)
                                return;
                              auto window = browser_view->GetWindow();
                              if (!window)
                                return;
#ifdef _WIN32
                              HWND hwnd = window->GetWindowHandle();
                              LONG style = GetWindowLong(hwnd, GWL_STYLE);
                              if (r) {
                                style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
                              } else {
                                style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
                              }
                              SetWindowLong(hwnd, GWL_STYLE, style);
#elif defined(__APPLE__)
                              SetNSWindowResizable(window->GetWindowHandle(),
                                                   r);
#elif defined(__linux__)
                              SetLinuxWindowResizable(window->GetWindowHandle(),
                                                      r);
#endif
                            },
                            browser, resizable));
  }
}

static bool Backend_IsResizable(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = true;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (!browser_view)
        return;
      auto window = browser_view->GetWindow();
      if (!window)
        return;
#ifdef _WIN32
      HWND hwnd = window->GetWindowHandle();
      LONG style = GetWindowLong(hwnd, GWL_STYLE);
      result = (style & WS_THICKFRAME) != 0;
#elif defined(__APPLE__)
      result = IsNSWindowResizable(window->GetWindowHandle());
#elif defined(__linux__)
      result = IsLinuxWindowResizable(window->GetWindowHandle());
#endif
    });
  }
  return result;
}

static void Backend_SetAlwaysOnTop(void* data, uint32_t window_id,
                                   bool always_on_top) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, bool on_top) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->SetAlwaysOnTop(on_top);
                                }
                              }
                            },
                            browser, always_on_top));
  }
}

static bool Backend_IsAlwaysOnTop(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = false;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          result = window->IsAlwaysOnTop();
        }
      }
    });
  }
  return result;
}

static void Backend_SetWindowOpacity(void* data, uint32_t window_id,
                                     double opacity) {
  if (opacity < 0.0)
    opacity = 0.0;
  if (opacity > 1.0)
    opacity = 1.0;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](CefRefPtr<CefBrowser> b, double o) {
                      auto browser_view = CefBrowserView::GetForBrowser(b);
                      if (!browser_view)
                        return;
                      auto window = browser_view->GetWindow();
                      if (!window)
                        return;
#ifdef _WIN32
                      HWND hwnd = window->GetWindowHandle();
                      LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
                      if (o >= 1.0) {
                        if (ex & WS_EX_LAYERED) {
                          if (ex & WS_EX_TRANSPARENT) {
                            // Click passthrough needs the layered style;
                            // keep it and just reset the alpha.
                            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
                          } else {
                            SetWindowLong(hwnd, GWL_EXSTYLE,
                                          ex & ~WS_EX_LAYERED);
                            RedrawWindow(hwnd, nullptr, nullptr,
                                         RDW_ERASE | RDW_INVALIDATE |
                                             RDW_FRAME | RDW_ALLCHILDREN);
                          }
                        }
                      } else {
                        if (!(ex & WS_EX_LAYERED))
                          SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
                        SetLayeredWindowAttributes(
                            hwnd, 0, (BYTE)(o * 255.0 + 0.5), LWA_ALPHA);
                      }
#elif defined(__APPLE__)
                      SetNSWindowOpacity(window->GetWindowHandle(), o);
#elif defined(__linux__)
                      SetLinuxWindowOpacity(window->GetWindowHandle(), o);
#endif
                    },
                    browser, opacity));
  }
}

static double Backend_GetWindowOpacity(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  double result = 1.0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (!browser_view)
        return;
      auto window = browser_view->GetWindow();
      if (!window)
        return;
#ifdef _WIN32
      HWND hwnd = window->GetWindowHandle();
      if (!(GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED))
        return;
      BYTE alpha = 255;
      DWORD flags = 0;
      if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) &&
          (flags & LWA_ALPHA)) {
        result = alpha / 255.0;
      }
#elif defined(__APPLE__)
      result = GetNSWindowOpacity(window->GetWindowHandle());
#elif defined(__linux__)
      result = GetLinuxWindowOpacity(window->GetWindowHandle());
#endif
    });
  }
  return result;
}

static double Backend_GetWindowScaleFactor(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  double result = 1.0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (!browser_view)
        return;
      auto window = browser_view->GetWindow();
      if (!window)
        return;
      auto display = window->GetDisplay();
      if (display)
        result = display->GetDeviceScaleFactor();
    });
  }
  return result;
}

static void Backend_SetClickPassthrough(void* data, uint32_t window_id,
                                        bool enabled) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(
        TID_UI,
        base::BindOnce(
            [](CefRefPtr<CefBrowser> b, bool en) {
              auto browser_view = CefBrowserView::GetForBrowser(b);
              if (!browser_view)
                return;
              auto window = browser_view->GetWindow();
              if (!window)
                return;
#ifdef _WIN32
              HWND hwnd = window->GetWindowHandle();
              LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
              if (en) {
                // WS_EX_TRANSPARENT excludes the whole top-level window
                // (children included, so also CEF's widget windows) from
                // mouse hit-testing, but only on a layered window.
                bool newly_layered = !(ex & WS_EX_LAYERED);
                SetWindowLong(hwnd, GWL_EXSTYLE,
                              ex | WS_EX_TRANSPARENT | WS_EX_LAYERED);
                if (newly_layered) {
                  // A window that just became layered renders nothing until
                  // its transparency attributes are set; fully opaque keeps
                  // it visually unchanged.
                  SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
                }
              } else {
                LONG new_ex = ex & ~WS_EX_TRANSPARENT;
                // Drop the layered style too unless a window opacity < 1.0
                // still needs it.
                BYTE alpha = 255;
                DWORD flags = 0;
                bool has_alpha =
                    (ex & WS_EX_LAYERED) &&
                    GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) &&
                    (flags & LWA_ALPHA) && alpha < 255;
                if (!has_alpha)
                  new_ex &= ~WS_EX_LAYERED;
                if (new_ex != ex) {
                  SetWindowLong(hwnd, GWL_EXSTYLE, new_ex);
                  if ((ex & WS_EX_LAYERED) && !(new_ex & WS_EX_LAYERED)) {
                    RedrawWindow(hwnd, nullptr, nullptr,
                                 RDW_ERASE | RDW_INVALIDATE | RDW_FRAME |
                                     RDW_ALLCHILDREN);
                  }
                }
              }
#elif defined(__APPLE__)
              SetNSWindowClickPassthrough(window->GetWindowHandle(), en);
#elif defined(__linux__)
              SetLinuxWindowClickPassthrough(window->GetWindowHandle(), en);
#endif
            },
            browser, enabled));
  }
}

static bool Backend_IsClickPassthrough(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = false;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (!browser_view)
        return;
      auto window = browser_view->GetWindow();
      if (!window)
        return;
#ifdef _WIN32
      HWND hwnd = window->GetWindowHandle();
      result = (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
#elif defined(__APPLE__)
      result = IsNSWindowClickPassthrough(window->GetWindowHandle());
#elif defined(__linux__)
      result = IsLinuxWindowClickPassthrough(window->GetWindowHandle());
#endif
    });
  }
  return result;
}

static void Backend_SetClickPassthroughForward(void* data, uint32_t window_id,
                                               bool forward) {
#ifdef __APPLE__
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](uint32_t wid, bool fwd) {
                              SetNSWindowClickPassthroughForward(wid, fwd);
                            },
                            window_id, forward));
  }
#else
  // Forwarding needs a global input observer; not implemented on this
  // platform yet (see docs/window-management.md).
  (void)data;
  (void)window_id;
  (void)forward;
#endif
}

static bool Backend_IsClickPassthroughForward(void* data, uint32_t window_id) {
#ifdef __APPLE__
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = false;
  if (browser) {
    cef_invoke_sync(
        [&] { result = IsNSWindowClickPassthroughForward(window_id); });
  }
  return result;
#else
  (void)data;
  (void)window_id;
  return false;
#endif
}

static bool Backend_IsVisible(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = false;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          result = window->IsVisible();
        }
      }
    });
  }
  return result;
}

static void Backend_Show(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->Show();
                                }
                              }
                            },
                            browser));
  }
}

static void Backend_Hide(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->Hide();
                                }
                              }
                            },
                            browser));
  }
}

static void Backend_Focus(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->Show();
                                  window->Activate();
                                }
                              }
                            },
                            browser));
  }
}

static void Backend_PostUiTask(void* data, void (*task)(void*),
                               void* task_data) {
  if (task) {
    CefPostTask(TID_UI, base::BindOnce([](void (*t)(void*), void* d) { t(d); },
                                       task, task_data));
  }
}

static void Backend_SetSecondInstanceHandler(void* /*data*/,
                                             laufey_second_instance_fn handler,
                                             void* user_data) {
  laufey_common::SetSecondInstanceHandler(handler, user_data);
}

// --- Passkeys (API >= 37) ---
//
// macOS and Windows run real ceremonies (backend-common passkey_mac.mm /
// passkey_win.cc); Linux has no platform API and answers not_supported.

static uint32_t Backend_PasskeyCapabilities(void* /*data*/) {
#if defined(__APPLE__)
  return laufey_common::PasskeyCapabilitiesMac();
#elif defined(_WIN32)
  return laufey_common::PasskeyCapabilitiesWin();
#else
  return 0;
#endif
}

static void Backend_PasskeyRequest(void* data, uint32_t window_id,
                                   uint32_t kind, const char* options_json,
                                   laufey_passkey_result_fn callback,
                                   void* user_data) {
  // Any thread. Refusals (no API, invalid options, busy) answer here,
  // synchronously; a started ceremony resolves its window on TID_UI (the
  // main thread on macOS).
  if (!callback)
    return;
  if (Backend_PasskeyCapabilities(data) == 0) {
    laufey_common::PasskeyReportNotSupported(callback, user_data);
    return;
  }
#if defined(__APPLE__) || defined(_WIN32)
  std::shared_ptr<laufey_common::PasskeyCeremony> ceremony =
      laufey_common::PasskeyBegin(kind, options_json, callback, user_data);
  if (!ceremony)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser =
      window_id != 0 ? loader->GetBrowserForWindow(window_id) : nullptr;
  if (window_id != 0 && !browser) {
    ceremony->Finish(laufey_common::PasskeyErrorEnvelope(
        laufey_common::kPasskeyUnknown,
        "window " + std::to_string(window_id) + " not found"));
    return;
  }
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](CefRefPtr<CefBrowser> b,
                     std::shared_ptr<laufey_common::PasskeyCeremony> c) {
                    void* native = nullptr;
                    if (b) {
                      auto browser_view = CefBrowserView::GetForBrowser(b);
                      auto window =
                          browser_view ? browser_view->GetWindow() : nullptr;
                      if (!window) {
                        c->Finish(laufey_common::PasskeyErrorEnvelope(
                            laufey_common::kPasskeyUnknown,
                            "the window has no native handle"));
                        return;
                      }
#if defined(__APPLE__)
                      native = NSWindowForCefHandle(window->GetWindowHandle());
#else
                      native =
                          reinterpret_cast<void*>(window->GetWindowHandle());
#endif
                    }
    // nullptr: the key / foreground window of the app.
#if defined(__APPLE__)
                    laufey_common::PasskeyStartMac(c, native);
#else
                    laufey_common::PasskeyStartWin(c, native);
#endif
                  },
                  browser, ceremony));
#else
  (void)data;
  (void)window_id;
  (void)kind;
  (void)options_json;
#endif
}

// --- UI-thread tasks (API >= 42) ---
//
// The UI thread is CEF's TID_UI (the process main thread, which runs
// CefRunMessageLoop / [NSApp run]); RuntimeLoader::Load binds it.

static void Backend_DispatchUiTask(void* /*data*/, laufey_ui_task_fn task,
                                   void* task_data) {
  laufey_common::UiTaskDispatcher::Get().Dispatch(task, task_data);
}

static bool Backend_IsUiThread(void* /*data*/) {
  return laufey_common::UiTaskDispatcher::Get().IsUiThread();
}

// --- Auth session (API >= 42) ---
//
// macOS runs ASWebAuthenticationSession (backend-common
// auth_session_mac.mm); Windows and Linux have no OS auth session and
// answer not_supported (RFC 8252: the embedder opens the system browser).

static uint32_t Backend_AuthSessionCapabilities(void* /*data*/) {
  return laufey_common::AuthSessionCapabilities();
}

static void Backend_AuthSessionStart(void* data, uint32_t window_id,
                                     const char* url, const char* callback,
                                     uint32_t flags,
                                     laufey_auth_session_result_fn on_result,
                                     void* user_data) {
  // Any thread. Refusals answer here, synchronously; a started session
  // resolves its window on TID_UI (the main thread on macOS).
  std::shared_ptr<laufey_common::AuthSession> session =
      laufey_common::AuthSessionBegin(laufey_common::AuthSessionCapabilities(),
                                      url, callback, flags, on_result,
                                      user_data);
  if (!session)
    return;
#if defined(__APPLE__)
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser =
      window_id != 0 ? loader->GetBrowserForWindow(window_id) : nullptr;
  if (window_id != 0 && !browser) {
    session->Finish(LAUFEY_AUTH_SESSION_INVALID,
                    "window " + std::to_string(window_id) + " not found");
    return;
  }
  bool posted = CefPostTask(
      TID_UI, base::BindOnce(
                  [](CefRefPtr<CefBrowser> b,
                     std::shared_ptr<laufey_common::AuthSession> s) {
                    void* native = nullptr;
                    if (b) {
                      auto browser_view = CefBrowserView::GetForBrowser(b);
                      auto window =
                          browser_view ? browser_view->GetWindow() : nullptr;
                      if (!window) {
                        s->Finish(LAUFEY_AUTH_SESSION_INVALID,
                                  "the window has no native handle");
                        return;
                      }
                      native = NSWindowForCefHandle(window->GetWindowHandle());
                    }
                    // nullptr: the key / main window of the app.
                    laufey_common::AuthSessionStartMac(s, native);
                  },
                  browser, session));
  if (!posted) {
    session->Finish(LAUFEY_AUTH_SESSION_CANCELLED, "the app is quitting");
  }
#else
  // AuthSessionBegin refused it: the capabilities are 0 here.
  (void)data;
  (void)window_id;
#endif
}

static bool Backend_TestCancelAuthSession(void* /*data*/) {
  return laufey_common::AuthSessionCancelCurrent(
      "the user cancelled the sign-in");
}

// API >= 43: the app cancels the running session (no session runs off
// macOS, so it answers false there).
static bool Backend_AuthSessionCancel(void* /*data*/) {
  return laufey_common::AuthSessionCancelCurrent(
      "the app cancelled the sign-in");
}

// --- CefValue <-> laufey::Value conversion (IPC boundary only) ---
//
// Values cross the renderer<->browser process boundary as CefValue trees, but
// the shared marshalling layer operates on laufey::Value. Convert once on the
// way in (incoming JS args / eval results) and once on the way out (responses,
// callback args). A JS function is encoded by the renderer as a dictionary
// {"__callback__": "<id>"}; decode it to laufey::Value::Callback so that
// value_is_callback works, matching the webview backend.

static laufey::ValuePtr CefValueToLaufey(CefRefPtr<CefValue> v) {
  if (!v)
    return laufey::Value::Null();
  switch (v->GetType()) {
    case VTYPE_BOOL:
      return laufey::Value::Bool(v->GetBool());
    case VTYPE_INT:
      return laufey::Value::Int(v->GetInt());
    case VTYPE_DOUBLE:
      return laufey::Value::Double(v->GetDouble());
    case VTYPE_STRING:
      return laufey::Value::String(v->GetString().ToString());
    case VTYPE_BINARY: {
      CefRefPtr<CefBinaryValue> bin = v->GetBinary();
      std::vector<uint8_t> buf(bin->GetSize());
      if (!buf.empty())
        bin->GetData(buf.data(), buf.size(), 0);
      return laufey::Value::Binary(buf.data(), buf.size());
    }
    case VTYPE_LIST: {
      CefRefPtr<CefListValue> list = v->GetList();
      auto out = laufey::Value::List();
      for (size_t i = 0; i < list->GetSize(); ++i) {
        out->GetList().push_back(CefValueToLaufey(list->GetValue(i)));
      }
      return out;
    }
    case VTYPE_DICTIONARY: {
      CefRefPtr<CefDictionaryValue> dict = v->GetDictionary();
      if (dict->HasKey("__callback__")) {
        // Renderer-supplied. CEF builds with -fno-exceptions, so parse without
        // std::stoull (which throws); strtoull returns 0 on a malformed id.
        std::string id = dict->GetString("__callback__").ToString();
        uint64_t cb_id = std::strtoull(id.c_str(), nullptr, 10);
        return laufey::Value::Callback(cb_id);
      }
      auto out = laufey::Value::Dict();
      CefDictionaryValue::KeyList keys;
      dict->GetKeys(keys);
      for (const auto& key : keys) {
        out->GetDict()[key.ToString()] = CefValueToLaufey(dict->GetValue(key));
      }
      return out;
    }
    case VTYPE_NULL:
    case VTYPE_INVALID:
    default:
      return laufey::Value::Null();
  }
}

static CefRefPtr<CefValue> LaufeyToCefValue(const laufey::ValuePtr& v) {
  CefRefPtr<CefValue> out = CefValue::Create();
  if (!v) {
    out->SetNull();
    return out;
  }
  switch (v->type) {
    case laufey::ValueType::Bool:
      out->SetBool(v->GetBool());
      break;
    case laufey::ValueType::Int:
      out->SetInt(v->GetInt());
      break;
    case laufey::ValueType::Double:
      out->SetDouble(v->GetDouble());
      break;
    case laufey::ValueType::String:
      out->SetString(v->GetString());
      break;
    case laufey::ValueType::Binary: {
      const auto& bin = v->GetBinary();
      out->SetBinary(CefBinaryValue::Create(bin.data.data(), bin.data.size()));
      break;
    }
    case laufey::ValueType::List: {
      CefRefPtr<CefListValue> list = CefListValue::Create();
      const auto& items = v->GetList();
      for (size_t i = 0; i < items.size(); ++i) {
        list->SetValue(i, LaufeyToCefValue(items[i]));
      }
      out->SetList(list);
      break;
    }
    case laufey::ValueType::Dict: {
      CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
      for (const auto& pair : v->GetDict()) {
        dict->SetValue(pair.first, LaufeyToCefValue(pair.second));
      }
      out->SetDictionary(dict);
      break;
    }
    case laufey::ValueType::Callback: {
      CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
      dict->SetString("__callback__", std::to_string(v->GetCallbackId()));
      out->SetDictionary(dict);
      break;
    }
    case laufey::ValueType::Null:
    default:
      out->SetNull();
      break;
  }
  return out;
}

// Build a CefListValue from a laufey list value (empty list otherwise).
static CefRefPtr<CefListValue> LaufeyListToCef(const laufey::ValuePtr& v) {
  CefRefPtr<CefListValue> list = CefListValue::Create();
  if (v && v->IsList()) {
    const auto& items = v->GetList();
    for (size_t i = 0; i < items.size(); ++i) {
      list->SetValue(i, LaufeyToCefValue(items[i]));
    }
  }
  return list;
}

// --- JS call/callback handling ---

static void Backend_SetJsCallHandler(void* data, laufey_js_call_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandler(handler, user_data);
}

static void Backend_SetJsCallHandlerEx(void* data, laufey_js_call_ex_fn handler,
                                       void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandlerEx(handler, user_data);
}

static void Backend_JsCallRespond(void* data, uint64_t call_id,
                                  laufey_value_t* result,
                                  laufey_value_t* error) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  // An id this backend never issued, or a second answer, reaches nothing.
  laufey_common::JsCallRoute route;
  if (!loader->TakeJsCall(call_id, &route))
    return;
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(route.window_id);
  if (!browser)
    return;

  CefRefPtr<CefProcessMessage> msg =
      CefProcessMessage::Create("laufey_response");
  CefRefPtr<CefListValue> args = msg->GetArgumentList();
  // IDs are 64-bit; carry as double (exact to 2^53) since CefValue has no
  // int64.
  args->SetDouble(0, static_cast<double>(route.page_call_id));

  if (result && result->value) {
    args->SetValue(1, LaufeyToCefValue(result->value));
  } else {
    CefRefPtr<CefValue> null_val = CefValue::Create();
    null_val->SetNull();
    args->SetValue(1, null_val);
  }

  if (error && error->value) {
    args->SetValue(2, LaufeyToCefValue(error->value));
  } else {
    CefRefPtr<CefValue> null_val = CefValue::Create();
    null_val->SetNull();
    args->SetValue(2, null_val);
  }

  CefPostTask(TID_UI,
              base::BindOnce(
                  [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
                    b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
                  },
                  browser, msg));
}

// --- Custom URL scheme handling ---

static void Backend_RegisterSchemeHandler(void* data, const char* scheme,
                                          laufey_scheme_request_fn handler,
                                          laufey_scheme_cancel_fn on_cancel,
                                          void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetSchemeRequestHandler(scheme ? scheme : "", handler, on_cancel,
                                  user_data);
}

static intptr_t Backend_SchemeRequestReadBody(
    void* /*data*/, laufey_scheme_exchange_t* exchange, uint8_t* buf,
    size_t cap) {
  return reinterpret_cast<LaufeySchemeHandler*>(exchange)->ReadRequestBody(buf,
                                                                           cap);
}

static void Backend_SchemeResponseBegin(void* /*data*/,
                                        laufey_scheme_exchange_t* exchange,
                                        int status, const char* headers,
                                        size_t headers_len) {
  reinterpret_cast<LaufeySchemeHandler*>(exchange)->Begin(status, headers,
                                                          headers_len);
}

static intptr_t Backend_SchemeResponseWrite(void* /*data*/,
                                            laufey_scheme_exchange_t* exchange,
                                            const uint8_t* buf, size_t len) {
  return reinterpret_cast<LaufeySchemeHandler*>(exchange)->WriteResponse(buf,
                                                                         len);
}

static void Backend_SchemeResponseFinish(void* /*data*/,
                                         laufey_scheme_exchange_t* exchange) {
  reinterpret_cast<LaufeySchemeHandler*>(exchange)->FinishResponse();
}

static void Backend_InvokeJsCallback(void* data, uint64_t callback_id,
                                     laufey_value_t* args) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  CefRefPtr<CefProcessMessage> msg =
      CefProcessMessage::Create("laufey_callback");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  msgArgs->SetDouble(0, static_cast<double>(callback_id));

  msgArgs->SetList(1, LaufeyListToCef(args ? args->value : nullptr));

  loader->ForEachBrowser([&msg](CefRefPtr<CefBrowser> browser) {
    CefPostTask(
        TID_UI,
        base::BindOnce(
            [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
              b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
            },
            browser, msg));
  });
}

static void Backend_SetKeyboardEventHandler(void* data,
                                            laufey_keyboard_event_fn handler,
                                            void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetKeyboardEventHandler(handler, user_data);
}

static void Backend_SetMouseClickHandler(void* data,
                                         laufey_mouse_click_fn handler,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseClickHandler(handler, user_data);
}

static void Backend_SetMouseMoveHandler(void* data,
                                        laufey_mouse_move_fn handler,
                                        void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseMoveHandler(handler, user_data);
}

static void Backend_SetWheelHandler(void* data, laufey_wheel_fn handler,
                                    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetWheelHandler(handler, user_data);
}

static void Backend_SetCursorEnterLeaveHandler(
    void* data, laufey_cursor_enter_leave_fn handler, void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCursorEnterLeaveHandler(handler, user_data);
}

static void Backend_SetFocusedHandler(void* data, laufey_focused_fn handler,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetFocusedHandler(handler, user_data);
}

static void Backend_SetResizeHandler(void* data, laufey_resize_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetResizeHandler(handler, user_data);
}

static void Backend_SetMoveHandler(void* data, laufey_move_fn handler,
                                   void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMoveHandler(handler, user_data);
}

static void Backend_ReleaseJsCallback(void* data, uint64_t callback_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  CefRefPtr<CefProcessMessage> msg =
      CefProcessMessage::Create("laufey_release_callback");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  msgArgs->SetDouble(0, static_cast<double>(callback_id));

  loader->ForEachBrowser([&msg](CefRefPtr<CefBrowser> browser) {
    CefPostTask(
        TID_UI,
        base::BindOnce(
            [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
              b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
            },
            browser, msg));
  });
}

// --- Platform-specific menu (stub on Windows, implemented in runtime_loader.mm
// on macOS) ---

#if defined(_WIN32) || defined(__linux__)
#if defined(_WIN32)
#include <win32_menu.h>
#endif
#include "views_menu.h"

// The application menu is a CEF Views menu bar on Windows and Linux (see
// views_menu.h): a native menu bar can't be attached to a Views window.
static void Backend_SetApplicationMenu(void* data, uint32_t window_id,
                                       laufey_value_t* menu_template,
                                       laufey_menu_click_fn on_click,
                                       void* on_click_data) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  // Parsed here: the template is the caller's only for this call.
  std::vector<laufey_common::MenuEntry> entries =
      laufey_common::ParseMenuTemplate(menu_template, &loader->GetBackendApi(),
                                       false);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser)
    return;
  CefPostTask(
      TID_UI,
      base::BindOnce(
          [](CefRefPtr<CefBrowser> b, uint32_t wid,
             std::vector<laufey_common::MenuEntry> items,
             laufey_menu_click_fn fn, void* d) {
            CefRefPtr<CefBrowserView> view = CefBrowserView::GetForBrowser(b);
            CefRefPtr<CefWindow> window = view ? view->GetWindow() : nullptr;
            laufey_cef_menu::SetApplicationMenu(window, view, wid,
                                                std::move(items), fn, d);
          },
          browser, window_id, std::move(entries), on_click, on_click_data));
}

// A context menu: Win32's TrackPopupMenu on Windows (native look and item
// icons), a CEF Views menu on Linux (there is no GtkWindow to anchor a GTK
// menu to).
static void ShowCefContextMenu(RuntimeLoader* loader, uint32_t window_id, int x,
                               int y,
                               std::vector<laufey_common::MenuEntry> entries,
                               laufey_menu_click_fn on_click,
                               void* on_click_data,
                               laufey_menu_closed_fn on_closed,
                               void* on_closed_data) {
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser || entries.empty()) {
    laufey_common::FireContextMenuClosedNow(window_id, on_closed,
                                            on_closed_data);
    return;
  }
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](CefRefPtr<CefBrowser> b, uint32_t wid, int cx, int cy,
                     std::vector<laufey_common::MenuEntry> items,
                     laufey_menu_click_fn fn, void* d,
                     laufey_menu_closed_fn closed, void* closed_data) {
#if defined(_WIN32)
                    // (cx, cy) is in window DIP, like every CEF geometry;
                    // the Win32 menu takes client pixels (the process is
                    // per-monitor DPI aware).
                    HWND hwnd = b->GetHost()->GetWindowHandle();
                    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
                    double scale = dpi ? dpi / 96.0 : 1.0;
                    win32_menu::ShowContextMenu(
                        hwnd, static_cast<int>(std::lround(cx * scale)),
                        static_cast<int>(std::lround(cy * scale)), items, fn, d,
                        wid, closed, closed_data);
#else
                    CefRefPtr<CefBrowserView> view =
                        CefBrowserView::GetForBrowser(b);
                    CefRefPtr<CefWindow> window =
                        view ? view->GetWindow() : nullptr;
                    laufey_cef_menu::ShowContextMenu(window, view, wid, cx, cy,
                                                     std::move(items), fn, d,
                                                     closed, closed_data);
#endif
                  },
                  browser, window_id, x, y, std::move(entries), on_click,
                  on_click_data, on_closed, on_closed_data));
}

static void Backend_ShowContextMenu(void* data, uint32_t window_id, int x,
                                    int y, laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  std::vector<laufey_common::MenuEntry> entries =
      laufey_common::ParseMenuTemplate(menu_template, &loader->GetBackendApi(),
                                       false);
#if defined(__linux__)
  // The Linux path always consumed the template.
  loader->GetBackendApi().value_free(menu_template);
#endif
  ShowCefContextMenu(loader, window_id, x, y, std::move(entries), on_click,
                     on_click_data, nullptr, nullptr);
}

static void Backend_ShowContextMenuEx(void* data, uint32_t window_id, int x,
                                      int y, laufey_value_t* menu_template,
                                      laufey_menu_click_fn on_click,
                                      void* on_click_data,
                                      laufey_menu_closed_fn on_closed,
                                      void* on_closed_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  std::vector<laufey_common::MenuEntry> entries =
      laufey_common::ParseMenuTemplate(menu_template, &loader->GetBackendApi(),
                                       false);
  if (menu_template)
    loader->GetBackendApi().value_free(menu_template);
  ShowCefContextMenu(loader, window_id, x, y, std::move(entries), on_click,
                     on_click_data, on_closed, on_closed_data);
}

static bool Backend_TestTriggerMenuAccelerator(void* /*data*/,
                                               uint32_t window_id,
                                               const char* accelerator) {
  bool fired = false;
  cef_invoke_sync([&] {
    fired = laufey_cef_menu::TestTriggerAccelerator(window_id, accelerator);
  });
  return fired;
}

static uint32_t Backend_MenuCapabilities(void* /*data*/) {
  uint32_t caps = LAUFEY_MENU_CAP_APP_MENU | LAUFEY_MENU_CAP_ACCELERATORS |
                  LAUFEY_MENU_CAP_CONTEXT_MENU | LAUFEY_MENU_CAP_CONTEXT_CLOSED;
#if defined(_WIN32)
  caps |= LAUFEY_MENU_CAP_ICONS;  // context menus (Win32)
#endif
  return caps;
}
#endif  // _WIN32 || __linux__

#if defined(__APPLE__)
// Defined in runtime_loader_mac.mm
extern void Backend_SetApplicationMenu_Mac(void* data, uint32_t window_id,
                                           laufey_value_t* menu_template,
                                           laufey_menu_click_fn on_click,
                                           void* on_click_data);
extern void Backend_ShowContextMenu_Mac(void* data, uint32_t window_id, int x,
                                        int y, laufey_value_t* menu_template,
                                        laufey_menu_click_fn on_click,
                                        void* on_click_data);
extern void Backend_ShowContextMenuEx_Mac(void* data, uint32_t window_id, int x,
                                          int y, laufey_value_t* menu_template,
                                          laufey_menu_click_fn on_click,
                                          void* on_click_data,
                                          laufey_menu_closed_fn on_closed,
                                          void* on_closed_data);
extern void Backend_SetDockBadge_Mac(void* data, const char* badge_or_null);
extern void Backend_BounceDock_Mac(void* data, int type);
extern void Backend_SetDockMenu_Mac(void* data, laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data);
extern void Backend_SetDockVisible_Mac(void* data, bool visible);
extern void Backend_SetDockReopenHandler_Mac(void* data,
                                             laufey_dock_reopen_fn handler,
                                             void* user_data);
extern void Backend_SetOpenUrlHandler_Mac(void* data,
                                          laufey_open_url_fn handler,
                                          void* user_data);
extern bool Backend_TestTriggerOpenUrl_Mac(void* data, const char* url);

extern uint32_t Backend_CreateTrayIcon_Mac(void* data);
extern void Backend_DestroyTrayIcon_Mac(void* data, uint32_t tray_id);
extern void Backend_SetTrayIcon_Mac(void* data, uint32_t tray_id,
                                    const void* png_bytes, size_t len);
extern void Backend_SetTrayTooltip_Mac(void* data, uint32_t tray_id,
                                       const char* tooltip_or_null);
extern void Backend_SetTrayMenu_Mac(void* data, uint32_t tray_id,
                                    laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data);
extern void Backend_SetTrayClickHandler_Mac(void* data, uint32_t tray_id,
                                            laufey_tray_click_fn handler,
                                            void* user_data);
extern void Backend_SetTrayDoubleClickHandler_Mac(void* data, uint32_t tray_id,
                                                  laufey_tray_click_fn handler,
                                                  void* user_data);
extern void Backend_SetTrayIconDark_Mac(void* data, uint32_t tray_id,
                                        const void* png_bytes, size_t len);
extern bool Backend_GetTrayIconBounds_Mac(void* data, uint32_t tray_id, int* x,
                                          int* y, int* width, int* height);
#elif defined(__linux__)
// Defined in runtime_loader_linux.cc
extern uint32_t Backend_CreateTrayIcon_Linux(void* data);
extern void Backend_DestroyTrayIcon_Linux(void* data, uint32_t tray_id);
extern void Backend_SetTrayIcon_Linux(void* data, uint32_t tray_id,
                                      const void* png_bytes, size_t len);
extern void Backend_SetTrayTooltip_Linux(void* data, uint32_t tray_id,
                                         const char* tooltip_or_null);
extern void Backend_SetTrayMenu_Linux(void* data, uint32_t tray_id,
                                      laufey_value_t* menu_template,
                                      laufey_menu_click_fn on_click,
                                      void* on_click_data);
extern void Backend_SetTrayClickHandler_Linux(void* data, uint32_t tray_id,
                                              laufey_tray_click_fn handler,
                                              void* user_data);
extern void Backend_SetTrayDoubleClickHandler_Linux(
    void* data, uint32_t tray_id, laufey_tray_click_fn handler,
    void* user_data);
extern void Backend_SetTrayIconDark_Linux(void* data, uint32_t tray_id,
                                          const void* png_bytes, size_t len);
#endif

// --- Notifications and permissions (every platform) ---
//
// Thin trampolines over backend-common's laufey_notifications.h, whose
// per-OS platform (UNUserNotificationCenter, toasts, D-Bus) does the work.

static uint32_t Backend_ShowNotification(void* data, laufey_value_t* options,
                                         laufey_notification_event_fn on_event,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  laufey_common::NotificationOptions opts =
      laufey_common::ParseNotificationOptions(options,
                                              &loader->GetBackendApi());
  return laufey_common::ShowNotification(opts, on_event, user_data);
}

static void Backend_CloseNotification(void* /*data*/,
                                      uint32_t notification_id) {
  laufey_common::CloseNotification(notification_id);
}

static uint32_t Backend_NotificationCapabilities(void* /*data*/) {
  return laufey_common::NotificationCapabilities();
}

static void Backend_SetNotificationResponseHandler(
    void* /*data*/, laufey_notification_response_fn handler, void* user_data) {
  laufey_common::SetNotificationResponseHandler(handler, user_data);
}

static void Backend_ListScheduledNotifications(void* /*data*/,
                                               laufey_notification_list_fn cb,
                                               void* user_data) {
  laufey_common::ListScheduledNotifications(cb, user_data);
}

static void Backend_CancelNotification(void* /*data*/, const char* tag) {
  laufey_common::CancelNotification(tag);
}

static bool Backend_TestNotificationRespond(void* /*data*/, const char* tag,
                                            const char* action_id) {
  return laufey_common::TestNotificationRespond(tag, action_id);
}

static void Backend_QueryPermission(void* /*data*/, int kind,
                                    laufey_permission_callback_fn cb,
                                    void* user_data) {
  laufey_common::QueryNotificationPermission(kind, cb, user_data);
}

static void Backend_RequestPermission(void* /*data*/, int kind,
                                      laufey_permission_callback_fn cb,
                                      void* user_data) {
  laufey_common::RequestNotificationPermission(kind, cb, user_data);
}

static bool Backend_TestDismissContextMenu(void* /*data*/) {
  return laufey_common::DismissOpenContextMenu();
}

// --- Dock / taskbar (Windows + Linux) ---
//
// The dock is a macOS concept; on Windows the analog is the taskbar button
// (per-window), and on Linux it's the WM urgency hint. Bounce maps cleanly:
//   - Windows: FlashWindowEx on every LAUFEY window's HWND.
//   - Linux:   X11 UrgencyHint on every LAUFEY window's X11 Window.
// Badge is implemented as a `"(N) " prefix on each window's title — the
// convention used by Slack/Discord/Telegram. If user code updates the title
// while a badge is active, the badge falls off (best-effort v1; proper
// Windows overlay icons and Linux libunity are future work). Menu, visible,
// and reopen have no clean analog.

#if !defined(__APPLE__)
#include <map>
#include <mutex>
#include <string>

#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

// Badge is implemented as a title prefix. Per window we remember the
// "original" title at the moment a badge is applied, so clearing can
// restore it. If user code updates the window title while a badge is
// active, the badge is visually lost — that's a best-effort v1
// limitation; calling set_dock_badge again re-applies it on top of the
// (now-stale) saved original.

static void Backend_SetDockBadge_TitlePrefix(void* data,
                                             const char* badge_or_null) {
  std::string badge =
      (badge_or_null && *badge_or_null) ? std::string(badge_or_null) : "";
#if defined(__linux__)
  // A dock that reads LauncherEntry shows the count instead of the prefix.
  if (laufey_common::SetLauncherEntryBadge(badge))
    badge.clear();
#endif
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  loader->ForEachBrowserWithId([&badge](uint32_t wid,
                                        CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](uint32_t wid, CefRefPtr<CefBrowser> b, std::string bg) {
                      auto bv = CefBrowserView::GetForBrowser(b);
                      if (!bv)
                        return;
                      auto win = bv->GetWindow();
                      if (!win)
                        return;
                      std::string current = win->GetTitle().ToString();
                      std::string next = laufey_common::ApplyTitlePrefixBadge(
                          wid, current, bg);
                      win->SetTitle(next);
                    },
                    wid, browser, badge));
  });
}
#endif  // !__APPLE__

#if defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

static void Backend_BounceDock_Win(void* data, int type) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->ForEachBrowser([type](CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, int t) {
                              HWND hwnd = b->GetHost()->GetWindowHandle();
                              if (!hwnd)
                                return;
                              FLASHWINFO fi = {sizeof(FLASHWINFO), hwnd, 0, 0,
                                               0};
                              if (t == LAUFEY_DOCK_BOUNCE_CRITICAL) {
                                fi.dwFlags = FLASHW_ALL | FLASHW_TIMER;
                                fi.uCount = 0;
                              } else {
                                fi.dwFlags = FLASHW_TIMERNOFG;
                                fi.uCount = 3;
                              }
                              FlashWindowEx(&fi);
                            },
                            browser, type));
  });
}

// --- Tray (Windows) ---
//
// Shell_NotifyIcon + a hidden top-level window that receives
// WM_TRAYICON (one per process). PNG → HICON via WIC.

// --- Tray (Windows) ---
//
// Thin trampolines over backend-common/src/tray_win.cc. CefPostTask
// marshals to the UI thread since the common impl is synchronous.

uint32_t Backend_CreateTrayIcon_Win(void* /*data*/) {
  // Allocate the id synchronously; do the Shell_NotifyIcon setup on the
  // UI thread so the hidden tray window is owned by the thread that
  // pumps messages for it.
  uint32_t tray_id = laufey_common::CreateTrayIconWin();
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](uint32_t tid) { laufey_common::FinalizeTrayIconWin(tid); },
                  tray_id));
  return tray_id;
}

void Backend_DestroyTrayIcon_Win(void* /*data*/, uint32_t tray_id) {
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](uint32_t tid) { laufey_common::DestroyTrayIconWin(tid); },
                  tray_id));
}

void Backend_SetTrayIcon_Win(void* /*data*/, uint32_t tray_id,
                             const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0)
    return;
  std::vector<BYTE> copy((const BYTE*)png_bytes, (const BYTE*)png_bytes + len);
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, std::vector<BYTE> b) {
                            laufey_common::SetTrayIconWin(tid, b.data(),
                                                          b.size());
                          },
                          tray_id, std::move(copy)));
}

void Backend_SetTrayIconDark_Win(void* /*data*/, uint32_t tray_id,
                                 const void* png_bytes, size_t len) {
  std::vector<BYTE> copy;
  if (png_bytes && len > 0) {
    copy.assign((const BYTE*)png_bytes, (const BYTE*)png_bytes + len);
  }
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, std::vector<BYTE> b) {
                            laufey_common::SetTrayIconDarkWin(
                                tid, b.empty() ? nullptr : b.data(), b.size());
                          },
                          tray_id, std::move(copy)));
}

bool Backend_GetTrayIconBounds_Win(void* /*data*/, uint32_t tray_id, int* x,
                                   int* y, int* width, int* height) {
  // Shell_NotifyIconGetRect touches the shell/message window owned by the UI
  // thread, so query synchronously there.
  bool ok = false;
  cef_invoke_sync([&] {
    ok = laufey_common::GetTrayIconBoundsWin(tray_id, x, y, width, height);
  });
  return ok;
}

void Backend_SetTrayDoubleClickHandler_Win(void* /*data*/, uint32_t tray_id,
                                           laufey_tray_click_fn handler,
                                           void* user_data) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, laufey_tray_click_fn h, void* d) {
                            laufey_common::SetTrayDoubleClickHandlerWin(tid, h,
                                                                        d);
                          },
                          tray_id, handler, user_data));
}

void Backend_SetTrayTooltip_Win(void* /*data*/, uint32_t tray_id,
                                const char* tooltip_or_null) {
  std::string tip = tooltip_or_null ? tooltip_or_null : std::string();
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, std::string t) {
                            laufey_common::SetTrayTooltipWin(
                                tid, t.empty() ? nullptr : t.c_str());
                          },
                          tray_id, std::move(tip)));
}

void Backend_SetTrayMenu_Win(void* data, uint32_t tray_id,
                             laufey_value_t* menu_template,
                             laufey_menu_click_fn on_click,
                             void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const laufey_backend_api_t* api = &loader->GetBackendApi();
  CefPostTask(
      TID_UI,
      base::BindOnce(
          [](uint32_t tid, laufey_value_t* tmpl, const laufey_backend_api_t* a,
             laufey_menu_click_fn cb, void* cb_data) {
            laufey_common::SetTrayMenuWin(tid, tmpl, a, cb, cb_data);
          },
          tray_id, menu_template, api, on_click, on_click_data));
}

void Backend_SetTrayClickHandler_Win(void* /*data*/, uint32_t tray_id,
                                     laufey_tray_click_fn handler,
                                     void* user_data) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, laufey_tray_click_fn h, void* d) {
                            laufey_common::SetTrayClickHandlerWin(tid, h, d);
                          },
                          tray_id, handler, user_data));
}

#elif defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

static void Backend_BounceDock_Linux(void* data, int /*type*/) {
  // X11 urgency hint is binary — there's no informational vs critical. Set
  // it on every LAUFEY window; WMs will surface this (taskbar flash, workspace
  // indicator, etc.).
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->ForEachBrowser([](CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](CefRefPtr<CefBrowser> b) {
#ifdef GDK_WINDOWING_X11
                      GdkDisplay* gdk_display = gdk_display_get_default();
                      if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
                        return;
                      Display* display = GDK_DISPLAY_XDISPLAY(gdk_display);
                      ::Window win = (::Window)b->GetHost()->GetWindowHandle();
                      if (!win)
                        return;
                      XWMHints* hints = XGetWMHints(display, win);
                      if (!hints)
                        hints = XAllocWMHints();
                      if (hints) {
                        hints->flags |= XUrgencyHint;
                        XSetWMHints(display, win, hints);
                        XFree(hints);
                        XFlush(display);
                      }
#else
                      (void)b;
#endif
                    },
                    browser));
  });
}
#endif

static void Backend_OpenDevTools(void* data, uint32_t window_id) {
  // DevTools are off for the whole process (LAUFEY_INSPECTABLE=0 /
  // "inspectable": false); see LaufeyApplyInspectable* in app.cc.
  if (!laufey_common::LaunchInspectable())
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) {
                              CefWindowInfo windowInfo;
#if defined(_WIN32)
                              windowInfo.SetAsPopup(nullptr, "DevTools");
#endif
                              b->GetHost()->ShowDevTools(windowInfo, nullptr,
                                                         CefBrowserSettings(),
                                                         CefPoint());
                            },
                            browser));
  }
}

// --- Global shortcuts, launch at login, DevTools (API >= 40) ---

static uint32_t Backend_SystemCapabilities(void* /*data*/) {
  uint32_t caps =
      laufey_common::ShortcutCapabilities() | LAUFEY_SYSTEM_CAP_DEVTOOLS;
  if (laufey_common::GetLaunchAtLogin() != LAUFEY_LOGIN_ITEM_NOT_SUPPORTED)
    caps |= LAUFEY_SYSTEM_CAP_LAUNCH_AT_LOGIN;
  return caps;
}

static void Backend_SetShortcutHandler(void* /*data*/,
                                       laufey_shortcut_fn handler,
                                       void* user_data) {
  laufey_common::SetShortcutHandler(handler, user_data);
}

static void Backend_RegisterShortcut(void* /*data*/, const char* accelerator,
                                     laufey_shortcut_result_fn callback,
                                     void* user_data) {
  laufey_common::RegisterShortcut(accelerator, callback, user_data);
}

static bool Backend_UnregisterShortcut(void* /*data*/,
                                       const char* accelerator) {
  return laufey_common::UnregisterShortcut(accelerator);
}

static void Backend_UnregisterAllShortcuts(void* /*data*/) {
  laufey_common::UnregisterAllShortcuts();
}

static char* Backend_ListShortcuts(void* /*data*/) {
  return laufey_common::ListShortcuts();
}

static char* Backend_PlatformFeatures(void* /*data*/) {
#if defined(__APPLE__)
  // macOS: the browser process runs with --use-mock-keychain (app.h), so
  // OSCrypt's key is derived from Chromium's fixed mock password: the same
  // on every install. Cookies on disk are obfuscated, not protected by the
  // Keychain.
  laufey_common::SetCookieEncryption("basic");
#elif !defined(__linux__)
  // Windows (DPAPI): OSCrypt's key is the OS's.
  laufey_common::SetCookieEncryption("os");
#endif
  // Linux: main_linux.cc records the --password-store it chose.
  return laufey_common::PlatformFeaturesJsonForAbi();
}

static char* Backend_TrayUnavailableReason(void* /*data*/) {
  return laufey_common::TrayUnavailableReasonForAbi();
}

// Fires on the CEF UI thread (it runs the default GLib main context, where
// the probe's NameOwnerChanged subscription delivers).
static void Backend_SetPlatformFeaturesChangedHandler(
    void* /*data*/, laufey_platform_features_changed_fn handler,
    void* user_data) {
  laufey_common::SetPlatformFeaturesChangedHandler(handler, user_data);
}

// Title bar preferences (API 47). The change handler fires on the
// watcher's own thread (Linux, Windows) or the main thread (macOS).
static char* Backend_TitleBarPreferences(void* /*data*/) {
  return laufey_common::TitleBarPreferencesJsonForAbi();
}

static void Backend_SetTitleBarPreferencesChangedHandler(
    void* /*data*/, laufey_title_bar_preferences_changed_fn handler,
    void* user_data) {
  laufey_common::SetTitleBarPreferencesChangedHandler(handler, user_data);
}

static char* Backend_CanonicalizeAccelerator(void* /*data*/,
                                             const char* accelerator) {
  return laufey_common::CanonicalizeAccelerator(accelerator);
}

static bool Backend_TestTriggerShortcut(void* /*data*/,
                                        const char* accelerator) {
  return laufey_common::TestTriggerShortcut(accelerator);
}

static int Backend_GetLaunchAtLogin(void* /*data*/) {
  return laufey_common::GetLaunchAtLogin();
}

static int Backend_SetLaunchAtLogin(void* /*data*/, bool enabled,
                                    char** error_out) {
  if (error_out)
    *error_out = nullptr;
  std::string error;
  int state = laufey_common::SetLaunchAtLogin(enabled, &error);
  if (state == LAUFEY_LOGIN_ITEM_FAILED && error_out && !error.empty()) {
    char* copy = static_cast<char*>(malloc(error.size() + 1));
    if (copy) {
      memcpy(copy, error.c_str(), error.size() + 1);
      *error_out = copy;
    }
  }
  return state;
}

static void Backend_CloseDevTools(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser)
    return;
  CefPostTask(TID_UI, base::BindOnce(
                          [](CefRefPtr<CefBrowser> b) {
                            b->GetHost()->CloseDevTools();
                          },
                          browser));
}

static bool Backend_IsDevToolsOpen(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser)
    return false;
  bool open = false;
  cef_invoke_sync([&] { open = browser->GetHost()->HasDevTools(); });
  return open;
}

static bool Backend_IsDevToolsEnabled(void* data, uint32_t window_id) {
  if (window_id == 0)
    return laufey_common::LaunchInspectable();
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser)
    return false;
  bool enabled = false;
  // Read back from CEF: the launch setting, unless a remote-debugging switch
  // made it onto the browser process's command line anyway.
  cef_invoke_sync([&] { enabled = LaufeyDevToolsReachable(); });
  return enabled;
}

static void Backend_SetJsNamespace(void* data, const char* name) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (name) {
    loader->SetJsNamespace(name);
  }
}

namespace {

// Observes the DevTools "Page.printToPDF" result for a single print request.
// The PDF is returned in-memory as base64 in the result JSON (no temp file);
// we decode it and hand the raw bytes to the laufey pdf-result callback (the
// capi layer owns any file output). The observer keeps itself alive via its
// own CefRegistration until the matching result arrives or the DevTools agent
// detaches.
class LaufeyPdfDevToolsObserver : public CefDevToolsMessageObserver {
 public:
  LaufeyPdfDevToolsObserver(int message_id, laufey_pdf_result_fn callback,
                            void* callback_data)
      : message_id_(message_id),
        callback_(callback),
        callback_data_(callback_data) {}

  // Called right after AddDevToolsMessageObserver so the observer can release
  // its own registration once it is done (which stops observing and drops the
  // last reference).
  void SetRegistration(CefRefPtr<CefRegistration> registration) {
    registration_ = registration;
  }

  void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int message_id,
                              bool success, const void* result,
                              size_t result_size) override {
    if (message_id != message_id_)
      return;
    if (!success) {
      Finish(nullptr, 0, "Page.printToPDF failed");
      return;
    }

    // `result` is the UTF-8 JSON of the method result: { "data": "<base64>" }.
    std::string json(static_cast<const char*>(result), result_size);
    CefRefPtr<CefValue> value =
        CefParseJSON(CefString(json), JSON_PARSER_ALLOW_TRAILING_COMMAS);
    if (!value || value->GetType() != VTYPE_DICTIONARY) {
      Finish(nullptr, 0, "invalid Page.printToPDF result");
      return;
    }
    CefRefPtr<CefDictionaryValue> dict = value->GetDictionary();
    if (!dict || !dict->HasKey("data")) {
      Finish(nullptr, 0, "Page.printToPDF result missing data");
      return;
    }
    CefRefPtr<CefBinaryValue> bin = CefBase64Decode(dict->GetString("data"));
    if (!bin) {
      Finish(nullptr, 0, "failed to decode PDF data");
      return;
    }

    size_t len = bin->GetSize();
    std::vector<uint8_t> bytes(len);
    if (len > 0)
      bin->GetData(bytes.data(), len, 0);

    Finish(bytes.empty() ? nullptr : bytes.data(), bytes.size(), nullptr);
  }

  // If the browser is destroyed while the request is in flight, CEF never
  // delivers the pending method result ("pending method results will not be
  // delivered", cef_devtools_message_observer.h). Without this override the
  // observer would keep itself alive through its own registration forever and
  // the callback would never fire, breaking the exactly-once contract.
  void OnDevToolsAgentDetached(CefRefPtr<CefBrowser> browser) override {
    Finish(nullptr, 0, "browser was closed before the PDF result arrived");
  }

 private:
  void Finish(const uint8_t* data, size_t len, const char* error) {
    if (done_)
      return;
    done_ = true;
    callback_(data, len, error, callback_data_);
    // Releasing the registration drops observation and may destroy `this`;
    // CEF holds a reference across this call, so doing it last is safe.
    registration_ = nullptr;
  }

  int message_id_;
  laufey_pdf_result_fn callback_;
  void* callback_data_;
  bool done_ = false;
  CefRefPtr<CefRegistration> registration_;

  IMPLEMENT_REFCOUNTING(LaufeyPdfDevToolsObserver);
};

}  // namespace

static void Backend_PrintToPdf(void* data, uint32_t window_id,
                               laufey_pdf_result_fn callback,
                               void* callback_data) {
  if (!callback)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  // Unique, positive DevTools message id per request so concurrent PDF
  // requests don't observe each other's results.
  static std::atomic<int> next_message_id{1};
  int message_id = next_message_id.fetch_add(1);

  // Everything — including the window lookup, whose "window not found" the
  // capi contract requires on the UI thread — runs in the posted task. This
  // also closes the race where a freshly created window's browser has not yet
  // been recorded (browsers_ is populated from a posted task itself).
  bool posted = CefPostTask(
      TID_UI,
      base::BindOnce(
          [](RuntimeLoader* loader, uint32_t win_id, int msg_id,
             laufey_pdf_result_fn cb, void* cb_data) {
            CefRefPtr<CefBrowser> b = loader->GetBrowserForWindow(win_id);
            if (!b) {
              cb(nullptr, 0, "window not found", cb_data);
              return;
            }
            CefRefPtr<LaufeyPdfDevToolsObserver> observer =
                new LaufeyPdfDevToolsObserver(msg_id, cb, cb_data);
            CefRefPtr<CefRegistration> registration =
                b->GetHost()->AddDevToolsMessageObserver(observer);
            if (!registration) {
              cb(nullptr, 0, "failed to attach DevTools observer", cb_data);
              return;
            }
            observer->SetRegistration(registration);

            // Empty params: default ReturnAsBase64 transfer mode. Returns the
            // PDF bytes inline in the result JSON.
            CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
            int sent = b->GetHost()->ExecuteDevToolsMethod(
                msg_id, "Page.printToPDF", params);
            if (sent <= 0) {
              // Result will never arrive; report and drop the observer.
              cb(nullptr, 0, "failed to invoke Page.printToPDF", cb_data);
              observer->SetRegistration(nullptr);
            }
          },
          loader, window_id, message_id, callback, callback_data));
  if (!posted) {
    // The UI task runner is gone (shutdown): the task was destroyed unrun, so
    // deliver the mandatory exactly-once callback here instead of never.
    callback(nullptr, 0, "failed to schedule print on the UI thread",
             callback_data);
  }
}

// --- InitializeBackendApi ---

static uint32_t Backend_CreateWindowImpl(void* data, uint32_t flags) {
  auto* loader = RuntimeLoader::GetInstance();
  uint32_t window_id = loader->AllocateWindowId();

  bool posted = CefPostTask(
      TID_UI, base::BindOnce(
                  [](uint32_t wid, uint32_t window_flags) {
                    auto* handler = LaufeyHandler::GetInstance();
                    if (!handler)
                      return;

                    // Push laufey_id before creating the browser so
                    // OnAfterCreated can pop it. Both run on the UI
                    // thread so no race.
                    g_pending_laufey_ids.push(wid);

                    CefBrowserSettings browser_settings;
                    CefRefPtr<CefDictionaryValue> extra_info =
                        CefDictionaryValue::Create();
                    extra_info->SetString(
                        "laufey_js_namespace",
                        RuntimeLoader::GetInstance()->GetJsNamespace());
                    CefRefPtr<CefBrowserView> browser_view =
                        CefBrowserView::CreateBrowserView(
                            handler, "about:blank", browser_settings,
                            extra_info, nullptr, nullptr);
                    CefWindow::CreateTopLevelWindow(new LaufeyWindowDelegate(
                        browser_view, wid, window_flags));
                  },
                  window_id, flags));

  // Block until the browser is registered by OnAfterCreated, so that
  // subsequent calls (navigate, set_title, etc.) can find it: until then they
  // are dropped. The wait returns as soon as the browser exists; its bound
  // only guards against a creation that never completes. A cold start on a
  // slow machine takes several seconds per browser (over 5 s, the old bound,
  // on a Windows CI runner, which dropped the window's first navigation).
  // (Nothing to wait for when CEF is no longer taking UI tasks.)
  constexpr int kBrowserCreateTimeoutMs = 30000;
  if (posted && !loader->WaitForBrowser(window_id, kBrowserCreateTimeoutMs)) {
    std::cerr << "laufey: the browser for window " << window_id
              << " was not created within " << kBrowserCreateTimeoutMs / 1000
              << " s; calls on the window are ignored until it is" << std::endl;
  }

  return window_id;
}

static uint32_t Backend_CreateWindow(void* data) {
  return Backend_CreateWindowImpl(data, 0);
}

static uint32_t Backend_CreateWindowEx(void* data, uint32_t flags) {
  return Backend_CreateWindowImpl(data, flags);
}

static void Backend_CloseWindow(void* data, uint32_t window_id) {
  auto* loader = RuntimeLoader::GetInstance();
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    // Mark before closing: CloseBrowser eventually drives the CefWindow's
    // close, whose CanClose must skip the close-requested negotiation for a
    // programmatic close (force_close=true only skips the beforeunload
    // prompt, not CanClose -- without the mark a registered handler would
    // re-defer this close forever).
    loader->MarkCloseAllowed(window_id);
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](CefRefPtr<CefBrowser> b, uint32_t id) {
#if defined(__APPLE__)
                      // An auth session / passkey sheet attached to
                      // the window keeps it from closing: end it
                      // first (`cancelled`), as WKWebView does.
                      if (void* nswindow = RuntimeLoader::GetInstance()
                                               ->GetNSWindowForLaufeyId(id)) {
                        laufey_common::PasskeyWindowClosing(nswindow);
                        laufey_common::AuthSessionWindowClosing(nswindow);
                      }
#else
                      (void)id;
#endif
                      b->GetHost()->CloseBrowser(true);
                    },
                    browser, window_id));
  }
}

static void Backend_SetCloseRequestedHandler(void* data,
                                             laufey_close_requested_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCloseRequestedHandler(handler, user_data);
}

// Test hook (API >= 31): synthesize a close-requested event through the same
// dispatch code a real OS close click runs (see CanClose in app.cc). Returns
// true if a registered handler deferred the close; false means the close was
// initiated (it completes asynchronously via CloseBrowser).
static bool Backend_TestTriggerCloseRequested(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  bool proceed = loader->DispatchCloseRequestedEvent(window_id);
  if (proceed) {
    Backend_CloseWindow(data, window_id);
    return false;
  }
  return true;
}

static int Backend_ShowDialog(void* /*data*/, uint32_t /*window_id*/,
                              int dialog_type, const char* title,
                              const char* message, const char* default_value,
                              char** out_input_value) {
  std::string title_str = title ? title : "";
  std::string message_str = message ? message : "";
  std::string default_str = default_value ? default_value : "";
#ifdef __APPLE__
  return laufey_common::ShowDialogMac(dialog_type, title_str, message_str,
                                      default_str, out_input_value);
#elif defined(__linux__)
  // GTK belongs to TID_UI: run the modal there (its nested loop keeps CEF's
  // tasks running, see ShowDialogLinux) and wait for it.
  int result = 0;
  cef_invoke_sync([&] {
    result = laufey_common::ShowDialogLinux(dialog_type, title_str, message_str,
                                            default_str, out_input_value);
  });
  return result;
#elif defined(_WIN32)
  return laufey_common::ShowDialogWin(dialog_type, title_str, message_str,
                                      default_str, out_input_value);
#else
  (void)out_input_value;
  return 0;
#endif
}

static void Backend_StringFree(void* /*data*/, char* s) {
  if (s)
    free(s);
}

static char* Backend_ReadClipboardText(void* /*data*/) {
#ifdef __APPLE__
  return laufey_common::ClipboardReadTextMac();
#elif defined(__linux__)
  laufey_common::GtkRunSync([] { CefEnsureGtkInit(); });
  return laufey_common::ClipboardReadTextLinux();
#elif defined(_WIN32)
  return laufey_common::ClipboardReadTextWin();
#else
  return nullptr;
#endif
}

static void Backend_WriteClipboardText(void* /*data*/, const char* text) {
  std::string text_str = text ? text : "";
#ifdef __APPLE__
  laufey_common::ClipboardWriteTextMac(text_str);
#elif defined(__linux__)
  laufey_common::GtkRunSync([] { CefEnsureGtkInit(); });
  laufey_common::ClipboardWriteTextLinux(text_str);
#elif defined(_WIN32)
  laufey_common::ClipboardWriteTextWin(text_str);
#else
  (void)text_str;
#endif
}

// --- Drag and drop, file dialogs, rich clipboard (API >= 39) ----------------
//
// Drops: CefDragHandler::OnDragEnter hands the browser process the dragged
// files' paths (LaufeyHandler keeps them per browser); a closure-private
// observer injected into the main frame (render_process_handler.cc) reports
// where the drag is and when it drops, and LaufeyHandler dispatches each
// phase with those paths. Dialogs, drag-out and the clipboard are the OS's
// own, shared with the WebView backends (backend-common): the same dialogs
// on every backend, not CEF's RunFileDialog.

static CefRefPtr<CefWindow> CefWindowForId(uint32_t window_id);

#ifdef __linux__
// GTK must be initialized before the Linux clipboard / dialogs / drag source
// touch it (runtime_loader_linux.cc); do it on the GTK (CEF UI) thread.
static void EnsureGtkReady() {
  laufey_common::GtkRunSync([] { CefEnsureGtkInit(); });
}
#endif

static void Backend_SetFileDropHandler(void* /*data*/,
                                       laufey_file_drop_fn handler,
                                       void* user_data) {
  laufey_common::SetFileDropHandler(handler, user_data);
}

static bool Backend_TestTriggerFileDrop(void* /*data*/, uint32_t window_id,
                                        int phase, double x, double y,
                                        const char* const* paths,
                                        size_t count) {
  // Real drops are dispatched on the UI thread (LaufeyHandler); so is this.
  bool delivered = false;
  cef_invoke_sync([&] {
    delivered = laufey_common::TestTriggerFileDrop(window_id, phase, x, y,
                                                   paths, count);
  });
  return delivered;
}

static void Backend_StartFileDrag(void* /*data*/, uint32_t window_id,
                                  const char* const* paths, size_t count,
                                  const uint8_t* icon_png, size_t icon_len,
                                  laufey_drag_result_fn callback,
                                  void* user_data) {
  auto* req = new laufey_common::DragOutRequest();
  req->callback = callback;
  req->user_data = user_data;
  if (!RuntimeLoader::GetInstance()->GetBrowserForWindow(window_id) ||
      !laufey_common::ValidateDragPaths(paths, count, &req->paths)) {
    req->Finish(LAUFEY_DRAG_RESULT_FAILED);
    delete req;
    return;
  }
  if (icon_png && icon_len > 0)
    req->icon_png.assign(icon_png, icon_png + icon_len);
#if defined(__APPLE__)
  // The window's content view is the drag source; resolved on the UI (main)
  // thread, where StartFileDragMac runs anyway.
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t wid, laufey_common::DragOutRequest* r) {
                            CefRefPtr<CefWindow> window = CefWindowForId(wid);
                            if (!window) {
                              r->Finish(LAUFEY_DRAG_RESULT_FAILED);
                              delete r;
                              return;
                            }
                            laufey_common::StartFileDragMac(
                                window->GetWindowHandle(), r);
                          },
                          window_id, req));
#elif defined(_WIN32)
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t wid, laufey_common::DragOutRequest* r) {
                            CefRefPtr<CefWindow> window = CefWindowForId(wid);
                            if (!window) {
                              r->Finish(LAUFEY_DRAG_RESULT_FAILED);
                              delete r;
                              return;
                            }
                            laufey_common::StartFileDragWin(
                                window->GetWindowHandle(), r);
                          },
                          window_id, req));
#else
  // Chromium's windows are not GTK's; GTK drags from its own invisible
  // source widget, which needs an X11 display (see the capability).
  EnsureGtkReady();
  laufey_common::StartFileDragLinux(nullptr, req);
#endif
}

#if defined(__APPLE__) || defined(_WIN32)
// The dialog's owner: the CefWindow's NSWindow / HWND, looked up on the UI
// thread right before the dialog shows.
static laufey_common::ParentResolver CefParentResolver(uint32_t window_id) {
  if (window_id == 0)
    return nullptr;
  return [window_id]() -> void* {
    CefRefPtr<CefWindow> window = CefWindowForId(window_id);
    if (!window)
      return nullptr;
#if defined(__APPLE__)
    return NSWindowForCefHandle(window->GetWindowHandle());
#else
    return window->GetWindowHandle();
#endif
  };
}
#endif

static uint32_t Backend_ShowFileDialog(
    void* /*data*/, uint32_t window_id,
    const laufey_file_dialog_options_t* options,
    laufey_file_dialog_result_fn callback, void* user_data) {
  if (!callback)
    return 0;
#if defined(__APPLE__)
  return laufey_common::ShowFileDialogMac(CefParentResolver(window_id), options,
                                          callback, user_data);
#elif defined(_WIN32)
  return laufey_common::ShowFileDialogWin(CefParentResolver(window_id), options,
                                          callback, user_data);
#else
  // GtkFileChooserNative needs a GtkWindow to be modal to; Chromium's X11 /
  // Wayland windows aren't GTK's, so GTK's chooser is app-level here. The
  // portal's FileChooser takes the window's X11 id ("x11:<xid>") on X11;
  // a Chromium Wayland toplevel can't be named to it (no xdg-foreign
  // export), so there the portal's dialog is app-level too.
  EnsureGtkReady();
  laufey_common::PortalParentResolver portal_parent;
  if (window_id != 0 && laufey_common::DisplayBackend() != "wayland") {
    portal_parent = [window_id]() -> std::string {
      CefRefPtr<CefWindow> window = CefWindowForId(window_id);
      unsigned long xid = window ? window->GetWindowHandle() : 0;
      if (!xid)
        return "";
      char buf[32];
      snprintf(buf, sizeof(buf), "x11:%lx", xid);
      return buf;
    };
  }
  return laufey_common::ShowFileDialogLinux(nullptr, options, callback,
                                            user_data, portal_parent);
#endif
}

static bool Backend_CancelFileDialog(void* /*data*/, uint32_t dialog_id) {
#if defined(__APPLE__)
  return laufey_common::CancelFileDialogMac(dialog_id);
#elif defined(_WIN32)
  return laufey_common::CancelFileDialogWin(dialog_id);
#else
  return laufey_common::CancelFileDialogLinux(dialog_id);
#endif
}

static bool Backend_TestFileDialogRespond(void* /*data*/, int action,
                                          const char* path) {
#if defined(__APPLE__)
  return laufey_common::TestFileDialogRespondMac(action, path);
#elif defined(_WIN32)
  return laufey_common::TestFileDialogRespondWin(action, path);
#else
  return laufey_common::TestFileDialogRespondLinux(action, path);
#endif
}

static uint32_t Backend_ClipboardCapabilities(void* /*data*/) {
#if defined(__APPLE__)
  return laufey_common::ClipboardCapabilitiesMac();
#elif defined(_WIN32)
  return laufey_common::ClipboardCapabilitiesWin();
#else
  EnsureGtkReady();
  return laufey_common::ClipboardCapabilitiesLinux();
#endif
}

static char* Backend_ReadClipboardHtml(void* /*data*/) {
#if defined(__APPLE__)
  return laufey_common::ClipboardReadHtmlMac();
#elif defined(_WIN32)
  return laufey_common::ClipboardReadHtmlWin();
#else
  EnsureGtkReady();
  return laufey_common::ClipboardReadHtmlLinux();
#endif
}

static bool Backend_WriteClipboardHtml(void* /*data*/, const char* html,
                                       const char* text_or_null) {
  if (!html)
    return false;
#if defined(__APPLE__)
  return laufey_common::ClipboardWriteHtmlMac(html, text_or_null);
#elif defined(_WIN32)
  return laufey_common::ClipboardWriteHtmlWin(html, text_or_null);
#else
  EnsureGtkReady();
  return laufey_common::ClipboardWriteHtmlLinux(html, text_or_null);
#endif
}

static uint8_t* Backend_ReadClipboardImage(void* /*data*/, size_t* len_out) {
#if defined(__APPLE__)
  return laufey_common::ClipboardReadImageMac(len_out);
#elif defined(_WIN32)
  return laufey_common::ClipboardReadImageWin(len_out);
#else
  EnsureGtkReady();
  return laufey_common::ClipboardReadImageLinux(len_out);
#endif
}

static bool Backend_WriteClipboardImage(void* /*data*/, const uint8_t* png,
                                        size_t len) {
  if (!png || len == 0)
    return false;
#if defined(__APPLE__)
  return laufey_common::ClipboardWriteImageMac(png, len);
#elif defined(_WIN32)
  return laufey_common::ClipboardWriteImageWin(png, len);
#else
  EnsureGtkReady();
  return laufey_common::ClipboardWriteImageLinux(png, len);
#endif
}

static char* Backend_ReadClipboardFormats(void* /*data*/) {
#if defined(__APPLE__)
  return laufey_common::ClipboardReadFormatsMac();
#elif defined(_WIN32)
  return laufey_common::ClipboardReadFormatsWin();
#else
  EnsureGtkReady();
  return laufey_common::ClipboardReadFormatsLinux();
#endif
}

static void Backend_SetClipboardChangeHandler(void* /*data*/,
                                              laufey_clipboard_change_fn fn,
                                              void* user_data) {
#ifdef __linux__
  EnsureGtkReady();
#endif
  laufey_common::SetClipboardChangeHandler(fn, user_data);
}

static void Backend_BufferFree(void* /*data*/, void* buffer) {
  free(buffer);
}

// Test hook (API >= 30): synthesize a click on a menu/tray item by id. Platform
// independent — the shared registry in backend-common holds the handlers.
static bool Backend_TestClickMenuItem(void* /*data*/, const char* item_id) {
  return laufey_common::TestClickMenuItem(item_id);
}

static void InjectKey(void* ctx, uint32_t window_id, int state, const char* key,
                      const char* code, uint32_t modifiers, bool repeat) {
  static_cast<RuntimeLoader*>(ctx)->DispatchKeyboardEvent(
      window_id, state, key, code, modifiers, repeat);
}
static void InjectClick(void* ctx, uint32_t window_id, int state, int button,
                        double x, double y, uint32_t modifiers,
                        int32_t click_count) {
  static_cast<RuntimeLoader*>(ctx)->DispatchMouseClickEvent(
      window_id, state, button, x, y, modifiers, click_count);
}
static void InjectMove(void* ctx, uint32_t window_id, double x, double y,
                       uint32_t modifiers) {
  static_cast<RuntimeLoader*>(ctx)->DispatchMouseMoveEvent(window_id, x, y,
                                                           modifiers);
}
static void InjectWheel(void* ctx, uint32_t window_id, double delta_x,
                        double delta_y, double x, double y, uint32_t modifiers,
                        int32_t delta_mode) {
  static_cast<RuntimeLoader*>(ctx)->DispatchWheelEvent(
      window_id, delta_x, delta_y, x, y, modifiers, delta_mode);
}
static void InjectEnterLeave(void* ctx, uint32_t window_id, int entered,
                             double x, double y, uint32_t modifiers) {
  static_cast<RuntimeLoader*>(ctx)->DispatchCursorEnterLeaveEvent(
      window_id, entered, x, y, modifiers);
}

static bool Backend_TestInjectInput(void* data, uint32_t window_id,
                                    const laufey_test_input_t* event) {
  laufey_common::TestInjectSink sink = {
      InjectKey, InjectClick, InjectMove, InjectWheel, InjectEnterLeave, data,
  };
  return laufey_common::TestInjectInput(window_id, event, sink);
}

// --- Window state, constraints, screens and chrome (API >= 38) ---
//
// Cross-platform through the CEF Views API (CefWindow / CefDisplay), in its
// units: DIP, CefWindow::GetPosition / GetSize. The OS notices state changes
// first; each platform hook (NSWindow notifications on macOS, a WM_SIZE
// subclass on Windows, the window delegate's bounds / activation /
// fullscreen callbacks everywhere) schedules CefRecheckWindowState, which
// reads the state back from CefWindow and reports it; duplicates are dropped
// in laufey_common::ReportWindowState.

// windowsx.h defines IsMaximized / IsMinimized as function-like macros
// (IsZoomed / IsIconic); `(window->IsMaximized)()` keeps them from expanding.
static CefRefPtr<CefWindow> CefWindowForId(uint32_t window_id) {
  CefRefPtr<CefBrowser> browser =
      RuntimeLoader::GetInstance()->GetBrowserForWindow(window_id);
  if (!browser)
    return nullptr;
  auto browser_view = CefBrowserView::GetForBrowser(browser);
  return browser_view ? browser_view->GetWindow() : nullptr;
}

static uint32_t CefStateOf(CefRefPtr<CefWindow> window) {
  uint32_t state = 0;
  if (window->IsFullscreen())
    state |= LAUFEY_WINDOW_STATE_FULLSCREEN;
  else if ((window->IsMaximized)())
    state |= LAUFEY_WINDOW_STATE_MAXIMIZED;
  if ((window->IsMinimized)())
    state |= LAUFEY_WINDOW_STATE_MINIMIZED;
  return state;
}

// Normal bounds: the frame's position and the page size (get_window_size).
static laufey_common::Bounds CefBoundsOf(CefRefPtr<CefWindow> window,
                                         uint32_t window_id) {
  laufey_common::Bounds b;
  CefPoint pos = window->GetPosition();
  CefSize size = window->GetSize();
  CefRefPtr<CefBrowser> browser =
      RuntimeLoader::GetInstance()->GetBrowserForWindow(window_id);
  auto browser_view =
      browser ? CefBrowserView::GetForBrowser(browser) : nullptr;
  CefSize frame = browser_view ? CefFrameAroundPage(browser_view) : CefSize();
  b.x = pos.x;
  b.y = pos.y;
  b.width = (std::max)(0, size.width - frame.width);
  b.height = (std::max)(0, size.height - frame.height);
  return b;
}

// UI thread.
void CefRecheckWindowState(uint32_t window_id) {
  CefRefPtr<CefWindow> window = CefWindowForId(window_id);
  if (!window || window->IsClosed())
    return;
  uint32_t state = CefStateOf(window);
  int64_t now = laufey_common::MonotonicMs();
  if (state != 0 && laufey_common::LastReportedWindowState(window_id) == 0)
    laufey_common::NoteWindowLeftNormal(window_id, now);
  laufey_common::NoteWindowGeometry(window_id, CefBoundsOf(window, window_id),
                                    state == 0, now);
  laufey_common::ReportWindowState(window_id, state);
}

// Any thread: a recheck now and a few more while the OS animates the change
// (Linux window managers apply state asynchronously; macOS animates
// zoom / fullscreen).
void CefScheduleWindowStateRecheck(uint32_t window_id) {
  auto task = [](uint32_t wid) { CefRecheckWindowState(wid); };
  CefPostTask(TID_UI, base::BindOnce(task, window_id));
  for (int64_t delay : {150, 600, 1500}) {
    CefPostDelayedTask(TID_UI, base::BindOnce(task, window_id), delay);
  }
}

static uint32_t Backend_WindowCapabilities(void* /*data*/) {
  uint32_t caps = LAUFEY_WINDOW_CAP_STATE | LAUFEY_WINDOW_CAP_STATE_EVENTS |
                  LAUFEY_WINDOW_CAP_SIZE_CONSTRAINTS |
                  LAUFEY_WINDOW_CAP_SCREENS | LAUFEY_WINDOW_CAP_DISPLAY_EVENTS |
                  LAUFEY_WINDOW_CAP_NORMAL_BOUNDS |
                  LAUFEY_WINDOW_CAP_KEEP_ALIVE;
#if defined(__APPLE__)
  // Titled NSWindow chrome is AppKit's; vibrancy is not available: the CEF
  // browser view paints an opaque background in windowed mode.
  caps |= LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN |
          LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN_INSET |
          LAUFEY_WINDOW_CAP_TRAFFIC_LIGHT_POSITION |
          LAUFEY_WINDOW_CAP_SET_POSITION;
#elif defined(_WIN32)
  // No Mica / Acrylic: the CEF browser view paints an opaque background in
  // windowed mode, so a DWM backdrop could never show through the page.
  caps |= LAUFEY_WINDOW_CAP_SET_POSITION;
#else
  // Ozone/Wayland (chosen when a Wayland display is there, see
  // main_linux.cc) can't place windows.
  if (laufey_common::DisplayBackend() != "wayland")
    caps |= LAUFEY_WINDOW_CAP_SET_POSITION;
#endif
  // Drag and drop and file dialogs (API >= 39). The paths of an external
  // drop come from the OS's drag data when the drag enters (CEF calls
  // OnDragEnter only for Alloy-style browsers; see LaufeyNativeDragFilePaths).
  // macOS sheets and Windows owner windows are modal; on Linux the GTK dialog
  // can't be made modal to Chromium's (non-GTK) window.
  caps |= LAUFEY_WINDOW_CAP_FILE_DIALOGS;
#if defined(__APPLE__)
  caps |= LAUFEY_WINDOW_CAP_FILE_DROP |
          LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS |
          LAUFEY_WINDOW_CAP_FILE_DRAG_OUT |
          LAUFEY_WINDOW_CAP_FILE_DIALOG_FILES_AND_DIRECTORIES |
          LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL;
#elif defined(_WIN32)
  caps |= LAUFEY_WINDOW_CAP_FILE_DROP |
          LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS |
          LAUFEY_WINDOW_CAP_FILE_DRAG_OUT | LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL;
#else
  // Linux: the drop's paths are read from X11's XdndSelection and the GTK
  // drag source needs an X11 display. Under Wayland there is no X drag data
  // to read, so a drop could never carry its paths: neither is reported.
  EnsureGtkReady();
  bool x11 = false;
  laufey_common::GtkRunSync([&] {
    GdkDisplay* display = gdk_display_get_default();
    x11 = display && strstr(G_OBJECT_TYPE_NAME(display), "X11") != nullptr;
  });
  if (x11) {
    caps |= LAUFEY_WINDOW_CAP_FILE_DROP |
            LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS |
            LAUFEY_WINDOW_CAP_FILE_DRAG_OUT;
  }
#endif
  return caps;
}

static void Backend_SetWindowState(void* /*data*/, uint32_t window_id,
                                   int action) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t wid, int act) {
                            CefRefPtr<CefWindow> window = CefWindowForId(wid);
                            if (!window)
                              return;
                            switch (act) {
                              case LAUFEY_WINDOW_ACTION_MAXIMIZE:
                                if (!window->IsFullscreen())
                                  window->Maximize();
                                break;
                              case LAUFEY_WINDOW_ACTION_UNMAXIMIZE:
                                if ((window->IsMaximized)())
                                  window->Restore();
                                break;
                              case LAUFEY_WINDOW_ACTION_MINIMIZE:
                                window->Minimize();
                                break;
                              case LAUFEY_WINDOW_ACTION_RESTORE:
                                if ((window->IsMinimized)())
                                  window->Restore();
                                break;
                              case LAUFEY_WINDOW_ACTION_ENTER_FULLSCREEN:
                                window->SetFullscreen(true);
                                break;
                              case LAUFEY_WINDOW_ACTION_LEAVE_FULLSCREEN:
                                window->SetFullscreen(false);
                                break;
                              default:
                                return;
                            }
                            CefScheduleWindowStateRecheck(wid);
                          },
                          window_id, action));
}

static uint32_t Backend_GetWindowState(void* /*data*/, uint32_t window_id) {
  uint32_t state = 0;
  cef_invoke_sync([&] {
    if (CefRefPtr<CefWindow> window = CefWindowForId(window_id))
      state = CefStateOf(window);
  });
  return state;
}

static void Backend_SetWindowStateHandler(void* /*data*/,
                                          laufey_window_state_fn handler,
                                          void* user_data) {
  laufey_common::SetWindowStateHandler(handler, user_data);
}

static void Backend_SetWindowSizeConstraints(void* /*data*/, uint32_t window_id,
                                             int min_width, int min_height,
                                             int max_width, int max_height) {
  laufey_common::SizeConstraints c = laufey_common::SetSizeConstraints(
      window_id, min_width, min_height, max_width, max_height);
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](uint32_t wid, laufey_common::SizeConstraints c) {
                    CefRefPtr<CefWindow> window = CefWindowForId(wid);
                    if (!window)
                      return;
#if defined(__APPLE__)
                    // LaufeyWindowDelegate::GetMinimumSize / GetMaximumSize
                    // answer Chromium when it asks; AppKit enforces the
                    // content limits during a live resize.
                    laufey_common::MacApplyContentSizeConstraints(
                        NSWindowForCefHandle(window->GetWindowHandle()), c);
#endif
                    // Have Chromium ask the delegate again.
                    window->InvalidateLayout();
                    if ((window->IsMaximized)() || (window->IsMinimized)() ||
                        window->IsFullscreen())
                      return;
                    CefRefPtr<CefBrowser> browser =
                        RuntimeLoader::GetInstance()->GetBrowserForWindow(wid);
                    auto browser_view =
                        browser ? CefBrowserView::GetForBrowser(browser)
                                : nullptr;
                    if (!browser_view)
                      return;
                    CefSize outer = window->GetSize();
                    CefSize frame = CefFrameAroundPage(browser_view);
                    int w = outer.width - frame.width;
                    int h = outer.height - frame.height;
                    if (laufey_common::ClampSize(c, &w, &h))
                      CefSetPageSize(browser_view, w, h);
                  },
                  window_id, c));
}

static void Backend_GetWindowSizeConstraints(void* /*data*/, uint32_t window_id,
                                             int* min_width, int* min_height,
                                             int* max_width, int* max_height) {
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

// CEF display ids are int64; keep them in the JS-safe range the ABI promises.
static int64_t CefSafeDisplayId(int64_t id) {
  if (id > 0 && id < (int64_t{1} << 53))
    return id;
  std::string s = std::to_string(id);
  return laufey_common::HashDisplayName(s.data(), s.size());
}

// UI thread.
static std::vector<laufey_screen_t> CefCollectScreens() {
  std::vector<laufey_screen_t> screens;
  std::vector<CefRefPtr<CefDisplay>> displays;
  CefDisplay::GetAllDisplays(displays);
  CefRefPtr<CefDisplay> primary = CefDisplay::GetPrimaryDisplay();
  int64_t primary_id = primary ? primary->GetID() : 0;
  for (const auto& d : displays) {
    laufey_screen_t s = {};
    s.id = CefSafeDisplayId(d->GetID());
    CefRect b = d->GetBounds();
    CefRect w = d->GetWorkArea();
    s.x = b.x;
    s.y = b.y;
    s.width = b.width;
    s.height = b.height;
    s.work_x = w.x;
    s.work_y = w.y;
    s.work_width = w.width;
    s.work_height = w.height;
    s.scale_factor = d->GetDeviceScaleFactor();
    s.is_primary = d->GetID() == primary_id;
    screens.push_back(s);
  }
  std::stable_partition(screens.begin(), screens.end(),
                        [](const laufey_screen_t& s) { return s.is_primary; });
  return screens;
}

static size_t Backend_GetScreens(void* /*data*/, laufey_screen_t* out,
                                 size_t capacity) {
  std::vector<laufey_screen_t> screens;
  cef_invoke_sync([&] { screens = CefCollectScreens(); });
  return laufey_common::CopyScreens(screens, out, capacity);
}

static int64_t Backend_GetWindowScreen(void* /*data*/, uint32_t window_id) {
  int64_t id = 0;
  cef_invoke_sync([&] {
    CefRefPtr<CefWindow> window = CefWindowForId(window_id);
    if (!window || window->IsClosed())
      return;
    // The screen the window overlaps most, from its bounds: CefWindow's
    // GetDisplay() crashed on macOS for a window that had just been shown
    // (seen in CI), and the overlap rule is what the ABI promises anyway.
    laufey_common::Bounds frame;
    CefPoint pos = window->GetPosition();
    CefSize size = window->GetSize();
    frame.x = pos.x;
    frame.y = pos.y;
    frame.width = size.width;
    frame.height = size.height;
    id = laufey_common::ScreenForBounds(CefCollectScreens(), frame);
  });
  return id;
}

#if !defined(__APPLE__) && !defined(_WIN32)
// Linux: CEF reports no display changes, so compare the layout every 2 s
// while a handler is registered.
static std::string CefScreensSignature() {
  std::string sig;
  for (const auto& s : CefCollectScreens()) {
    sig += std::to_string(s.id) + ":" + std::to_string(s.x) + "," +
           std::to_string(s.y) + "," + std::to_string(s.width) + "x" +
           std::to_string(s.height) + "/" + std::to_string(s.work_x) + "," +
           std::to_string(s.work_y) + "," + std::to_string(s.work_width) + "x" +
           std::to_string(s.work_height) + "@" +
           std::to_string(s.scale_factor) + (s.is_primary ? "p" : "") + ";";
  }
  return sig;
}

static void CefPollDisplays(std::string last) {
  std::string now = CefScreensSignature();
  if (now != last)
    laufey_common::NotifyDisplayChanged();
  CefPostDelayedTask(TID_UI, base::BindOnce(&CefPollDisplays, now), 2000);
}
#endif

static void Backend_SetDisplayChangedHandler(void* /*data*/,
                                             laufey_display_changed_fn handler,
                                             void* user_data) {
  laufey_common::SetDisplayChangedHandler(handler, user_data);
  if (!handler)
    return;
  static std::atomic<bool> installed{false};
  if (installed.exchange(true))
    return;
#if defined(__APPLE__)
  laufey_common::MacInstallDisplayWatcher();
#elif defined(_WIN32)
  CefPostTask(TID_UI, base::BindOnce(
                          [] { laufey_common::WinInstallDisplayWatcher(); }));
#else
  CefPostTask(TID_UI,
              base::BindOnce([] { CefPollDisplays(CefScreensSignature()); }));
#endif
}

static bool Backend_SetWindowTitlebarStyle(void* /*data*/, uint32_t window_id,
                                           int style) {
#if defined(__APPLE__)
  bool ok = false;
  cef_invoke_sync([&] {
    if (CefRefPtr<CefWindow> window = CefWindowForId(window_id)) {
      ok = laufey_common::MacSetTitlebarStyle(
          NSWindowForCefHandle(window->GetWindowHandle()), style);
    }
  });
  return ok;
#else
  (void)window_id;
  (void)style;
  return false;
#endif
}

static bool Backend_SetWindowTrafficLightPosition(void* /*data*/,
                                                  uint32_t window_id, int x,
                                                  int y) {
#if defined(__APPLE__)
  bool ok = false;
  cef_invoke_sync([&] {
    if (CefRefPtr<CefWindow> window = CefWindowForId(window_id)) {
      ok = laufey_common::MacSetTrafficLightPosition(
          NSWindowForCefHandle(window->GetWindowHandle()), x, y);
    }
  });
  return ok;
#else
  (void)window_id;
  (void)x;
  (void)y;
  return false;
#endif
}

static bool Backend_SetWindowBackdrop(void* /*data*/, uint32_t /*window_id*/,
                                      int backdrop, int /*material*/) {
  // See Backend_WindowCapabilities: nothing can show through the CEF view.
  return backdrop == LAUFEY_BACKDROP_NONE;
}

static bool Backend_GetWindowNormalBounds(void* /*data*/, uint32_t window_id,
                                          int* x, int* y, int* width,
                                          int* height) {
  bool found = false;
  laufey_common::Bounds b;
  cef_invoke_sync([&] {
    CefRefPtr<CefWindow> window = CefWindowForId(window_id);
    if (!window)
      return;
    found = true;
    if (CefStateOf(window) != 0 &&
        laufey_common::GetCommittedNormalBounds(window_id, &b))
      return;
    b = CefBoundsOf(window, window_id);
  });
  if (!found)
    return false;
  if (x)
    *x = b.x;
  if (y)
    *y = b.y;
  if (width)
    *width = b.width;
  if (height)
    *height = b.height;
  return true;
}

static void Backend_SetQuitOnLastWindowClosed(void* /*data*/, bool quit) {
  laufey_common::SetQuitOnLastWindowClosed(quit);
}

void RuntimeLoader::InitializeBackendApi() {
  memset(&backend_api_, 0, sizeof(backend_api_));
  backend_api_.version = LAUFEY_API_VERSION;
  backend_api_.backend_data = this;
  backend_api_.test_click_menu_item = Backend_TestClickMenuItem;
#ifdef __linux__
  // backend-common's GTK work (clipboard, file dialogs, drag source) runs on
  // the CEF UI thread, through CEF's own task queue.
  laufey_common::SetGtkThread(
      [](std::function<void()> fn) {
        CefPostTask(TID_UI, base::BindOnce([](std::function<void()> f) { f(); },
                                           std::move(fn)));
      },
      [] { return CefCurrentlyOn(TID_UI); });
#endif

  backend_api_.create_window = Backend_CreateWindow;
  backend_api_.create_window_ex = Backend_CreateWindowEx;
  backend_api_.close_window = Backend_CloseWindow;

  backend_api_.navigate = Backend_Navigate;
  backend_api_.set_title = Backend_SetTitle;
  backend_api_.execute_js = Backend_ExecuteJs;
  backend_api_.quit = Backend_Quit;
  backend_api_.set_window_size = Backend_SetWindowSize;
  backend_api_.get_window_size = Backend_GetWindowSize;
  backend_api_.get_window_outer_size = Backend_GetWindowOuterSize;
  backend_api_.set_window_position = Backend_SetWindowPosition;
  backend_api_.get_window_position = Backend_GetWindowPosition;
  backend_api_.get_window_inner_position = Backend_GetWindowInnerPosition;
  backend_api_.set_resizable = Backend_SetResizable;
  backend_api_.is_resizable = Backend_IsResizable;
  backend_api_.set_always_on_top = Backend_SetAlwaysOnTop;
  backend_api_.is_always_on_top = Backend_IsAlwaysOnTop;
  backend_api_.set_window_opacity = Backend_SetWindowOpacity;
  backend_api_.get_window_opacity = Backend_GetWindowOpacity;
  backend_api_.get_window_scale_factor = Backend_GetWindowScaleFactor;
  backend_api_.set_click_passthrough = Backend_SetClickPassthrough;
  backend_api_.is_click_passthrough = Backend_IsClickPassthrough;
  backend_api_.set_click_passthrough_forward =
      Backend_SetClickPassthroughForward;
  backend_api_.is_click_passthrough_forward = Backend_IsClickPassthroughForward;
  backend_api_.is_visible = Backend_IsVisible;
  backend_api_.show = Backend_Show;
  backend_api_.hide = Backend_Hide;
  backend_api_.focus = Backend_Focus;
  backend_api_.post_ui_task = Backend_PostUiTask;

  laufey_register_value_api(&backend_api_);

  backend_api_.set_js_call_handler = Backend_SetJsCallHandler;
  backend_api_.set_js_call_handler_ex = Backend_SetJsCallHandlerEx;
  backend_api_.platform_features = Backend_PlatformFeatures;
  backend_api_.tray_unavailable_reason = Backend_TrayUnavailableReason;
  backend_api_.set_platform_features_changed_handler =
      Backend_SetPlatformFeaturesChangedHandler;
  backend_api_.exit_app = Backend_ExitApp;
  backend_api_.title_bar_preferences = Backend_TitleBarPreferences;
  backend_api_.set_title_bar_preferences_changed_handler =
      Backend_SetTitleBarPreferencesChangedHandler;
#if defined(__linux__) || defined(__APPLE__)
  // The secure store (API 47): the Secret Service through libsecret on
  // Linux, the Keychain (Security.framework, items only this app may read
  // without a prompt; see docs/secure-store.md) on macOS. Windows keeps NULL
  // (its Credential Locker is the embedder's).
  backend_api_.secret_lookup = [](void*, const char* service,
                                  const char* account, uint32_t timeout_ms,
                                  char** value, char** reason) {
    return laufey_common::SecretLookupForAbi(service, account, timeout_ms,
                                             value, reason);
  };
  backend_api_.secret_store =
      [](void*, const char* service, const char* account, const char* label,
         const char* value, uint32_t timeout_ms, char** reason) {
        return laufey_common::SecretStoreForAbi(service, account, label, value,
                                                timeout_ms, reason);
      };
  backend_api_.secret_delete = [](void*, const char* service,
                                  const char* account, uint32_t timeout_ms,
                                  char** reason) {
    return laufey_common::SecretDeleteForAbi(service, account, timeout_ms,
                                             reason);
  };
#endif
  backend_api_.js_call_respond = Backend_JsCallRespond;

  backend_api_.invoke_js_callback = Backend_InvokeJsCallback;
  backend_api_.release_js_callback = Backend_ReleaseJsCallback;

  backend_api_.get_window_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
  backend_api_.get_display_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
#if defined(_WIN32)
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return LAUFEY_WINDOW_HANDLE_WIN32;
  };
#elif defined(__APPLE__)
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return LAUFEY_WINDOW_HANDLE_APPKIT;
  };
#else
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return LAUFEY_WINDOW_HANDLE_X11;
  };
#endif

  backend_api_.set_keyboard_event_handler = Backend_SetKeyboardEventHandler;
  backend_api_.set_mouse_click_handler = Backend_SetMouseClickHandler;
  backend_api_.set_mouse_move_handler = Backend_SetMouseMoveHandler;
  backend_api_.set_wheel_handler = Backend_SetWheelHandler;
  backend_api_.set_cursor_enter_leave_handler =
      Backend_SetCursorEnterLeaveHandler;
  backend_api_.set_focused_handler = Backend_SetFocusedHandler;
  backend_api_.set_resize_handler = Backend_SetResizeHandler;
  backend_api_.set_move_handler = Backend_SetMoveHandler;
  backend_api_.set_close_requested_handler = Backend_SetCloseRequestedHandler;
  backend_api_.test_trigger_close_requested = Backend_TestTriggerCloseRequested;
  backend_api_.test_inject_input = Backend_TestInjectInput;

  // Window state, constraints, screens and chrome (API >= 38).
  backend_api_.set_window_state = Backend_SetWindowState;
  backend_api_.get_window_state = Backend_GetWindowState;
  backend_api_.set_window_state_handler = Backend_SetWindowStateHandler;
  backend_api_.set_window_size_constraints = Backend_SetWindowSizeConstraints;
  backend_api_.get_window_size_constraints = Backend_GetWindowSizeConstraints;
  backend_api_.get_screens = Backend_GetScreens;
  backend_api_.get_window_screen = Backend_GetWindowScreen;
  backend_api_.set_display_changed_handler = Backend_SetDisplayChangedHandler;
  backend_api_.window_capabilities = Backend_WindowCapabilities;
  backend_api_.set_window_titlebar_style = Backend_SetWindowTitlebarStyle;
  backend_api_.set_window_traffic_light_position =
      Backend_SetWindowTrafficLightPosition;
  backend_api_.set_window_backdrop = Backend_SetWindowBackdrop;
  backend_api_.get_window_normal_bounds = Backend_GetWindowNormalBounds;
  backend_api_.set_quit_on_last_window_closed =
      Backend_SetQuitOnLastWindowClosed;

  backend_api_.poll_js_calls = [](void* data) {
    RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
    loader->PollPendingJsCalls();
  };

  backend_api_.set_js_call_notify = [](void* data, void (*notify_fn)(void*),
                                       void* notify_data) {
    RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
    loader->SetJsCallNotify(notify_fn, notify_data);
  };

  backend_api_.register_scheme_handler = Backend_RegisterSchemeHandler;
  backend_api_.scheme_request_read_body = Backend_SchemeRequestReadBody;
  backend_api_.scheme_response_begin = Backend_SchemeResponseBegin;
  backend_api_.scheme_response_write = Backend_SchemeResponseWrite;
  backend_api_.scheme_response_finish = Backend_SchemeResponseFinish;

#if defined(__APPLE__)
  backend_api_.set_application_menu = Backend_SetApplicationMenu_Mac;
  backend_api_.show_context_menu = Backend_ShowContextMenu_Mac;
  backend_api_.show_context_menu_ex = Backend_ShowContextMenuEx_Mac;
  backend_api_.menu_capabilities = [](void*) -> uint32_t {
    return LAUFEY_MENU_CAP_APP_MENU | LAUFEY_MENU_CAP_ACCELERATORS |
           LAUFEY_MENU_CAP_CONTEXT_MENU | LAUFEY_MENU_CAP_CONTEXT_CLOSED |
           LAUFEY_MENU_CAP_ICONS | LAUFEY_MENU_CAP_TOOLTIPS;
  };
  backend_api_.test_trigger_menu_accelerator =
      [](void*, uint32_t window_id, const char* accelerator) -> bool {
    return laufey_common::TestTriggerMenuAcceleratorMac(window_id, accelerator);
  };
#else
  // Windows and Linux: a CEF Views menu bar with window accelerators.
  backend_api_.set_application_menu = Backend_SetApplicationMenu;
  backend_api_.show_context_menu = Backend_ShowContextMenu;
  backend_api_.show_context_menu_ex = Backend_ShowContextMenuEx;
  backend_api_.menu_capabilities = Backend_MenuCapabilities;
  backend_api_.test_trigger_menu_accelerator =
      Backend_TestTriggerMenuAccelerator;
#endif
  backend_api_.test_dismiss_context_menu = Backend_TestDismissContextMenu;

  backend_api_.open_devtools = Backend_OpenDevTools;
  backend_api_.print_to_pdf = Backend_PrintToPdf;
  backend_api_.set_js_namespace = Backend_SetJsNamespace;
  backend_api_.show_dialog = Backend_ShowDialog;
  backend_api_.string_free = Backend_StringFree;
  backend_api_.read_clipboard_text = Backend_ReadClipboardText;
  backend_api_.write_clipboard_text = Backend_WriteClipboardText;

  // --- Dock / taskbar ---
#if defined(__APPLE__)
  backend_api_.set_dock_badge = Backend_SetDockBadge_Mac;
  backend_api_.bounce_dock = Backend_BounceDock_Mac;
  backend_api_.set_dock_menu = Backend_SetDockMenu_Mac;
  backend_api_.set_dock_visible = Backend_SetDockVisible_Mac;
  backend_api_.set_dock_reopen_handler = Backend_SetDockReopenHandler_Mac;
  // Deep links: macOS-only by design (see set_open_url_handler in laufey.h).
  // Windows/Linux receive the URL as argv in a new process, which only the
  // embedder can turn into "focus the running app", so the pointers stay
  // NULL there and an embedder can detect the absence.
  backend_api_.set_open_url_handler = Backend_SetOpenUrlHandler_Mac;
  backend_api_.test_trigger_open_url = Backend_TestTriggerOpenUrl_Mac;
#elif defined(_WIN32)
  backend_api_.bounce_dock = Backend_BounceDock_Win;
  backend_api_.set_dock_badge = Backend_SetDockBadge_TitlePrefix;
  // Menu/visible/reopen have no Windows analog — left nullptr; the
  // runtime-side Rust wrapper silently no-ops when the pointer is missing.
#elif defined(__linux__)
  backend_api_.bounce_dock = Backend_BounceDock_Linux;
  backend_api_.set_dock_badge = Backend_SetDockBadge_TitlePrefix;
  // Menu/visible/reopen: left nullptr (no clean Linux analog).
#endif

  // Single instance (API >= 36), every OS: forwarded launches from
  // laufey_single_instance (see docs/deep-links.md).
  backend_api_.set_second_instance_handler = Backend_SetSecondInstanceHandler;
  // Passkeys (API >= 37): see docs/passkeys.md.
  backend_api_.passkey_capabilities = Backend_PasskeyCapabilities;
  backend_api_.passkey_request = Backend_PasskeyRequest;

  // Drag and drop, file dialogs and the rich clipboard (API >= 39).
  backend_api_.set_file_drop_handler = Backend_SetFileDropHandler;
  backend_api_.start_file_drag = Backend_StartFileDrag;
  backend_api_.test_trigger_file_drop = Backend_TestTriggerFileDrop;
  backend_api_.show_file_dialog = Backend_ShowFileDialog;
  backend_api_.cancel_file_dialog = Backend_CancelFileDialog;
  backend_api_.test_file_dialog_respond = Backend_TestFileDialogRespond;
  backend_api_.clipboard_capabilities = Backend_ClipboardCapabilities;
  backend_api_.read_clipboard_html = Backend_ReadClipboardHtml;
  backend_api_.write_clipboard_html = Backend_WriteClipboardHtml;
  backend_api_.read_clipboard_image = Backend_ReadClipboardImage;
  backend_api_.write_clipboard_image = Backend_WriteClipboardImage;
  backend_api_.read_clipboard_formats = Backend_ReadClipboardFormats;
  backend_api_.set_clipboard_change_handler = Backend_SetClipboardChangeHandler;
  backend_api_.buffer_free = Backend_BufferFree;

  // Global shortcuts, launch at login, DevTools (API >= 40): see
  // docs/global-shortcuts.md, docs/launch-at-login.md, docs/devtools.md.
#if defined(__APPLE__)
  laufey_common::InstallShortcutPlatform(
      laufey_common::CreateShortcutPlatformMac());
#elif defined(_WIN32)
  // RegisterHotKey's window lives on the CEF UI thread.
  laufey_common::InstallShortcutPlatform(
      laufey_common::CreateShortcutPlatformWin([](std::function<void()> task) {
        if (CefCurrentlyOn(TID_UI)) {
          task();
          return;
        }
        CefPostTask(TID_UI, base::BindOnce([](std::function<void()> t) { t(); },
                                           std::move(task)));
      }));
#else
  laufey_common::InstallShortcutPlatform(
      laufey_common::CreateShortcutPlatformLinux());
#endif
  backend_api_.system_capabilities = Backend_SystemCapabilities;
  backend_api_.set_shortcut_handler = Backend_SetShortcutHandler;
  backend_api_.register_shortcut = Backend_RegisterShortcut;
  backend_api_.unregister_shortcut = Backend_UnregisterShortcut;
  backend_api_.unregister_all_shortcuts = Backend_UnregisterAllShortcuts;
  backend_api_.list_shortcuts = Backend_ListShortcuts;
  backend_api_.canonicalize_accelerator = Backend_CanonicalizeAccelerator;
  backend_api_.test_trigger_shortcut = Backend_TestTriggerShortcut;
  backend_api_.get_launch_at_login = Backend_GetLaunchAtLogin;
  backend_api_.set_launch_at_login = Backend_SetLaunchAtLogin;
  backend_api_.close_devtools = Backend_CloseDevTools;
  backend_api_.is_devtools_open = Backend_IsDevToolsOpen;
  backend_api_.is_devtools_enabled = Backend_IsDevToolsEnabled;

  // --- Tray / status bar ---
#if defined(__APPLE__)
  backend_api_.create_tray_icon = Backend_CreateTrayIcon_Mac;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon_Mac;
  backend_api_.set_tray_icon = Backend_SetTrayIcon_Mac;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip_Mac;
  backend_api_.set_tray_menu = Backend_SetTrayMenu_Mac;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler_Mac;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler_Mac;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark_Mac;
  backend_api_.get_tray_icon_bounds = Backend_GetTrayIconBounds_Mac;
#elif defined(_WIN32)
  backend_api_.create_tray_icon = Backend_CreateTrayIcon_Win;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon_Win;
  backend_api_.set_tray_icon = Backend_SetTrayIcon_Win;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip_Win;
  backend_api_.set_tray_menu = Backend_SetTrayMenu_Win;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler_Win;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler_Win;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark_Win;
  backend_api_.get_tray_icon_bounds = Backend_GetTrayIconBounds_Win;
#elif defined(__linux__)
  backend_api_.create_tray_icon = Backend_CreateTrayIcon_Linux;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon_Linux;
  backend_api_.set_tray_icon = Backend_SetTrayIcon_Linux;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip_Linux;
  backend_api_.set_tray_menu = Backend_SetTrayMenu_Linux;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler_Linux;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler_Linux;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark_Linux;
  // No get_tray_icon_bounds on Linux: the AppIndicator / StatusNotifier
  // protocol does not expose the icon's screen position, so it stays NULL
  // and Tray.getBounds() reports null.
#endif

  // --- Notifications and permissions (laufey_notifications.h) ---
  backend_api_.show_notification = Backend_ShowNotification;
  backend_api_.close_notification = Backend_CloseNotification;
  backend_api_.query_permission = Backend_QueryPermission;
  backend_api_.request_permission = Backend_RequestPermission;
  backend_api_.notification_capabilities = Backend_NotificationCapabilities;
  backend_api_.set_notification_response_handler =
      Backend_SetNotificationResponseHandler;
  backend_api_.list_scheduled_notifications =
      Backend_ListScheduledNotifications;
  backend_api_.cancel_notification = Backend_CancelNotification;
  backend_api_.test_notification_respond = Backend_TestNotificationRespond;

  // UI-thread tasks and auth sessions (API >= 42): see docs/c-abi.md and
  // docs/auth-session.md. The dispatcher is bound to TID_UI in Load.
  backend_api_.dispatch_ui_task = Backend_DispatchUiTask;
  backend_api_.is_ui_thread = Backend_IsUiThread;
  backend_api_.auth_session_capabilities = Backend_AuthSessionCapabilities;
  backend_api_.auth_session_start = Backend_AuthSessionStart;
  backend_api_.test_cancel_auth_session = Backend_TestCancelAuthSession;
  backend_api_.auth_session_cancel = Backend_AuthSessionCancel;
}

// --- RuntimeLoader lifecycle ---

RuntimeLoader::RuntimeLoader() {
  instance_ = this;
  InitializeBackendApi();
}

RuntimeLoader::~RuntimeLoader() {
  Shutdown();
  if (library_handle_) {
#ifndef _WIN32
    dlclose(library_handle_);
#else
    FreeLibrary(static_cast<HMODULE>(library_handle_));
#endif
  }
  instance_ = nullptr;
}

RuntimeLoader* RuntimeLoader::GetInstance() {
  if (!instance_) {
    instance_ = new RuntimeLoader();
  }
  return instance_;
}

bool RuntimeLoader::Load(const std::string& path) {
  // Load runs on TID_UI (OnContextInitialized), or on the main thread of a
  // headless worker, which has no loop (its host calls UiLoopEnded before
  // starting the runtime). CefPostTask refuses once CEF has shut down.
  laufey_common::UiTaskDispatcher::Get().Bind(
      [](void (*task)(void*), void* task_data) {
        return CefPostTask(
            TID_UI, base::BindOnce([](void (*t)(void*), void* d) { t(d); },
                                   task, task_data));
      });
#if defined(__linux__)
  // Deno.exit() ends the process from the runtime thread: keep the UI thread
  // from drawing while exit tears the libraries down.
  laufey_common::InstallUiExitGuard();
#endif
#if defined(_WIN32)
  // A context menu's TrackPopupMenu runs a native modal loop on TID_UI, and
  // Chromium runs no tasks inside one unless told to: every CefPostTask (the
  // runtime's synchronous UI-thread calls, dispatch_ui_task, the page's
  // binding calls) would wait for the menu to close, and the runtime with
  // them. Allow nestable tasks for the length of the loop (laufey_menu.h).
  // The menu code is reentrancy safe for this: a task that shows another
  // menu ends the open one first (win32_menu.h), and one that closes the
  // window ends it too. Chromium's pump takes a message from the queue each
  // time it gets a slice in such a loop, which would take the menu's own
  // keys and clicks from it: those stay the menu's (app.h).
  laufey_common::SetNativeModalLoopHook([](bool entering) {
    LaufeyGuardModalLoopInput(entering);
    CefSetNestableTasksAllowed(entering);
  });
#endif
#ifndef _WIN32
  library_handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: " << dlerror() << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<laufey_runtime_init_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_INIT_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<laufey_runtime_start_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_START_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<laufey_runtime_shutdown_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_SHUTDOWN_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }
#else
  library_handle_ = LoadLibraryW(laufey_common::Utf8ToWide(path).c_str());
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: error " << GetLastError()
              << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<laufey_runtime_init_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_INIT_SYMBOL << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<laufey_runtime_start_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_START_SYMBOL << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<laufey_runtime_shutdown_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_SHUTDOWN_SYMBOL
              << std::endl;
    return false;
  }
#endif

  std::cout << "Runtime loaded successfully from: " << path << std::endl;
  return true;
}

bool RuntimeLoader::Start() {
  if (running_) {
    return true;
  }

  if (!init_fn_ || !start_fn_) {
    std::cerr << "Runtime not loaded" << std::endl;
    return false;
  }

  int result = init_fn_(&backend_api_);
  if (result != 0) {
    std::cerr << "Runtime init failed with code: " << result << std::endl;
    return false;
  }

  running_ = true;
  runtime_thread_ = std::thread(&RuntimeLoader::RuntimeThread, this);

  std::cout << "Runtime started" << std::endl;
  return true;
}

void RuntimeLoader::RuntimeThread() {
  int result = start_fn_();
  if (result != 0) {
    std::cerr << "Runtime start returned error: " << result << std::endl;
  }
  running_ = false;
  runtime_exit_.Done();
}

void RuntimeLoader::Shutdown() {
  if (shutdown_fn_) {
    shutdown_fn_();
  }

  if (runtime_thread_.joinable()) {
    // The app asked to exit (exit_app): its thread may never return (an
    // exit() that blocks for good, as Deno.exit()), and the process ends
    // with the code it asked for without it. A runtime that does return is
    // given a moment to.
    if (laufey_common::ExitRequested() &&
        !runtime_exit_.WaitFor(kRuntimeExitGrace)) {
      runtime_thread_.detach();
      return;
    }
    // The loop has ended (UiLoopEnded), so the runtime's synchronous UI calls
    // already return. A runtime that still ignores the shutdown is abandoned
    // after the timeout, as on Winit, rather than hanging the exit.
    if (runtime_exit_.WaitFor(kRuntimeShutdownTimeout)) {
      runtime_thread_.join();
    } else {
      std::cerr << "laufey: the runtime did not stop within "
                << kRuntimeShutdownTimeout.count()
                << " ms of shutdown; exiting without it" << std::endl;
      runtime_thread_.detach();
    }
  }
}

void RuntimeLoader::HandleEvalResult(uint64_t eval_id,
                                     CefRefPtr<CefValue> result,
                                     const std::string& error) {
  PendingEval eval;
  {
    std::lock_guard<std::mutex> lock(eval_mutex_);
    auto it = pending_evals_.find(eval_id);
    if (it == pending_evals_.end())
      return;
    eval = it->second;
    pending_evals_.erase(it);
  }

  if (!error.empty()) {
    laufey_value errLaufey(laufey::Value::String(error));
    eval.callback(nullptr, &errLaufey, eval.callback_data);
  } else if (result && result->GetType() != VTYPE_NULL) {
    laufey_value resultLaufey(CefValueToLaufey(result));
    eval.callback(&resultLaufey, nullptr, eval.callback_data);
  } else {
    eval.callback(nullptr, nullptr, eval.callback_data);
  }
}

void RuntimeLoader::OnJsCall(uint32_t window_id, uint64_t page_call_id,
                             const std::string& method_path,
                             CefRefPtr<CefListValue> args,
                             const std::string& origin) {
  // The CefListValue passed in is owned by the CefProcessMessage and
  // becomes invalid once OnProcessMessageReceived returns. Copy it so the
  // queued entry survives until PollPendingJsCalls runs.
  CefRefPtr<CefListValue> owned_args =
      args ? args->Copy() : CefListValue::Create();
  uint64_t call_id = js_calls_.Add(window_id, page_call_id);
  // A document the launch file's bridge pin doesn't cover never reaches the
  // runtime (the renderer doesn't bind the namespace there either; this
  // catches a compromised renderer).
  if (!laufey_common::BridgeOriginAllowed(
          laufey_common::ProcessBridgeOriginPolicy(), origin)) {
    std::cerr << "laufey: refused a bridge call from " << origin
              << " (not in the app's bridgeOrigins)" << std::endl;
    laufey_value_t err(
        laufey::Value::String(laufey_common::kBridgeOriginRefused));
    Backend_JsCallRespond(this, call_id, nullptr, &err);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_js_calls_.push(
        {window_id, call_id, method_path, owned_args, origin});
  }

  std::lock_guard<std::mutex> lock(notify_mutex_);
  if (js_call_notify_fn_) {
    js_call_notify_fn_(js_call_notify_data_);
  }
}

void RuntimeLoader::PollPendingJsCalls() {
  std::vector<PendingJsCall> calls;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    while (!pending_js_calls_.empty()) {
      calls.push_back(std::move(pending_js_calls_.front()));
      pending_js_calls_.pop();
    }
  }

  if (calls.empty())
    return;

  laufey_js_call_fn handler;
  void* user_data;
  laufey_js_call_ex_fn handler_ex;
  void* user_data_ex;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = js_call_handler_;
    user_data = js_call_user_data_;
    handler_ex = js_call_handler_ex_;
    user_data_ex = js_call_user_data_ex_;
  }

  for (auto& call : calls) {
    if (handler_ex) {
      CefRefPtr<CefValue> argsValue = CefValue::Create();
      argsValue->SetList(call.args);
      laufey_value_t* argsWrapper =
          new laufey_value(CefValueToLaufey(argsValue));
      handler_ex(user_data_ex, call.window_id, call.call_id,
                 call.method_path.c_str(), argsWrapper, call.origin.c_str());
    } else if (handler) {
      CefRefPtr<CefValue> argsValue = CefValue::Create();
      argsValue->SetList(call.args);
      laufey_value_t* argsWrapper =
          new laufey_value(CefValueToLaufey(argsValue));
      handler(user_data, call.window_id, call.call_id, call.method_path.c_str(),
              argsWrapper);
    } else {
      laufey_value_t errWrapper(
          laufey::Value::String("No JS call handler registered"));
      Backend_JsCallRespond(this, call.call_id, nullptr, &errWrapper);
    }
  }
}

void RuntimeLoader::SetSchemeRequestHandler(const std::string& scheme,
                                            laufey_scheme_request_fn handler,
                                            laufey_scheme_cancel_fn on_cancel,
                                            void* user_data) {
  // Schemes that still need a handler factory. The built-in "app" is served
  // whenever any handler is registered, as on the WebView backends, so an
  // embedder that registers only its own scheme keeps app:// working.
  std::vector<std::string> to_register;
  {
    std::lock_guard<std::mutex> lock(scheme_mutex_);
    scheme_request_handler_ = handler;
    scheme_cancel_handler_ = on_cancel;
    scheme_user_data_ = user_data;
    if (handler) {
      std::vector<std::string> wanted = {LAUFEY_APP_SCHEME};
      if (!laufey_common::IsValidSchemeName(scheme)) {
        std::cerr << "laufey: ignoring invalid URL scheme name \"" << scheme
                  << "\" passed to register_scheme_handler" << std::endl;
      } else {
        wanted.push_back(laufey_common::NormalizeSchemeName(scheme));
      }
      for (const std::string& s : wanted) {
        if (scheme_factories_.insert(s).second) {
          to_register.push_back(s);
        }
      }
    }
  }
  for (const std::string& s : to_register) {
    if (!laufey_schemes::IsDeclared(s)) {
      // The factory still serves the scheme, but Chromium registered its
      // standard/secure/CORS flags at startup, so an undeclared scheme gets
      // an opaque origin and no secure context.
      std::cerr << "laufey: scheme \"" << s
                << "\" was not declared at startup (--"
                << laufey_schemes::kSwitch << " / " << laufey_schemes::kEnv
                << " / \"customSchemes\" in laufey-launch.json); pages "
                   "served over it will not be a secure "
                   "`<scheme>://<host>` origin"
                << std::endl;
    }
    // CefRegisterSchemeHandlerFactory must run on the UI thread.
    CefPostTask(TID_UI, base::BindOnce(
                            [](std::string name) {
                              CefRegisterSchemeHandlerFactory(
                                  name, "", new LaufeySchemeHandlerFactory());
                            },
                            s));
  }
}

void RuntimeLoader::DispatchSchemeCancel(void* exchange) {
  laufey_scheme_cancel_fn on_cancel;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(scheme_mutex_);
    on_cancel = scheme_cancel_handler_;
    user_data = scheme_user_data_;
  }
  if (on_cancel)
    on_cancel(user_data, reinterpret_cast<laufey_scheme_exchange_t*>(exchange));
}

void RuntimeLoader::DispatchSchemeRequest(uint32_t window_id, void* exchange,
                                          const std::string& method,
                                          const std::string& url,
                                          const std::string& flat_headers) {
  laufey_scheme_request_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(scheme_mutex_);
    handler = scheme_request_handler_;
    user_data = scheme_user_data_;
  }
  if (handler) {
    handler(user_data, window_id,
            reinterpret_cast<laufey_scheme_exchange_t*>(exchange),
            method.c_str(), url.c_str(), flat_headers.data(),
            flat_headers.size());
  } else {
    // No handler registered: finish the exchange so the request doesn't hang.
    reinterpret_cast<LaufeySchemeHandler*>(exchange)->FinishResponse();
  }
}
