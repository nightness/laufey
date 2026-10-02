// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_APP_H_
#define LAUFEY_APP_H_

#include <cctype>
#include <cstdlib>
#include <list>
#include <map>
#include <queue>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_command_handler.h"
#include "include/cef_command_line.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_jsdialog_handler.h"
#include "include/cef_permission_handler.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

extern std::string g_runtime_path;

// Default CEF log severity for CefSettings.log_severity.
//
// Chromium continuously logs ERROR/WARNING lines that are irrelevant to an
// embedded webview app and not actionable by the app developer: GCM
// registration failures (`registration_request.cc ... PHONE_REGISTRATION_ERROR`
// / `DEPRECATED_ENDPOINT`), on-device model service disconnects
// (`on_device_model/...`), xdg desktop-portal "Request cancelled by user",
// long-running `CompositorAnimationObserver` warnings, "Unable to get gpu
// adapter", etc. Targeted `--disable-*` switches can't cover all of them (the
// portal and compositor messages aren't feature-gated), so raise the log floor
// to FATAL by default to hide the noise — matching what Electron production
// apps do. Set LAUFEY_CEF_LOG_SEVERITY to restore output while debugging:
// verbose | debug | info | warning | error | fatal | disable | default.
inline cef_log_severity_t LaufeyCefLogSeverity() {
  const char* env = getenv("LAUFEY_CEF_LOG_SEVERITY");
  if (!env || !*env) {
    return LOGSEVERITY_FATAL;
  }
  std::string v(env);
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (v == "verbose")
    return LOGSEVERITY_VERBOSE;
  if (v == "debug")
    return LOGSEVERITY_DEBUG;
  if (v == "info")
    return LOGSEVERITY_INFO;
  if (v == "warning")
    return LOGSEVERITY_WARNING;
  if (v == "error")
    return LOGSEVERITY_ERROR;
  if (v == "fatal")
    return LOGSEVERITY_FATAL;
  if (v == "disable")
    return LOGSEVERITY_DISABLE;
  if (v == "default")
    return LOGSEVERITY_DEFAULT;
  return LOGSEVERITY_FATAL;
}

// Wayland app_id / X11 WM_CLASS for the app's windows. Read at startup from
// LAUFEY_APP_ID or the launch file's "appId" (laufey_launch_config.h),
// falling back to LAUFEY_APP_NAME. Empty leaves the
// CEF/Chromium default (the backend binary name). On Wayland the compositor
// keys the taskbar/overview icon off this app_id matching an installed
// `<app_id>.desktop`, so it must equal the desktop file's id for the icon to
// show.
extern std::string g_app_id;

// With a persistent profile (LAUFEY_DATA_DIR / LAUFEY_APP_ID), CEF's process
// singleton allows one instance per root_cache_path: a second launch hands its
// arguments to the running instance and exits from CefInitialize.
//
// Called from each platform's OnAlreadyRunningAppRelaunch in the running
// instance. Returns true (handled) so CEF doesn't open a default Chrome-style
// window; the relaunch is otherwise ignored. It is only a fallback: with
// single-instance mode on (docs/deep-links.md) a second launch forwards its
// arguments and exits in main() before CefInitialize, so CEF's own singleton
// never sees it.
bool LaufeyHandleAlreadyRunningAppRelaunch();

// Single-instance delivery (laufey_single_instance.h): installs the UI hooks
// that post forwarded launches to the CEF UI thread and bring the app's
// first visible window to the front. Call after CefInitialize succeeded; call
// LaufeyClearSecondInstanceHooks before CefShutdown.
void LaufeyInstallSecondInstanceHooks();
void LaufeyClearSecondInstanceHooks();

// Called in the launching process when CefInitialize fails; explains the
// "another instance owns this profile" case on stderr.
void LaufeyReportCefInitializeFailure(const std::string& root_cache_path);

// Open `url` in the user's default OS browser. Implemented per platform in
// main_mac.mm / main_windows.cc / main_linux.cc. Used to honor the external
// link redirect policy (laufey_external_links.h).
void LaufeyOpenExternalURL(const std::string& url);

