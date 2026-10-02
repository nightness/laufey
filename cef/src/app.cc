// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "app.h"
#include "custom_schemes.h"
#include "runtime_loader.h"
#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_launch_args.h"
#include "laufey_launch_config.h"
#include "laufey_menu.h"
#include "laufey_external_links.h"
#include "laufey_passkey.h"
#include "laufey_scheme_registry.h"
#include "laufey_auth_session.h"
#include "laufey_single_instance.h"
#include "laufey_window.h"
#include "scheme_handler.h"
#if defined(_WIN32) || defined(__linux__)
#include "views_menu.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef __linux__
#include <gtk/gtk.h>
#endif

#ifdef __APPLE__
// Defined in runtime_loader_mac.mm
struct NativeDialogResult {
  bool confirmed;
  std::string text;
};
NativeDialogResult ShowNativeJSDialog_Mac(int type, const std::string& message,
                                          const std::string& default_text);
#endif

#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/cef_command_ids.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

std::string g_runtime_path;
std::string g_app_id;
std::queue<uint32_t> g_pending_laufey_ids;

namespace {
LaufeyHandler* g_handler = nullptr;
}

// LaufeyWindowDelegate implementation

void LaufeyWindowDelegate::OnWindowCreated(CefRefPtr<CefWindow> window) {
  window->AddChildView(browser_view_);

  // Register native window for event routing. Non-activating panels
  // (LAUFEY_WINDOW_FLAG_NO_ACTIVATE) are reconfigured before the first Show()
  // so they float without stealing focus from the foreground app.
  bool no_activate = (flags_ & LAUFEY_WINDOW_FLAG_NO_ACTIVATE) != 0;
  CefWindowHandle handle = window->GetWindowHandle();
  if (handle && laufey_id_ > 0) {
#if defined(__APPLE__)
    if (no_activate) {
      ConfigureNSWindowAsPanelForCefHandle(handle);
    }
    if ((flags_ & LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR) != 0) {
      ConfigureNSWindowTransparentTitlebarForCefHandle(handle);
    }
    RegisterNSWindowForCefHandle(handle, laufey_id_);
    laufey_common::MacObserveWindowStateChanges(NSWindowForCefHandle(handle),
                                                laufey_id_,
                                                CefScheduleWindowStateRecheck);
#elif defined(_WIN32)
    if (no_activate) {
      ConfigureWin32WindowAsPanel((void*)handle);
    }
    RuntimeLoader::GetInstance()->RegisterNativeHandle((void*)(uintptr_t)handle,
                                                       laufey_id_);
    // WM_SIZE is the one signal for a minimize / restore on Windows.
    laufey_common::WinSubclassForStateChanges(reinterpret_cast<void*>(handle),
                                              laufey_id_,
                                              CefScheduleWindowStateRecheck);
#elif defined(__linux__)
    if (no_activate) {
      ConfigureLinuxWindowAsPanel(handle);
    }
    RuntimeLoader::GetInstance()->RegisterNativeHandle((void*)(uintptr_t)handle,
                                                       laufey_id_);
    MonitorLinuxWindowEvents(handle);
#endif
  }

  window->Show();
  InstallNativeMouseMonitor();
  if (laufey_id_ > 0)
    CefScheduleWindowStateRecheck(laufey_id_);
}

// The constraints are page (client) sizes, like set_window_size, which is
// what Chromium applies the delegate's minimum / maximum to.
CefSize LaufeyWindowDelegate::GetMinimumSize(CefRefPtr<CefView> view) {
  laufey_common::SizeConstraints c =
      laufey_common::GetSizeConstraints(laufey_id_);
  if (c.min_width == 0 && c.min_height == 0)
    return CefSize();
  return CefSize(c.min_width, c.min_height);
}

CefSize LaufeyWindowDelegate::GetMaximumSize(CefRefPtr<CefView> view) {
  laufey_common::SizeConstraints c =
      laufey_common::GetSizeConstraints(laufey_id_);
  // CefSize() (0x0) means "no maximum"; an unbounded axis next to a bounded
  // one gets a size nothing reaches.
  if (c.max_width == 0 && c.max_height == 0)
    return CefSize();
  return CefSize(c.max_width > 0 ? c.max_width : 1 << 24,
                 c.max_height > 0 ? c.max_height : 1 << 24);
}

void LaufeyWindowDelegate::OnWindowBoundsChanged(CefRefPtr<CefWindow> window,
                                                 const CefRect& new_bounds) {
  if (laufey_id_ > 0)
    CefRecheckWindowState(laufey_id_);
}

void LaufeyWindowDelegate::OnWindowFullscreenTransition(
    CefRefPtr<CefWindow> window, bool is_completed) {
  if (laufey_id_ > 0 && is_completed)
    CefScheduleWindowStateRecheck(laufey_id_);
}

bool LaufeyWindowDelegate::IsFrameless(CefRefPtr<CefWindow> window) {
  return (flags_ & LAUFEY_WINDOW_FLAG_FRAMELESS) != 0;
}

cef_state_t LaufeyWindowDelegate::AcceptsFirstMouse(
    CefRefPtr<CefWindow> window) {
  return (flags_ & LAUFEY_WINDOW_FLAG_NO_ACTIVATE) ? STATE_ENABLED
                                                   : STATE_DEFAULT;
}

#if defined(__linux__)
void LaufeyWindowDelegate::OnWindowActivationChanged(
    CefRefPtr<CefWindow> window, bool active) {
  if (laufey_id_ > 0) {
    RuntimeLoader::GetInstance()->DispatchFocusedEvent(laufey_id_, active);
    // A window manager iconifies / restores through activation changes too.
    CefScheduleWindowStateRecheck(laufey_id_);
  }
}

bool LaufeyWindowDelegate::GetLinuxWindowProperties(
    CefRefPtr<CefWindow> window, CefLinuxWindowProperties& properties) {
  if (g_app_id.empty()) {
    return false;
  }
  // Same id for Wayland's app_id and X11's WM_CLASS (both class and name) so
  // the window matches the installed `<g_app_id>.desktop` under either backend.
  CefString(&properties.wayland_app_id) = g_app_id;
  CefString(&properties.wm_class_class) = g_app_id;
  CefString(&properties.wm_class_name) = g_app_id;
  return true;
}
#endif

void LaufeyWindowDelegate::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  // Unregister native window
  CefWindowHandle handle = window->GetWindowHandle();
  if (handle) {
    // A passkey sheet / dialog anchored to this window ends with it
    // (`cancelled`).
#if defined(__APPLE__)
    // By now the content view may be detached from its NSWindow: prefer the
    // NSWindow registered for this laufey window.
    void* nswindow =
        laufey_id_ > 0
            ? RuntimeLoader::GetInstance()->GetNSWindowForLaufeyId(laufey_id_)
            : nullptr;
    if (!nswindow)
      nswindow = NSWindowForCefHandle(handle);
    laufey_common::PasskeyWindowClosing(nswindow);
    // So does an auth session sheet.
    laufey_common::AuthSessionWindowClosing(nswindow);
#elif defined(_WIN32)
    laufey_common::PasskeyWindowClosing(reinterpret_cast<void*>(handle));
#endif
#ifdef __APPLE__
    laufey_common::MacUnwatchWindowState(NSWindowForCefHandle(handle));
    UnregisterNSWindowForCefHandle(handle);
#else
#if defined(_WIN32)
    laufey_common::WinUnsubclassForStateChanges(
        reinterpret_cast<void*>(handle));
#endif
    RuntimeLoader::GetInstance()->UnregisterNativeHandle(
        (void*)(uintptr_t)handle);
#endif
  }
  if (laufey_id_ > 0) {
    RuntimeLoader::GetInstance()->UnregisterBrowser(laufey_id_);
    laufey_common::ForgetWindow(laufey_id_);
#if defined(_WIN32) || defined(__linux__)
    laufey_cef_menu::ForgetWindow(laufey_id_);
#endif
  }
  RemoveNativeMouseMonitor();
  browser_view_ = nullptr;
}

#if defined(_WIN32) || defined(__linux__)
bool LaufeyWindowDelegate::OnAccelerator(CefRefPtr<CefWindow> /*window*/,
                                         int command_id) {
  // An app menu item's accelerator (views_menu.cc).
  return laufey_id_ > 0 &&
         laufey_cef_menu::OnAccelerator(laufey_id_, command_id);
}
#endif