// Queue of laufey window IDs waiting for OnAfterCreated to fire.
// Push before CreateBrowserView, pop in OnAfterCreated.
// Both happen on the UI thread so no synchronization needed.
extern std::queue<uint32_t> g_pending_laufey_ids;

class LaufeyWindowDelegate : public CefWindowDelegate {
 public:
  LaufeyWindowDelegate(CefRefPtr<CefBrowserView> browser_view,
                       uint32_t laufey_id, uint32_t flags = 0)
      : browser_view_(browser_view), laufey_id_(laufey_id), flags_(flags) {}

  void OnWindowCreated(CefRefPtr<CefWindow> window) override;
  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override;
  bool CanClose(CefRefPtr<CefWindow> window) override;
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override;
  // Size constraints (API 38): what set_window_size_constraints stored.
  CefSize GetMinimumSize(CefRefPtr<CefView> view) override;
  CefSize GetMaximumSize(CefRefPtr<CefView> view) override;
  // Window-state events (API 38): both can follow a maximize, minimize,
  // restore or fullscreen change.
  void OnWindowBoundsChanged(CefRefPtr<CefWindow> window,
                             const CefRect& new_bounds) override;
  void OnWindowFullscreenTransition(CefRefPtr<CefWindow> window,
                                    bool is_completed) override;

  // Frameless windows (LAUFEY_WINDOW_FLAG_FRAMELESS) drop the title bar and
  // standard window buttons.
  bool IsFrameless(CefRefPtr<CefWindow> window) override;
  // Non-activating panels (LAUFEY_WINDOW_FLAG_NO_ACTIVATE) accept the first
  // click without the app having to activate first, so a tray popover can be
  // interacted with while the previously-focused app keeps focus.
  cef_state_t AcceptsFirstMouse(CefRefPtr<CefWindow> window) override;

#if defined(_WIN32) || defined(__linux__)
  // The app menu's accelerators (API 41; views_menu.h).
  bool OnAccelerator(CefRefPtr<CefWindow> window, int command_id) override;
#endif

#if defined(__linux__)
  // CEF Views reports activation on both X11 and Wayland. Use this instead of
  // the X11-only XI2 monitor so focus events work with either Ozone backend.
  void OnWindowActivationChanged(CefRefPtr<CefWindow> window,
                                 bool active) override;

  // Advertise the Wayland app_id / X11 WM_CLASS (from g_app_id) so window
  // managers can attribute the right `.desktop` file — and therefore the right
  // taskbar/overview icon — to our windows. Without this the app_id defaults to
  // the backend binary name and Wayland shows a generic placeholder icon.
  bool GetLinuxWindowProperties(CefRefPtr<CefWindow> window,
                                CefLinuxWindowProperties& properties) override;
#endif

 private:
  CefRefPtr<CefBrowserView> browser_view_;
  uint32_t laufey_id_ = 0;
  uint32_t flags_ = 0;
  IMPLEMENT_REFCOUNTING(LaufeyWindowDelegate);
};

// DevTools gating (API 40; app.cc). With LAUFEY_INSPECTABLE=0 /
// "inspectable": false, DevTools are off for the whole process:
//   - the remote-debugging switches are stripped from the browser process's
//     command line (and settings.remote_debugging_port is never set);
//   - LaufeyHandler swallows Chrome's DevTools commands (F12,
//     Ctrl/Cmd+Shift+I / J / C, Cmd+Option+I / J / C) and drops the context
//     menu's Inspect items;
//   - open_devtools is a no-op.
// Chromium's "devtools.availability" preference is NOT used: it also refuses
// the in-process DevTools protocol client that print_to_pdf drives
// (ExecuteDevToolsMethod), so PDFs would break.
void LaufeyApplyInspectableToCommandLine(
    CefRefPtr<CefCommandLine> command_line);
// A deep-link launch (a URL among the process's positional arguments, see
// laufey_launch_args.h) drops every Chromium switch its own command line
// carried before "--": an allow-list that is empty, since a link-started app
// needs none. Registrations run `"<exe>" -- "%1"`, which Chromium's parser
// already stops at; this covers an older registration without the "--".
// Uses the arguments the host recorded with laufey_common::SetProcessArgs (a
// no-op where it recorded none). Browser process only.
void LaufeyStripDeepLinkSwitches(CefRefPtr<CefCommandLine> command_line);
// Whether DevTools can be reached in this process: inspectable, or a
// remote-debugging switch present on the browser process's command line
// despite it (read back from CEF's global command line). UI thread.
bool LaufeyDevToolsReachable();
// Whether a command id is one of Chrome's DevTools / Inspect commands.
bool LaufeyIsDevToolsCommand(int command_id);