bool LaufeyWindowDelegate::CanClose(CefRefPtr<CefWindow> window) {
  // The close-requested negotiation lives here, not in DoClose: laufey's
  // Views-hosted browsers run the default Chrome runtime style, and CEF
  // only calls CefLifeSpanHandler::DoClose for Alloy-style browsers (see
  // include/cef_life_span_handler.h) — a DoClose-based dispatch would
  // simply never fire. CanClose runs for every close attempt on the
  // CefWindow, user- and code-initiated alike; programmatic closes
  // (close_window(), app quit) mark themselves close-allowed first so they
  // skip the negotiation instead of re-deferring forever.
  auto* loader = RuntimeLoader::GetInstance();
  if (laufey_id_ > 0 && !loader->IsCloseAllowed(laufey_id_) &&
      !loader->DispatchCloseRequestedEvent(laufey_id_)) {
    // A registered handler defers the close: the app decides later, out of
    // band, by calling close_window().
    return false;
  }
  CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
  return browser ? browser->GetHost()->TryCloseBrowser() : true;
}

CefSize LaufeyWindowDelegate::GetPreferredSize(CefRefPtr<CefView> view) {
  return CefSize(800, 600);
}

LaufeyHandler::LaufeyHandler() {
  g_handler = this;
}

LaufeyHandler::~LaufeyHandler() {
  g_handler = nullptr;
}

LaufeyHandler* LaufeyHandler::GetInstance() {
  return g_handler;
}

void LaufeyHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  browser_list_.push_back(browser);

  auto* loader = RuntimeLoader::GetInstance();
  if (!g_pending_laufey_ids.empty()) {
    uint32_t laufey_id = g_pending_laufey_ids.front();
    g_pending_laufey_ids.pop();
    loader->RegisterBrowser(laufey_id, browser);
  }
#if defined(_WIN32)
  // External file drops: see LaufeyNativeDragFilePaths.
  if (auto view = CefBrowserView::GetForBrowser(browser)) {
    if (auto window = view->GetWindow())
      LaufeyHookWindowDropTarget(window->GetWindowHandle());
  }
#endif
}

bool LaufeyHandler::OnBeforePopup(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id,
    const CefString& target_url, const CefString& target_frame_name,
    WindowOpenDisposition target_disposition, bool user_gesture,
    const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
    CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
    CefRefPtr<CefDictionaryValue>& extra_info, bool* no_javascript_access) {
  CEF_REQUIRE_UI_THREAD();
  // `target="_blank"` / `window.open()` aren't seen by the page's Navigation
  // API listener. Cancel the popup and route http(s) destinations to the OS
  // browser; return true to prevent the new browser from being created.
  std::string url = target_url.ToString();
  if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
    LaufeyOpenExternalURL(url);
  }
  return true;
}

bool LaufeyHandler::DoClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  // No close-requested dispatch here: it lives in
  // LaufeyWindowDelegate::CanClose, because Chrome-runtime-style browsers
  // (laufey's default) never receive a DoClose call at all.
  if (browser_list_.size() == 1) {
    is_closing_ = true;
  }
  return false;
}

void LaufeyHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  file_drag_paths_.erase(browser->GetIdentifier());

  for (auto it = browser_list_.begin(); it != browser_list_.end(); ++it) {
    if ((*it)->IsSame(browser)) {
      browser_list_.erase(it);
      break;
    }
  }
  if (browser_list_.empty()) {
    // A tray / menu-bar app keeps running with no window when it asked to
    // (set_quit_on_last_window_closed(false), or an Accessory-policy macOS
    // app, as the WebView backend); quit() ends the loop either way.
#if defined(__APPLE__)
    bool end = laufey_common::ShouldQuitAfterLastWindowMac();
#else
    bool end = laufey_common::ShouldEndLoopAfterLastWindow();
#endif
    if (end)
      LaufeyQuitMainLoop();
  }
}

void LaufeyQuitMainLoop() {
#if defined(__APPLE__)
  // macOS runs [NSApp run] (external_message_pump); stop that instead.
  LaufeyQuitMainLoopMac();
#else
  CefQuitMessageLoop();
#endif
}

void LaufeyHandler::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                  const CefString& title) {
  CEF_REQUIRE_UI_THREAD();
  // Don't let the page's document.title (or the URL, which CEF falls back to
  // when the document has no <title>) clobber a title the embedder set
  // explicitly via the C API.
  auto* loader = RuntimeLoader::GetInstance();
  uint32_t wid = loader ? loader->GetLaufeyIdForBrowser(browser) : 0;
  if (wid > 0 && loader->HasExplicitTitle(wid)) {
    return;
  }
  if (auto browser_view = CefBrowserView::GetForBrowser(browser)) {
    if (auto window = browser_view->GetWindow()) {
      window->SetTitle(title);
    }
  }
}

bool LaufeyHandler::OnDragEnter(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefDragData> dragData,
                                DragOperationsMask /*mask*/) {
  CEF_REQUIRE_UI_THREAD();
  std::vector<std::string> paths;
  if (dragData && dragData->IsFile()) {
    std::vector<CefString> names;
    // GetFilePaths: the full paths (GetFileNames is display names).
    if (dragData->GetFilePaths(names)) {
      for (const CefString& name : names) {
        std::string path = name.ToString();
        if (!path.empty() && paths.size() < LAUFEY_MAX_DROP_PATHS)
          paths.push_back(std::move(path));
      }
    }
  }
  if (paths.empty())
    file_drag_paths_.erase(browser->GetIdentifier());
  else
    file_drag_paths_[browser->GetIdentifier()] = std::move(paths);
  return false;
}

void LaufeyHandler::OnFileDropMessage(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefListValue> args) {
  if (!args || args->GetSize() < 4)
    return;
  int phase = args->GetInt(0);
  double x = args->GetDouble(1);
  double y = args->GetDouble(2);
  int count = args->GetInt(3);
  if (phase < LAUFEY_DRAG_ENTER || phase > LAUFEY_DRAG_DROP || count < 0)
    return;
  int id = browser->GetIdentifier();
  auto it = file_drag_paths_.find(id);
  // Only a drag known to carry files counts: the browser process is where
  // the paths come from, the page only says where the drag is. CEF calls
  // OnDragEnter only for Alloy-style browsers, so for laufey's (Chrome
  // style) the paths are read from the OS's drag data the first time the
  // page reports a drag with files.
  if (it == file_drag_paths_.end() && count > 0 && phase != LAUFEY_DRAG_LEAVE) {
    std::vector<std::string> native = LaufeyNativeDragFilePaths();
    if (!native.empty())
      it = file_drag_paths_.emplace(id, std::move(native)).first;
  }
  if (it == file_drag_paths_.end())
    return;
  uint32_t wid = RuntimeLoader::GetInstance()->GetLaufeyIdForBrowser(browser);
  if (wid == 0)
    return;
  std::vector<std::string> paths = it->second;
  if (phase == LAUFEY_DRAG_LEAVE || phase == LAUFEY_DRAG_DROP)
    file_drag_paths_.erase(it);
  laufey_common::DispatchFileDrop(wid, phase, x, y, paths, paths.size());
}

void LaufeyHandler::OnDraggableRegionsChanged(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    const std::vector<CefDraggableRegion>& regions) {
  CEF_REQUIRE_UI_THREAD();
  // Apply the page's `-webkit-app-region` rectangles to the window so those
  // areas drag the OS window (the rest stays interactive). Lets a web toolbar
  // in the transparent title bar behave like a native one.
  if (auto browser_view = CefBrowserView::GetForBrowser(browser)) {
    if (auto window = browser_view->GetWindow()) {
      window->SetDraggableRegions(regions);
    }
  }
}

// Keyboard mapping lives in backend-common (laufey_common::VkToKey / VkToCode).
// CEF normalizes every platform's key events to Windows VK codes, so the
// same table works here.

bool LaufeyHandler::OnKeyEvent(CefRefPtr<CefBrowser> browser,
                               const CefKeyEvent& event,
                               CefEventHandle os_event) {
  int state;
  if (event.type == KEYEVENT_RAWKEYDOWN || event.type == KEYEVENT_KEYDOWN) {
    state = LAUFEY_KEY_PRESSED;
  } else if (event.type == KEYEVENT_KEYUP) {
    state = LAUFEY_KEY_RELEASED;
  } else {
    return false;
  }

  uint32_t modifiers = 0;
  if (event.modifiers & EVENTFLAG_SHIFT_DOWN)
    modifiers |= LAUFEY_MOD_SHIFT;
  if (event.modifiers & EVENTFLAG_CONTROL_DOWN)
    modifiers |= LAUFEY_MOD_CONTROL;
  if (event.modifiers & EVENTFLAG_ALT_DOWN)
    modifiers |= LAUFEY_MOD_ALT;
  if (event.modifiers & EVENTFLAG_COMMAND_DOWN)
    modifiers |= LAUFEY_MOD_META;

  std::string key = laufey_common::VkToKey(event.windows_key_code,
                                           event.character, false, false);
  std::string code = laufey_common::VkToCode(event.windows_key_code, false, 0);

  uint32_t wid = RuntimeLoader::GetInstance()->GetLaufeyIdForBrowser(browser);
  RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
      wid, state, key.c_str(), code.c_str(), modifiers, false);

  return false;  // Don't consume the event — let CEF handle it too
}