// The file paths of the external drag over a window, read from the OS's own
// drag data: on X11 the XDND source's text/uri-list (XdndSelection,
// drag_paths_linux.cc), on Windows the OLE drag's CF_HDROP
// (drag_paths_win.cc), on macOS the drag pasteboard (drag_paths_mac.mm).
// Empty when no files are being dragged, or where there is no such source
// to ask (Wayland). Needed because laufey's browsers are of the Chrome
// runtime style, and CEF calls CefDragHandler::OnDragEnter only for
// Alloy-style ones, so that hook never sees the drag. UI thread; on X11 it
// may wait up to a second for the source.
std::vector<std::string> LaufeyNativeDragFilePaths();

#if defined(_WIN32)
// Windows: wraps the drop target Chromium registered on a CEF window, so the
// files of an OLE drag over it are recorded for LaufeyNativeDragFilePaths
// (cef/src/drag_paths_win.cc). Once per window; UI thread.
void LaufeyHookWindowDropTarget(HWND hwnd);
#endif

class LaufeyHandler : public CefClient,
                      public CefLifeSpanHandler,
                      public CefDisplayHandler,
                      public CefKeyboardHandler,
                      public CefDragHandler,
                      public CefJSDialogHandler,
                      public CefCommandHandler,
                      public CefContextMenuHandler,
                      public CefPermissionHandler {
 public:
  LaufeyHandler();
  ~LaufeyHandler() override;

  static LaufeyHandler* GetInstance();

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
    return this;
  }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
    return this;
  }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override {
    return this;
  }
  CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override {
    return this;
  }
  CefRefPtr<CefDragHandler> GetDragHandler() override {
    return this;
  }
  CefRefPtr<CefCommandHandler> GetCommandHandler() override {
    return this;
  }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
    return this;
  }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
    return this;
  }

  // Local Network Access: the embedder's own origins (its declared custom
  // schemes and "app") may reach loopback / private addresses; everyone
  // else is denied. See LocalNetworkPromptDecision in
  // laufey_scheme_registry.h. Other prompts keep the default handling.
  bool OnShowPermissionPrompt(
      CefRefPtr<CefBrowser> browser, uint64_t prompt_id,
      const CefString& requesting_origin, uint32_t requested_permissions,
      CefRefPtr<CefPermissionPromptCallback> callback) override;

  // DevTools off (API 40, LAUFEY_INSPECTABLE=0): Chrome's DevTools commands
  // (F12, Ctrl/Cmd+Shift+I / J / C, the app menu) are swallowed and the
  // context menu loses its Inspect items. See LaufeyApplyInspectable* below.
  bool OnChromeCommand(CefRefPtr<CefBrowser> browser, int command_id,
                       cef_window_open_disposition_t disposition) override;
  void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefContextMenuParams> params,
                           CefRefPtr<CefMenuModel> model) override;
  bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefContextMenuParams> params,
                            int command_id, EventFlags event_flags) override;

  // A drag entering a browser (API 39 file drops): remembers the dragged
  // files' native paths for that browser; the page's observer then reports
  // where the drag goes and the drop (see OnProcessMessageReceived). Never
  // cancels the drag.
  bool OnDragEnter(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefDragData> dragData,
                   DragOperationsMask mask) override;

  // Forward the page's `-webkit-app-region: drag` rectangles to the window so
  // those areas drag the OS window (used by the transparent-titlebar layout).
  void OnDraggableRegionsChanged(
      CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
      const std::vector<CefDraggableRegion>& regions) override;

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  // `target="_blank"` / `window.open()` requests aren't seen by the page's
  // Navigation API listener; cancel the popup and open http(s) destinations in
  // the OS browser instead.
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                     int popup_id, const CefString& target_url,
                     const CefString& target_frame_name,
                     WindowOpenDisposition target_disposition,
                     bool user_gesture, const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override;

  bool OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event,
                  CefEventHandle os_event) override;

  bool OnJSDialog(CefRefPtr<CefBrowser> browser, const CefString& origin_url,
                  JSDialogType dialog_type, const CefString& message_text,
                  const CefString& default_prompt_text,
                  CefRefPtr<CefJSDialogCallback> callback,
                  bool& suppress_message) override;

  bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser> browser,
                            const CefString& message_text, bool is_reload,
                            CefRefPtr<CefJSDialogCallback> callback) override;

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  void CloseAllBrowsers(bool force_close);
  bool IsClosing() const {
    return is_closing_;
  }

 private:
  std::list<CefRefPtr<CefBrowser>> browser_list_;
  bool is_closing_ = false;
  // The file paths of the drag currently over each browser (by browser id),
  // from OnDragEnter. UI thread only.
  std::map<int, std::vector<std::string>> file_drag_paths_;
  // Handles a "laufey_file_drop" message from the page observer.
  void OnFileDropMessage(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefListValue> args);

  IMPLEMENT_REFCOUNTING(LaufeyHandler);
};