bool LaufeyHandler::OnJSDialog(CefRefPtr<CefBrowser> browser,
                               const CefString& origin_url,
                               JSDialogType dialog_type,
                               const CefString& message_text,
                               const CefString& default_prompt_text,
                               CefRefPtr<CefJSDialogCallback> callback,
                               bool& suppress_message) {
  CEF_REQUIRE_UI_THREAD();
  std::string msg = message_text.ToString();

#ifdef _WIN32
  // The window the dialog is modal to.
  HWND hwnd = nullptr;
  if (auto bv = CefBrowserView::GetForBrowser(browser)) {
    if (auto win = bv->GetWindow()) {
      hwnd = win->GetWindowHandle();
    }
  }
  // ShowDialogWin brackets the modal with a ScopedNativeModalLoop, so CEF's
  // tasks keep running while the dialog is open (this is TID_UI).
  if (dialog_type == JSDialogType::JSDIALOGTYPE_ALERT) {
    laufey_common::ShowDialogWin(LAUFEY_DIALOG_ALERT, "Alert", msg, "", nullptr,
                                 hwnd);
    callback->Continue(true, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_CONFIRM) {
    int confirmed = laufey_common::ShowDialogWin(
        LAUFEY_DIALOG_CONFIRM, "Confirm", msg, "", nullptr, hwnd);
    callback->Continue(confirmed != 0, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_PROMPT) {
    char* input = nullptr;
    int confirmed = laufey_common::ShowDialogWin(
        LAUFEY_DIALOG_PROMPT, "Prompt", msg, default_prompt_text.ToString(),
        &input, hwnd);
    std::string text = input ? input : "";
    free(input);
    callback->Continue(confirmed != 0, text);
    return true;
  }
#elif defined(__APPLE__)
  // macOS: use native NSAlert via helper in runtime_loader_mac.mm. runModal
  // is a nested loop on TID_UI: let CEF's tasks run inside it.
  laufey_common::ScopedNativeModalLoop modal_loop;
  if (dialog_type == JSDialogType::JSDIALOGTYPE_ALERT) {
    ShowNativeJSDialog_Mac(0, msg, "");
    callback->Continue(true, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_CONFIRM) {
    auto result = ShowNativeJSDialog_Mac(1, msg, "");
    callback->Continue(result.confirmed, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_PROMPT) {
    auto result =
        ShowNativeJSDialog_Mac(2, msg, default_prompt_text.ToString());
    callback->Continue(result.confirmed, result.text);
    return true;
  }
#elif defined(__linux__)
  // Linux: use GTK dialogs. gtk_dialog_run is a nested loop on TID_UI: let
  // CEF's tasks run inside it.
  laufey_common::ScopedNativeModalLoop modal_loop;
  if (dialog_type == JSDialogType::JSDIALOGTYPE_ALERT) {
    GtkWidget* dlg =
        gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                               GTK_BUTTONS_OK, "%s", msg.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    callback->Continue(true, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_CONFIRM) {
    GtkWidget* dlg =
        gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_OK_CANCEL, "%s", msg.c_str());
    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    callback->Continue(result == GTK_RESPONSE_OK, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_PROMPT) {
    std::string defaultText = default_prompt_text.ToString();
    GtkWidget* dlg =
        gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_OK_CANCEL, "%s", msg.c_str());
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), defaultText.c_str());
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show(entry);
    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    std::string resultText =
        (result == GTK_RESPONSE_OK) ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
    gtk_widget_destroy(dlg);
    callback->Continue(result == GTK_RESPONSE_OK, resultText);
    return true;
  }
#endif

  return false;
}

bool LaufeyHandler::OnBeforeUnloadDialog(
    CefRefPtr<CefBrowser> browser, const CefString& message_text,
    bool is_reload, CefRefPtr<CefJSDialogCallback> callback) {
  callback->Continue(true, "");
  return true;
}

void LaufeyHandler::CloseAllBrowsers(bool force_close) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::BindOnce(&LaufeyHandler::CloseAllBrowsers, this,
                                       force_close));
    return;
  }
  // App-level quit: mark every window close-allowed so CanClose skips the
  // close-requested negotiation -- quit is deliberately not interceptable
  // by a window's on_close hook. Note CloseBrowser(force_close=true) only
  // skips the beforeunload/unload prompt (see include/cef_browser.h); it
  // does NOT bypass CanClose or DoClose, which is why the marks are needed.
  // is_closing_ doubles as terminate:'s reentry guard against a second quit
  // attempt while the closes are in flight; with the negotiation skipped
  // the quit always completes, so latching it is safe.
  auto* loader = RuntimeLoader::GetInstance();
  if (force_close) {
    is_closing_ = true;
    for (const auto& browser : browser_list_) {
      uint32_t wid = loader->GetLaufeyIdForBrowser(browser);
      if (wid > 0) {
        loader->MarkCloseAllowed(wid);
      }
    }
  }
  for (const auto& browser : browser_list_) {
    browser->GetHost()->CloseBrowser(force_close);
  }
}

bool LaufeyHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    CefProcessId source_process, CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_UI_THREAD();

  const std::string& name = message->GetName().ToString();

  if (name == "laufey_call") {
    // Defense in depth: even though the renderer only installs the bridge in
    // the main frame, a compromised renderer could forge a laufey_call from a
    // sub-frame. Frame provenance is discarded once dispatched to the runtime
    // (only the browser maps to a window id), so drop anything that is not the
    // main frame here.
    if (!frame || !frame->IsMain()) {
      return true;
    }

    CefRefPtr<CefListValue> args = message->GetArgumentList();
    uint64_t call_id = static_cast<uint64_t>(args->GetDouble(0));
    std::string method_path = args->GetString(1).ToString();
    CefRefPtr<CefListValue> callArgs = args->GetList(2);

    // Reserved bridge call from the injected external-link interceptor
    // (laufey_external_links.h): open the URL in the OS browser instead of
    // forwarding to the runtime, then resolve the page-side promise.
    if (method_path == LAUFEY_OPEN_EXTERNAL_METHOD) {
      if (callArgs && callArgs->GetSize() > 0 &&
          callArgs->GetType(0) == VTYPE_STRING) {
        std::string url = callArgs->GetString(0).ToString();
        if (IsAllowedExternalLinkUrl(url)) {
          LaufeyOpenExternalURL(url);
        }
      }
      CefRefPtr<CefProcessMessage> reply =
          CefProcessMessage::Create("laufey_response");
      CefRefPtr<CefListValue> replyArgs = reply->GetArgumentList();
      replyArgs->SetDouble(0, static_cast<double>(call_id));
      replyArgs->SetNull(1);
      replyArgs->SetString(2, "");
      frame->SendProcessMessage(PID_RENDERER, reply);
      return true;
    }

    uint32_t wid = RuntimeLoader::GetInstance()->GetLaufeyIdForBrowser(browser);
    RuntimeLoader::GetInstance()->OnJsCall(wid, call_id, method_path, callArgs);
    return true;
  }

  if (name == "laufey_file_drop") {
    // From the observer the renderer injects into the main frame only; drop
    // anything else, as for laufey_call.
    if (frame && frame->IsMain())
      OnFileDropMessage(browser, message->GetArgumentList());
    return true;
  }

  if (name == "laufey_eval_result") {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    uint64_t eval_id = static_cast<uint64_t>(args->GetDouble(0));
    CefRefPtr<CefValue> result = args->GetValue(1);
    std::string error = args->GetString(2).ToString();
    RuntimeLoader::GetInstance()->HandleEvalResult(eval_id, result, error);
    return true;
  }

  return false;
}

bool LaufeyHandleAlreadyRunningAppRelaunch() {
  std::cerr << "laufey: another launch of this app was ignored (it shares this "
               "instance's web data directory)"
            << std::endl;
  return true;
}