class LaufeyApp : public CefApp, public CefBrowserProcessHandler {
 public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    command_line->AppendSwitch("use-mock-keychain");

    // Silence Chromium's background networking. The GCM (Google Cloud
    // Messaging) client tries to register on startup and logs noisy
    // `registration_request.cc ... PHONE_REGISTRATION_ERROR` /
    // `DEPRECATED_ENDPOINT` errors that have nothing to do with the app. A
    // webview-embedding desktop app doesn't use GCM, the component updater,
    // safebrowsing auto-update, etc., so disable the lot (matches what
    // Electron/Puppeteer do). Only the browser process needs the switch; CEF
    // propagates it to subprocesses.
    if (process_type.empty()) {
      LaufeyStripDeepLinkSwitches(command_line);
      command_line->AppendSwitch("disable-background-networking");
      LaufeyApplyInspectableToCommandLine(command_line);
    }
  }

  void OnContextInitialized() override;

  bool OnAlreadyRunningAppRelaunch(
      CefRefPtr<CefCommandLine> command_line,
      const CefString& current_directory) override {
    return LaufeyHandleAlreadyRunningAppRelaunch();
  }

  // Register the custom "app" scheme plus every scheme declared with
  // --laufey-custom-schemes / LAUFEY_CUSTOM_SCHEMES (standard, secure,
  // fetch/CORS-enabled) so the in-process scheme handler can serve pages over
  // them like an https origin. Called early in every process; see
  // custom_schemes.h for why the list must come from the command line.
  void OnRegisterCustomSchemes(
      CefRawPtr<CefSchemeRegistrar> registrar) override;

  // Forward the declared custom schemes to renderer / utility processes so
  // they register the same set.
  void OnBeforeChildProcessLaunch(
      CefRefPtr<CefCommandLine> command_line) override;

#if defined(__APPLE__)
  // Drive CefDoMessageLoopWork from the main run loop (external_message_pump)
  // so the libdispatch main queue keeps draining — tray/status-item creation
  // and other dispatch_async(main_queue) work would otherwise never run under
  // CEF's own message loop.
  void OnScheduleMessagePumpWork(int64_t delay_ms) override;
#endif

 private:
  IMPLEMENT_REFCOUNTING(LaufeyApp);
};

#if defined(__APPLE__)
// Stop the [NSApp run] main loop (replaces CefQuitMessageLoop on macOS).
// Implemented in main_mac.mm.
void LaufeyQuitMainLoopMac();
#endif

// Ends the CEF backend's main loop on every platform (LaufeyQuitMainLoopMac on
// macOS, CefQuitMessageLoop elsewhere). UI thread.
void LaufeyQuitMainLoop();

// Window-state bookkeeping (API 38; runtime_loader.cc). Recheck reads the
// window's state back from CefWindow on the UI thread and reports a change;
// Schedule does that now and a few times over the next ~1.5 s, from any
// thread.
void CefRecheckWindowState(uint32_t window_id);
void CefScheduleWindowStateRecheck(uint32_t window_id);

#endif