#if !defined(__APPLE__)
namespace {

// Brings the app to the front for a forwarded launch: the first (oldest)
// window that isn't hidden. CEF UI thread. (macOS: ActivateAppMac.)
void ActivateAppCef(void*) {
  for (const CefRefPtr<CefBrowser>& browser :
       RuntimeLoader::GetInstance()->GetAllBrowsers()) {
    CefRefPtr<CefBrowserView> view = CefBrowserView::GetForBrowser(browser);
    CefRefPtr<CefWindow> window = view ? view->GetWindow() : nullptr;
    if (!window || !window->IsVisible())
      continue;
    if (window->IsMinimized())
      window->Restore();
    window->Activate();
    return;
  }
}

}  // namespace
#endif  // !defined(__APPLE__)

void LaufeyInstallSecondInstanceHooks() {
#if defined(__APPLE__)
  // [NSApp run] drains the main dispatch queue (see main_mac.mm).
  laufey_common::InstallSecondInstanceHooksMac();
#else
  laufey_common::SecondInstanceUiHooks hooks;
  hooks.post = [](void*, void (*task)(void*), void* data) {
    CefPostTask(TID_UI, base::BindOnce([](void (*t)(void*), void* d) { t(d); },
                                       task, data));
  };
  hooks.activate = ActivateAppCef;
  laufey_common::SetSecondInstanceUiHooks(hooks);
#endif
}

void LaufeyClearSecondInstanceHooks() {
  laufey_common::SetSecondInstanceUiHooks({});
}

void LaufeyReportCefInitializeFailure(const std::string& root_cache_path) {
  if (CefGetExitCode() == CEF_RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED) {
    std::cerr << "laufey: another instance is already running with the web "
                 "data directory \""
              << root_cache_path << "\"; exiting" << std::endl;
  }
}

void LaufeyApp::OnRegisterCustomSchemes(
    CefRawPtr<CefSchemeRegistrar> registrar) {
  laufey_schemes::RegisterAll(registrar);
}

void LaufeyApp::OnBeforeChildProcessLaunch(
    CefRefPtr<CefCommandLine> command_line) {
  laufey_schemes::ForwardToChild(command_line);
}

// --- DevTools gating (API 40) ---------------------------------------------

namespace {
// The switches that expose a DevTools endpoint on the browser process.
const char* const kRemoteDebuggingSwitches[] = {
    "remote-debugging-port", "remote-debugging-pipe",
    "remote-debugging-address", "remote-debugging-io-pipes",
    "auto-open-devtools-for-tabs"};
}  // namespace

bool LaufeyIsDevToolsCommand(int command_id) {
  switch (command_id) {
    case IDC_DEV_TOOLS:
    case IDC_DEV_TOOLS_CONSOLE:
    case IDC_DEV_TOOLS_DEVICES:
    case IDC_DEV_TOOLS_INSPECT:
    case IDC_DEV_TOOLS_TOGGLE:
    case IDC_CONTENT_CONTEXT_INSPECTELEMENT:
    case IDC_CONTENT_CONTEXT_INSPECTBACKGROUNDPAGE:
    case IDC_CONTENT_CONTEXT_INSPECTELEMENT_WITH_DEVTOOLS:
    case IDC_CONTENT_CONTEXT_INSPECTELEMENT_WITH_GEMINI:
      return true;
  }
  return false;
}

void LaufeyApplyInspectableToCommandLine(
    CefRefPtr<CefCommandLine> command_line) {
  if (laufey_common::LaunchInspectable())
    return;
  for (const char* sw : kRemoteDebuggingSwitches) {
    if (command_line->HasSwitch(sw))
      command_line->RemoveSwitch(sw);
  }
}

void LaufeyStripDeepLinkSwitches(CefRefPtr<CefCommandLine> command_line) {
  const std::vector<std::string> strip =
      laufey_common::DeepLinkSwitchesToStrip(laufey_common::ProcessArgs(), {});
  for (const std::string& name : strip) {
    if (command_line->HasSwitch(name)) {
      std::cerr << "laufey: ignoring --" << name
                << " on the command line of a deep-link launch" << std::endl;
      command_line->RemoveSwitch(name);
    }
  }
}

bool LaufeyDevToolsReachable() {
  if (laufey_common::LaunchInspectable())
    return true;
  // Off: reachable only if a remote-debugging switch got through anyway.
  CefRefPtr<CefCommandLine> cl = CefCommandLine::GetGlobalCommandLine();
  if (!cl)
    return false;
  for (const char* sw : kRemoteDebuggingSwitches) {
    if (cl->HasSwitch(sw))
      return true;
  }
  return false;
}

bool LaufeyHandler::OnShowPermissionPrompt(
    CefRefPtr<CefBrowser> /*browser*/, uint64_t /*prompt_id*/,
    const CefString& requesting_origin, uint32_t requested_permissions,
    CefRefPtr<CefPermissionPromptCallback> callback) {
  // Chromium's Local Network Access prompt (CEF 136 named it
  // LOCAL_NETWORK_ACCESS, CEF 145 split it into LOCAL_NETWORK and
  // LOOPBACK_NETWORK). The CEF host has no prompt UI, so an unanswered one
  // holds the page's request forever.
  constexpr uint32_t kLocalNetwork = CEF_PERMISSION_TYPE_LOCAL_NETWORK_ACCESS |
                                     CEF_PERMISSION_TYPE_LOCAL_NETWORK |
                                     CEF_PERMISSION_TYPE_LOOPBACK_NETWORK;
  switch (laufey_common::DecideLocalNetworkPrompt(
      requesting_origin.ToString(), requested_permissions, kLocalNetwork,
      laufey_schemes::Declared())) {
    case laufey_common::LocalNetworkPromptDecision::kAccept:
      callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
      return true;
    case laufey_common::LocalNetworkPromptDecision::kDeny:
      callback->Continue(CEF_PERMISSION_RESULT_DENY);
      return true;
    case laufey_common::LocalNetworkPromptDecision::kDefault:
      break;
  }
  return false;
}

bool LaufeyHandler::OnChromeCommand(
    CefRefPtr<CefBrowser> /*browser*/, int command_id,
    cef_window_open_disposition_t /*disposition*/) {
  // Handled (= dropped) only while DevTools are off.
  return !laufey_common::LaunchInspectable() &&
         LaufeyIsDevToolsCommand(command_id);
}

void LaufeyHandler::OnBeforeContextMenu(
    CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/,
    CefRefPtr<CefContextMenuParams> /*params*/, CefRefPtr<CefMenuModel> model) {
  if (laufey_common::LaunchInspectable() || !model)
    return;
  for (size_t i = model->GetCount(); i > 0; i--) {
    int id = model->GetCommandIdAt(i - 1);
    if (LaufeyIsDevToolsCommand(id))
      model->RemoveAt(i - 1);
  }
  // Drop a separator left dangling at the end.
  size_t count = model->GetCount();
  if (count > 0 && model->GetTypeAt(count - 1) == MENUITEMTYPE_SEPARATOR)
    model->RemoveAt(count - 1);
}

bool LaufeyHandler::OnContextMenuCommand(
    CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/,
    CefRefPtr<CefContextMenuParams> /*params*/, int command_id,
    EventFlags /*event_flags*/) {
  return !laufey_common::LaunchInspectable() &&
         LaufeyIsDevToolsCommand(command_id);
}

void LaufeyApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  // Create the handler and keep it alive for the lifetime of the app.
  // Backend_CreateWindow uses LaufeyHandler::GetInstance() from the runtime
  // thread, so the handler must outlive this function scope.
  static CefRefPtr<LaufeyHandler> handler(new LaufeyHandler());

  if (!g_runtime_path.empty()) {
    if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
      std::cerr << "Failed to load runtime, exiting" << std::endl;
      CefQuitMessageLoop();
      return;
    }
    // Defer Start() to the next message loop iteration. OnContextInitialized
    // runs during CefInitialize(), before CefRunMessageLoop() has started.
    // The runtime thread's Backend_CreateWindow posts CefPostTasks to the UI
    // thread and blocks until they complete — this deadlocks if the message
    // loop isn't running yet.
    CefPostTask(TID_UI, base::BindOnce(
                            []() { RuntimeLoader::GetInstance()->Start(); }));
  } else {
    // No runtime: create a default window for demo
    uint32_t laufey_id = RuntimeLoader::GetInstance()->AllocateWindowId();
    g_pending_laufey_ids.push(laufey_id);
    CefBrowserSettings browser_settings;
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
        handler, "https://example.com", browser_settings, nullptr, nullptr,
        nullptr);
    CefWindow::CreateTopLevelWindow(
        new LaufeyWindowDelegate(browser_view, laufey_id));
  }
}
