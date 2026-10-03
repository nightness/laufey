// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonDigest.h>
#import <UserNotifications/UserNotifications.h>
#import <WebKit/WebKit.h>

#include "runtime_loader.h"

#import <objc/message.h>
#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_single_instance.h"
#include "laufey_json.h"
#include "laufey_launch_config.h"
#include "laufey_menu.h"
#include "laufey_notifications.h"
#include "laufey_passkey.h"
#include "laufey_auth_session.h"
#include "laufey_scheme_cancel.h"
#include "laufey_scheme_registry.h"
#include "laufey_ui_tasks.h"
#include "laufey_system.h"
#include "laufey_window.h"
#include "init_script.h"

#include <atomic>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

@class LaufeyScriptMessageHandler;
@class LaufeyWindowDelegate;
@class LaufeyUIDelegate;
@class LaufeyNavigationDelegate;

// Defined in main_mac.mm — appends a default Edit submenu (Cut/Copy/Paste/
// Select All/Undo/Redo) to the given menubar if no submenu in the tree
// already exposes -copy:. Cmd+C/V on macOS dispatch via the main menu, so
// this has to be present for them to work at all.
extern void EnsureEditMenu(NSMenu* menubar);

// Per-window state
struct MacWindowState {
  uint32_t window_id;
  NSWindow* window;
  WKWebView* webview;
  LaufeyScriptMessageHandler* message_handler;
  LaufeyWindowDelegate* window_delegate;
  id focus_observer;
  id blur_observer;
  id resize_observer;
  id move_observer;
  NSMenu* menu = nil;  // per-window menu (nil = no custom menu)
  LaufeyUIDelegate* ui_delegate;
  LaufeyNavigationDelegate* navigation_delegate;
  id title_observer = nil;  // KVO observer mirroring document.title
  // While click passthrough is active, keep feeding this window's mouse
  // events from the global (other-app) monitors.
  bool click_passthrough_forward = false;
};

class WKWebViewBackend : public LaufeyBackend {
 public:
  WKWebViewBackend();
  ~WKWebViewBackend() override;

  void CreateWindow(uint32_t window_id, int width, int height) override;
  void CreateWindowEx(uint32_t window_id, int width, int height,
                      uint32_t flags) override;
  void CloseWindow(uint32_t window_id) override;

  void Navigate(uint32_t window_id, const std::string& url) override;
  // Record a scheme the embedder registered; every window created afterwards
  // installs the in-process URL scheme handler for it, next to "app".
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
  void SetClickPassthroughForward(uint32_t window_id, bool forward) override;
  bool IsClickPassthroughForward(uint32_t window_id) override;
  bool IsVisible(uint32_t window_id) override;
  void Show(uint32_t window_id) override;
  void Hide(uint32_t window_id) override;
  void Focus(uint32_t window_id) override;
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

  int ShowDialog(uint32_t window_id, int dialog_type, const std::string& title,
                 const std::string& message, const std::string& default_value,
                 char** out_input_value) override;

  char* ReadClipboardText() override {
    return laufey_common::ClipboardReadTextMac();
  }
  void WriteClipboardText(const std::string& text) override {
    laufey_common::ClipboardWriteTextMac(text);
  }

  void SetDockBadge(const char* badge_or_null) override;
  void BounceDock(int type) override;
  void SetDockMenu(laufey_value_t* menu_template,
                   const laufey_backend_api_t* api,
                   laufey_menu_click_fn on_click, void* on_click_data) override;
  void SetDockVisible(bool visible) override;
  void SetDockReopenHandler(laufey_dock_reopen_fn handler,
                            void* user_data) override;

  void SetOpenUrlHandler(laufey_open_url_fn handler, void* user_data) override;
  bool TestTriggerOpenUrl(const char* url) override;

  uint32_t PasskeyCapabilities() override {
    return laufey_common::PasskeyCapabilitiesMac();
  }
  void PasskeyRequest(uint32_t window_id, uint32_t kind,
                      const char* options_json,
                      laufey_passkey_result_fn callback,
                      void* user_data) override;

  uint32_t AuthSessionCapabilities() override {
    return laufey_common::AuthSessionCapabilities();
  }
  void AuthSessionStart(uint32_t window_id, const char* url,
                        const char* callback, uint32_t flags,
                        laufey_auth_session_result_fn on_result,
                        void* user_data) override;

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

  void QueryPermission(int kind, laufey_permission_callback_fn cb,
                       void* user_data) override;
  void RequestPermission(int kind, laufey_permission_callback_fn cb,
                         void* user_data) override;

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
           LAUFEY_MENU_CAP_ICONS | LAUFEY_MENU_CAP_TOOLTIPS;
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
                                  const char* accelerator) override {
    return laufey_common::TestTriggerMenuAcceleratorMac(window_id, accelerator);
  }

  // Drag and drop, file dialogs, rich clipboard (API >= 39).
  void SetFileDropHandler(laufey_file_drop_fn handler,
                          void* user_data) override {
    laufey_common::SetFileDropHandler(handler, user_data);
  }
  bool TestTriggerFileDrop(uint32_t window_id, int phase, double x, double y,
                           const char* const* paths, size_t count) override;
  void StartFileDrag(uint32_t window_id, const char* const* paths, size_t count,
                     const uint8_t* icon_png, size_t icon_len,
                     laufey_drag_result_fn callback, void* user_data) override;
  uint32_t ShowFileDialog(uint32_t window_id,
                          const laufey_file_dialog_options_t* options,
                          laufey_file_dialog_result_fn callback,
                          void* user_data) override;
  bool CancelFileDialog(uint32_t dialog_id) override {
    return laufey_common::CancelFileDialogMac(dialog_id);
  }
  bool TestFileDialogRespond(int action, const char* path) override {
    return laufey_common::TestFileDialogRespondMac(action, path);
  }
  uint32_t ClipboardCapabilities() override {
    return laufey_common::ClipboardCapabilitiesMac();
  }
  char* ReadClipboardHtml() override {
    return laufey_common::ClipboardReadHtmlMac();
  }
  bool WriteClipboardHtml(const std::string& html,
                          const char* text_or_null) override {
    return laufey_common::ClipboardWriteHtmlMac(html, text_or_null);
  }
  uint8_t* ReadClipboardImage(size_t* len_out) override {
    return laufey_common::ClipboardReadImageMac(len_out);
  }
  bool WriteClipboardImage(const uint8_t* png, size_t len) override {
    return laufey_common::ClipboardWriteImageMac(png, len);
  }
  char* ReadClipboardFormats() override {
    return laufey_common::ClipboardReadFormatsMac();
  }
  void SetClipboardChangeHandler(laufey_clipboard_change_fn handler,
                                 void* user_data) override {
    laufey_common::SetClipboardChangeHandler(handler, user_data);
  }

  // Global shortcuts, launch at login, DevTools (API >= 40).
  // The Carbon hot-key platform is installed on first use.
  static void EnsureShortcuts() {
    static std::once_flag once;
    std::call_once(once, [] {
      laufey_common::InstallShortcutPlatform(
          laufey_common::CreateShortcutPlatformMac());
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

  // Window state, constraints, screens and chrome (API >= 38).
  uint32_t WindowCapabilities() override;
  void SetWindowState(uint32_t window_id, int action) override;
  uint32_t GetWindowState(uint32_t window_id) override;
  void SetWindowStateHandler(laufey_window_state_fn handler,
                             void* user_data) override;
  void SetWindowSizeConstraints(uint32_t window_id, int min_width,
                                int min_height, int max_width,
                                int max_height) override;
  void GetWindowSizeConstraints(uint32_t window_id, int* min_width,
                                int* min_height, int* max_width,
                                int* max_height) override;
  size_t GetScreens(laufey_screen_t* out, size_t capacity) override;
  int64_t GetWindowScreen(uint32_t window_id) override;
  void SetDisplayChangedHandler(laufey_display_changed_fn handler,
                                void* user_data) override;
  bool SetWindowTitlebarStyle(uint32_t window_id, int style) override;
  bool SetWindowTrafficLightPosition(uint32_t window_id, int x, int y) override;
  bool SetWindowBackdrop(uint32_t window_id, int backdrop,
                         int material) override;
  bool GetWindowNormalBounds(uint32_t window_id, int* x, int* y, int* width,
                             int* height) override;
  void SetQuitOnLastWindowClosed(bool quit) override;

  void HandleJsMessage(uint32_t window_id, uint64_t call_id,
                       const std::string& method, laufey::ValuePtr args);

  // Called from the window delegate's windowWillClose: when AppKit closes
  // the NSWindow directly (windowShouldClose: returned YES because no
  // close-requested handler deferred). CloseWindow() never reaches this —
  // it detaches the delegate (via RemoveWindowState) before [window close].
  void OnWindowClosedByUser(uint32_t window_id);

 private:
  MacWindowState* GetWindow(uint32_t window_id);
  WKWebView* WebViewOf(uint32_t window_id);
  void RemoveWindowState(uint32_t window_id);
  void InstallGlobalMonitors();
  void RemoveGlobalMonitors();
  // Install / remove the other-app (forwarding) monitors depending on
  // whether any window currently wants click-passthrough forwarding.
  // Main thread only.
  void UpdateForwardMonitors();
  // Frontmost visible window with passthrough + forwarding active whose
  // frame contains `screen_point`, or 0. Fills `out_win` on a hit.
  // Main thread only.
  uint32_t ForwardTargetAt(NSPoint screen_point, NSWindow** out_win);

  std::map<uint32_t, MacWindowState> windows_;
  std::mutex windows_mutex_;

  // Set once the first WKWebViewConfiguration has been built, i.e. the
  // registered-scheme set has been read; a later RegisterSchemeHandler warns.
  std::atomic<bool> any_window_created_{false};

  // Global event monitors (installed once)
  id keyboard_monitor_ = nil;
  id mouse_monitor_ = nil;
  id mouse_move_monitor_ = nil;
  id scroll_monitor_ = nil;
  bool monitors_installed_ = false;

  // Forwarding monitors: NSEvent *global* monitors that observe mouse events
  // delivered to OTHER applications — which is what a passthrough window's
  // events become. Installed only while at least one window has forwarding
  // enabled, removed when the last one turns it off.
  id forward_mouse_monitor_ = nil;
  id forward_mouse_move_monitor_ = nil;
  id forward_scroll_monitor_ = nil;
};

// NSWindow → laufey_id mapping for event routing
static std::map<void*, uint32_t> g_nswindow_to_laufey_id;
static std::mutex g_nswindow_mutex;

static uint32_t LaufeyIdForNSWindow(NSWindow* win) {
  if (!win)
    return 0;
  std::lock_guard<std::mutex> lock(g_nswindow_mutex);
  auto it = g_nswindow_to_laufey_id.find((__bridge void*)win);
  return it != g_nswindow_to_laufey_id.end() ? it->second : 0;
}

static void RegisterNSWindow(NSWindow* win, uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_nswindow_mutex);
  g_nswindow_to_laufey_id[(__bridge void*)win] = window_id;
}

static void UnregisterNSWindow(NSWindow* win) {
  std::lock_guard<std::mutex> lock(g_nswindow_mutex);
  g_nswindow_to_laufey_id.erase((__bridge void*)win);
}

// A borderless NSWindow returns NO from
// -canBecomeKeyWindow/-canBecomeMainWindow by default, so a frameless window
// never takes key focus and its WKWebView never becomes first responder —
// killing all keyboard and mouse input. This subclass forces both to YES so
// frameless windows behave like normal ones.
@interface LaufeyKeyableWindow : NSWindow
@end

@implementation LaufeyKeyableWindow
- (BOOL)canBecomeKeyWindow {
  return YES;
}
- (BOOL)canBecomeMainWindow {
  return YES;
}
@end

// The WKWebView every window uses. It is the window's drag destination, so it
// is where file drags are seen: each phase goes to the file-drop handler with
// the native paths (from the dragging pasteboard's file URLs) before WebKit
// handles it, so the page keeps getting its own DOM drag events. A file drag
// is always accepted (copy), even where the page doesn't handle it, so the
// drop reaches the runtime; WebKit's answer still decides what the page does.
@interface LaufeyWebView : WKWebView
@property(nonatomic, assign) uint32_t laufeyWindowId;
@end

static std::vector<std::string> DraggedFilePaths(id<NSDraggingInfo> info) {
  std::vector<std::string> paths;
  NSArray<NSURL*>* urls = [[info draggingPasteboard]
      readObjectsForClasses:@[ [NSURL class] ]
                    options:@{
                      NSPasteboardURLReadingFileURLsOnlyKey : @YES
                    }];
  for (NSURL* url in urls) {
    if (url.isFileURL && url.path.length > 0)
      paths.emplace_back(url.path.UTF8String);
  }
  return paths;
}

@implementation LaufeyWebView {
  std::vector<std::string> _dragPaths;  // the current file drag's paths
  bool _fileDrag;
  NSDragOperation _webkitOp;  // WebKit's answer to the last enter / update
  NSPoint _lastPoint;
}

- (NSPoint)laufeyDragPoint:(id<NSDraggingInfo>)info {
  NSPoint p = [self convertPoint:[info draggingLocation] fromView:nil];
  if (!self.isFlipped)
    p.y = NSHeight(self.bounds) - p.y;
  return p;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
  NSDragOperation op = [super draggingEntered:info];
  _webkitOp = op;
  _dragPaths = DraggedFilePaths(info);
  _fileDrag = !_dragPaths.empty();
  if (!_fileDrag)
    return op;
  _lastPoint = [self laufeyDragPoint:info];
  laufey_common::DispatchFileDrop(self.laufeyWindowId, LAUFEY_DRAG_ENTER,
                                  _lastPoint.x, _lastPoint.y, _dragPaths,
                                  _dragPaths.size());
  return op == NSDragOperationNone ? NSDragOperationCopy : op;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
  NSDragOperation op = [super draggingUpdated:info];
  _webkitOp = op;
  if (!_fileDrag)
    return op;
  NSPoint p = [self laufeyDragPoint:info];
  if (!NSEqualPoints(p, _lastPoint)) {
    _lastPoint = p;
    laufey_common::DispatchFileDrop(self.laufeyWindowId, LAUFEY_DRAG_OVER, p.x,
                                    p.y, _dragPaths, _dragPaths.size());
  }
  return op == NSDragOperationNone ? NSDragOperationCopy : op;
}

- (void)draggingExited:(id<NSDraggingInfo>)info {
  [super draggingExited:info];
  if (!_fileDrag)
    return;
  _fileDrag = false;
  _dragPaths.clear();
  NSPoint p = [self laufeyDragPoint:info];
  laufey_common::DispatchFileDrop(self.laufeyWindowId, LAUFEY_DRAG_LEAVE, p.x,
                                  p.y, {}, 0);
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
  if (!_fileDrag)
    return [super performDragOperation:info];
  std::vector<std::string> paths = std::move(_dragPaths);
  _dragPaths.clear();
  _fileDrag = false;
  NSPoint p = [self laufeyDragPoint:info];
  laufey_common::DispatchFileDrop(self.laufeyWindowId, LAUFEY_DRAG_DROP, p.x,
                                  p.y, paths, paths.size());
  if (_webkitOp == NSDragOperationNone) {
    // WebKit refused this drop; we only accepted it for the runtime. End
    // WebKit's drag session the way a cancelled drag would.
    [super draggingExited:info];
    return YES;
  }
  return [super performDragOperation:info];
}
@end

@interface LaufeyScriptMessageHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) WKWebViewBackend* backend;
@property(nonatomic, assign) uint32_t windowId;
@end

@implementation LaufeyScriptMessageHandler

// Whether a message from a document with `origin` may be the web view's page
// at `url`: the same scheme, host and port (a missing port is the scheme's
// default, which WebKit reports as 0). A page without a host (HTML loaded
// with no base URL is about:blank, with an opaque origin) has no origin to
// compare, so only the main-frame check applies to it.
static bool LaufeyOriginMatchesURL(WKSecurityOrigin* origin, NSURL* url) {
  if (!origin)
    return false;
  if (!url || url.host.length == 0)
    return true;
  NSString* host = url.host;
  NSInteger port = url.port ? url.port.integerValue : 0;
  return [origin.protocol caseInsensitiveCompare:url.scheme] == NSOrderedSame &&
         [origin.host caseInsensitiveCompare:host] == NSOrderedSame &&
         origin.port == port;
}

- (void)userContentController:(WKUserContentController*)userContentController
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if (![message.name isEqualToString:@"laufey"])
    return;

  // WebKit exposes a script message handler to every frame (only the bridge
  // script is injected forMainFrameOnly), so a sub-frame, cross-origin
  // content included, could post here directly. Accept only the main frame,
  // and only while its document is still the web view's page (not a message
  // from a page navigated away from). Mirrors WebView2's source check and
  // CEF's main-frame-only binding.
  WKFrameInfo* frame = message.frameInfo;
  if (!frame || !frame.isMainFrame ||
      !LaufeyOriginMatchesURL(frame.securityOrigin, message.webView.URL)) {
    return;
  }

  if (![message.body isKindOfClass:[NSDictionary class]])
    return;

  NSDictionary* body = (NSDictionary*)message.body;

  NSNumber* callIdNum = body[@"callId"];
  NSString* method = body[@"method"];
  id argsJson = body[@"args"];

  if (!callIdNum || !method)
    return;

  uint64_t call_id = [callIdNum unsignedLongLongValue];
  std::string methodStr = [method UTF8String];

  laufey::ValuePtr args = laufey::Value::List();
  if ([argsJson isKindOfClass:[NSArray class]]) {
    NSArray* argsArray = (NSArray*)argsJson;
    NSError* error = nil;
    NSData* jsonData = [NSJSONSerialization dataWithJSONObject:argsArray
                                                       options:0
                                                         error:&error];
    if (jsonData) {
      NSString* jsonStr = [[NSString alloc] initWithData:jsonData
                                                encoding:NSUTF8StringEncoding];
      args = json::ParseJson([jsonStr UTF8String]);
    }
  }

  if (self.backend) {
    self.backend->HandleJsMessage(self.windowId, call_id, methodStr, args);
  }
}

@end

// --- Custom URL scheme handling (in-process transport) ---
//
// Serves "app" and every scheme the embedder registered (see
// RegisterSchemeHandler); the same handler object is installed for each.

class MacSchemeExchange;

// One in-flight WKURLSchemeTask: `stopped` is set when WebKit stops it
// (-stopURLSchemeTask:), after which it must not be messaged; `gate`
// reports that stop to the runtime (on_cancel) unless it finished first.
struct MacSchemeTaskState {
  std::atomic<bool> stopped{false};
  laufey_common::SchemeCancelGate gate;
  // Set on the main thread before the runtime sees it; deleted (on the main
  // thread) only after gate.Finish().
  MacSchemeExchange* exchange = nullptr;
};

// The in-flight tasks of every scheme handler, by task. An entry goes when
// WebKit stops the task or when its exchange finishes, whichever is first.
static std::mutex g_scheme_tasks_mutex;
static std::map<void*, std::shared_ptr<MacSchemeTaskState>>& SchemeTasks() {
  static auto* tasks =
      new std::map<void*, std::shared_ptr<MacSchemeTaskState>>();
  return *tasks;
}

// Exchange wrapping a WKURLSchemeTask. WKURLSchemeTask methods must be invoked
// on the main thread, so each response step hops there; the task's `stopped`
// flag prevents messaging an already-cancelled task.
class MacSchemeExchange : public SchemeExchangeBase {
 public:
  MacSchemeExchange(id<WKURLSchemeTask> task, NSData* body,
                    std::shared_ptr<MacSchemeTaskState> state)
      : task_(task), body_(body), state_(std::move(state)) {}

  intptr_t ReadRequestBody(uint8_t* buf, size_t cap) override {
    if (cap == 0 || !body_)
      return 0;
    size_t total = body_.length;
    if (cursor_ >= total)
      return 0;
    size_t n = std::min(cap, total - cursor_);
    memcpy(buf, static_cast<const uint8_t*>(body_.bytes) + cursor_, n);
    cursor_ += n;
    return static_cast<intptr_t>(n);
  }

  void Begin(int status, const char* headers, size_t headers_len) override {
    auto pairs = LaufeyParseFlatHeaders(headers, headers_len);
    NSMutableDictionary* hdr = [NSMutableDictionary dictionary];
    for (const auto& [k, v] : pairs) {
      NSString* name = [NSString stringWithUTF8String:k.c_str()];
      NSString* value = [NSString stringWithUTF8String:v.c_str()];
      if (!name || !value)
        continue;  // not UTF-8
      // A header field may repeat (Set-Cookie); NSHTTPURLResponse takes a
      // dictionary, so join the values the way HTTP folds them, as
      // CFNetwork does for a network response.
      NSString* previous = hdr[name];
      hdr[name] = previous
                      ? [NSString stringWithFormat:@"%@, %@", previous, value]
                      : value;
    }
    NSURL* url = task_.request.URL;
    id<WKURLSchemeTask> task = task_;
    std::shared_ptr<MacSchemeTaskState> state = state_;
    began_.store(true);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (state->stopped.load())
        return;
      NSHTTPURLResponse* resp =
          [[NSHTTPURLResponse alloc] initWithURL:url
                                      statusCode:status
                                     HTTPVersion:@"HTTP/1.1"
                                    headerFields:hdr];
      @try {
        [task didReceiveResponse:resp];
      } @catch (...) {
      }
    });
  }

  intptr_t WriteResponse(const uint8_t* buf, size_t len) override {
    if (state_->stopped.load())
      return -1;
    NSData* data = [NSData dataWithBytes:buf length:len];
    id<WKURLSchemeTask> task = task_;
    std::shared_ptr<MacSchemeTaskState> state = state_;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (state->stopped.load())
        return;
      @try {
        [task didReceiveData:data];
      } @catch (...) {
      }
    });
    return static_cast<intptr_t>(len);
  }

  void Finish() override {
    // No on_cancel from now on (and one in progress has returned).
    state_->gate.Finish();
    id<WKURLSchemeTask> task = task_;
    std::shared_ptr<MacSchemeTaskState> state = state_;
    bool began = began_.load();
    MacSchemeExchange* self = this;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!state->stopped.load()) {
        @try {
          if (began) {
            [task didFinish];
          } else {
            // Finished without a head: no response, so the request fails
            // (WebKit throws on didFinish before didReceiveResponse, which
            // left the request pending forever).
            [task
                didFailWithError:[NSError
                                     errorWithDomain:NSURLErrorDomain
                                                code:NSURLErrorBadServerResponse
                                            userInfo:nil]];
          }
        } @catch (...) {
        }
      }
      {
        std::lock_guard<std::mutex> lock(g_scheme_tasks_mutex);
        auto& tasks = SchemeTasks();
        auto it = tasks.find((__bridge void*)task);
        if (it != tasks.end() && it->second == state)
          tasks.erase(it);
      }
      delete self;
    });
  }

 private:
  id<WKURLSchemeTask> task_;
  NSData* body_;
  size_t cursor_ = 0;
  std::shared_ptr<MacSchemeTaskState> state_;
  std::atomic<bool> began_{false};
};

@interface LaufeyURLSchemeHandler : NSObject <WKURLSchemeHandler>
@property(nonatomic, assign) uint32_t windowId;
@end

@implementation LaufeyURLSchemeHandler

- (void)webView:(WKWebView*)webView
    startURLSchemeTask:(id<WKURLSchemeTask>)task {
  NSURLRequest* req = task.request;
  std::string method =
      req.HTTPMethod ? std::string(req.HTTPMethod.UTF8String) : "GET";
  std::string url = req.URL.absoluteString.UTF8String;
  std::vector<std::pair<std::string, std::string>> headers;
  NSDictionary<NSString*, NSString*>* fields = req.allHTTPHeaderFields;
  for (NSString* key in fields) {
    headers.emplace_back(key.UTF8String, fields[key].UTF8String);
  }
  std::string flat = LaufeyFlattenHeaders(headers);

  // Note: WKURLSchemeHandler does not expose the request body for all request
  // kinds (a long-standing WebKit limitation); HTTPBody is forwarded when
  // present.
  auto state = std::make_shared<MacSchemeTaskState>();
  {
    std::lock_guard<std::mutex> lock(g_scheme_tasks_mutex);
    SchemeTasks()[(__bridge void*)task] = state;
  }
  MacSchemeExchange* exchange =
      new MacSchemeExchange(task, req.HTTPBody, state);
  state->exchange = exchange;
  RuntimeLoader::GetInstance()->DispatchSchemeRequest(self.windowId, exchange,
                                                      method, url, flat);
}

- (void)webView:(WKWebView*)webView
    stopURLSchemeTask:(id<WKURLSchemeTask>)task {
  std::shared_ptr<MacSchemeTaskState> state;
  {
    std::lock_guard<std::mutex> lock(g_scheme_tasks_mutex);
    auto& tasks = SchemeTasks();
    auto it = tasks.find((__bridge void*)task);
    if (it == tasks.end())
      return;
    state = it->second;
    state->stopped.store(true);
    tasks.erase(it);
  }
  // The page gave up on the request: tell the runtime, unless it finished.
  // The exchange is deleted on this thread, after gate.Finish().
  MacSchemeExchange* exchange = state->exchange;
  state->gate.Cancel([exchange] {
    RuntimeLoader::GetInstance()->DispatchSchemeCancel(exchange);
  });
}

@end

// ============================================================================
// Per-app website data store
// ============================================================================

// Namespace for the name-based (v5) UUIDs below. Fixed forever: changing it
// would orphan every app's stored data.
static const uuid_t kLaufeyDataStoreNamespace = {
    0x91, 0x48, 0x13, 0xaa, 0x31, 0x15, 0x47, 0x24,
    0x93, 0x7f, 0x29, 0xb5, 0x4f, 0x44, 0x13, 0x56};  // 914813aa-3115-4724-...

// RFC 9562 UUIDv5 (SHA-1) of `name` in kLaufeyDataStoreNamespace.
static NSUUID* LaufeyNameBasedUUID(const std::string& name) {
  std::string input(reinterpret_cast<const char*>(kLaufeyDataStoreNamespace),
                    sizeof(uuid_t));
  input += name;
  unsigned char digest[CC_SHA1_DIGEST_LENGTH];
  // SHA-1 is what RFC 9562 specifies for v5; this is an identifier, not a
  // security boundary.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CC_SHA1(input.data(), static_cast<CC_LONG>(input.size()), digest);
#pragma clang diagnostic pop
  uuid_t bytes;
  memcpy(bytes, digest, sizeof(uuid_t));
  bytes[6] = (bytes[6] & 0x0f) | 0x50;  // version 5
  bytes[8] = (bytes[8] & 0x3f) | 0x80;  // RFC 9562 variant
  return [[NSUUID alloc] initWithUUIDBytes:bytes];
}

// The data store every WKWebView uses, or nil for the default store. WKWebView
// can't be pointed at an arbitrary directory, so a configured app data dir
// maps to a persistent identifier-based store (macOS 14+) instead: the UUIDv5
// of LAUFEY_DATA_DIR when that is set, else of LAUFEY_APP_ID. Without either,
// or before macOS 14, the default store (keyed by the host bundle id) is kept.
static WKWebsiteDataStore* LaufeyWebsiteDataStore() {
  static WKWebsiteDataStore* store = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    const std::string& dir = laufey_common::AppDataDir();
    if (dir.empty())
      return;
    // AppDataDir() is non-empty, so it came from a valid data dir or,
    // failing that, from a valid app id (each from its environment variable
    // or the launch file, see laufey_launch_config.h).
    std::string data_dir = laufey_common::LaunchDataDir();
    std::string name = laufey_common::IsAbsolutePath(data_dir)
                           ? dir
                           : laufey_common::LaunchAppId();
    if (@available(macOS 14.0, *)) {
      store =
          [WKWebsiteDataStore dataStoreForIdentifier:LaufeyNameBasedUUID(name)];
    }
  });
  return store;
}

@interface LaufeyUIDelegate : NSObject <WKUIDelegate>
@end

@implementation LaufeyUIDelegate

// The page's window.outerWidth / outerHeight and screenX / screenY. WebKit
// asks its UI delegate for the window's frame through this informal
// WKUIDelegatePrivate method (Safari implements it) and answers 0 for all
// four when the delegate does not. WebKit flips the Cocoa frame itself.
- (void)_webView:(WKWebView*)webView
    getWindowFrameWithCompletionHandler:(void (^)(CGRect))completionHandler {
  NSWindow* window = [webView window];
  completionHandler(window ? NSRectToCGRect([window frame]) : CGRectZero);
}

// `target="_blank"` and `window.open()` request a new browsing context, which
// the Navigation API interceptor never sees. WKWebView has no popup support, so
// route http(s) destinations to the OS browser and create no new webview.
- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)navigationAction
                    windowFeatures:(WKWindowFeatures*)windowFeatures {
  NSURL* url = navigationAction.request.URL;
  if (url && ([url.scheme isEqualToString:@"http"] ||
              [url.scheme isEqualToString:@"https"])) {
    [[NSWorkspace sharedWorkspace] openURL:url];
  }
  return nil;
}

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))completionHandler {
  NSAlert* alert = [[NSAlert alloc] init];
  [alert setMessageText:message];
  [alert addButtonWithTitle:@"OK"];
  [alert setAlertStyle:NSAlertStyleInformational];
  [alert runModal];
  completionHandler();
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:
                           (void (^)(BOOL result))completionHandler {
  NSAlert* alert = [[NSAlert alloc] init];
  [alert setMessageText:message];
  [alert addButtonWithTitle:@"OK"];
  [alert addButtonWithTitle:@"Cancel"];
  [alert setAlertStyle:NSAlertStyleInformational];
  NSModalResponse response = [alert runModal];
  completionHandler(response == NSAlertFirstButtonReturn);
}

- (void)webView:(WKWebView*)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
                              defaultText:(NSString*)defaultText
                         initiatedByFrame:(WKFrameInfo*)frame
                        completionHandler:(void (^)(NSString* _Nullable result))
                                              completionHandler {
  NSAlert* alert = [[NSAlert alloc] init];
  [alert setMessageText:prompt];
  [alert addButtonWithTitle:@"OK"];
  [alert addButtonWithTitle:@"Cancel"];
  NSTextField* input =
      [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
  [input setStringValue:defaultText ?: @""];
  [alert setAccessoryView:input];
  [alert.window setInitialFirstResponder:input];
  NSModalResponse response = [alert runModal];
  if (response == NSAlertFirstButtonReturn) {
    completionHandler([input stringValue]);
  } else {
    completionHandler(nil);
  }
}

// `<input type="file">`. WKWebView has no default handler; the host must
// present the open panel or the input stays inert. Honor multiple-selection and
// directory flags from the parameters, and attach as a sheet to the window.
- (void)webView:(WKWebView*)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
              initiatedByFrame:(WKFrameInfo*)frame
             completionHandler:
                 (void (^)(NSArray<NSURL*>* _Nullable))completionHandler {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  [panel setAllowsMultipleSelection:parameters.allowsMultipleSelection];
  [panel setCanChooseDirectories:parameters.allowsDirectories];
  [panel setCanChooseFiles:!parameters.allowsDirectories];

  void (^handler)(NSModalResponse) = ^(NSModalResponse response) {
    if (response == NSModalResponseOK) {
      completionHandler([panel URLs]);
    } else {
      completionHandler(nil);
    }
  };

  NSWindow* window = webView.window;
  if (window) {
    [panel beginSheetModalForWindow:window completionHandler:handler];
  } else {
    handler([panel runModal]);
  }
}

@end

@interface LaufeyWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) WKWebViewBackend* backend;
@property(nonatomic, assign) uint32_t windowId;
@end

@implementation LaufeyWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
  return RuntimeLoader::GetInstance()->DispatchCloseRequestedEvent(
             self.windowId)
             ? YES
             : NO;
}

// Fires only for AppKit-driven closes (windowShouldClose: returned YES):
// without it the backend's per-window state — the NSWindow strong ref
// (releasedWhenClosed:NO), notification observers, and the "laufey" script
// message handler — would leak, and the window id would stay live (get_size
// still reporting the old frame, show() able to resurrect the window).
// CloseWindow() detaches the delegate before [window close], so the
// programmatic path can't double-clean through here.
- (void)windowWillClose:(NSNotification*)notification {
  if (self.backend) {
    self.backend->OnWindowClosedByUser(self.windowId);
  }
}

@end

// Mirrors the page's `document.title` onto the NSWindow title via KVO on
// WKWebView.title, matching the Windows (DocumentTitleChanged) and Linux
// (notify::title) backends — macOS was the only backend where a page setting
// `document.title` never reached the OS-level window title
// (denoland/deno#35711). An empty title (page without <title>) is ignored so
// it doesn't clobber the host-set default (app name).
@interface LaufeyTitleObserver : NSObject
@property(nonatomic, weak) NSWindow* window;
@end

@implementation LaufeyTitleObserver

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
  if (![keyPath isEqualToString:@"title"]) {
    return;
  }
  NSString* title = change[NSKeyValueChangeNewKey];
  if ([title isKindOfClass:[NSString class]] && title.length > 0) {
    [self.window setTitle:title];
  }
}

@end

@interface LaufeyNavigationDelegate : NSObject <WKNavigationDelegate>
@property(nonatomic, assign) uint32_t windowId;
@end

@implementation LaufeyNavigationDelegate

// Fired when a main-frame navigation finishes loading. Used to reveal a window
// created with LAUFEY_WINDOW_FLAG_HIDDEN once it has real content to show.
- (void)webView:(WKWebView*)webView
    didFinishNavigation:(WKNavigation*)navigation {
  RuntimeLoader::GetInstance()->DispatchPageLoadEvent(self.windowId);
}

@end

namespace {

// NSEvent → W3C key/code lives in backend-common
// (laufey_common::NSEventKeyToKey / NSEventKeyToCode). Local aliases keep the
// existing callers compiling.
inline std::string NSEventKeyToString(NSEvent* event) {
  return laufey_common::NSEventKeyToKey((__bridge void*)event);
}
inline std::string NSEventKeyCodeToCode(unsigned short keyCode) {
  return laufey_common::NSEventKeyToCode(keyCode);
}

// FlagsChanged has no `characters`. Map the hardware key to the Web `key`.
std::string ModifierKeyFromKeyCode(unsigned short keyCode) {
  switch (keyCode) {
    case 56:
    case 60:
      return "Shift";
    case 59:
    case 62:
      return "Control";
    case 58:
    case 61:
      return "Alt";
    case 54:
    case 55:
      return "Meta";
    default:
      return "";
  }
}

bool ModifierFlagIsDown(unsigned short keyCode, NSEventModifierFlags flags) {
  switch (keyCode) {
    case 56:
    case 60:
      return (flags & NSEventModifierFlagShift) != 0;
    case 59:
    case 62:
      return (flags & NSEventModifierFlagControl) != 0;
    case 58:
    case 61:
      return (flags & NSEventModifierFlagOption) != 0;
    case 54:
    case 55:
      return (flags & NSEventModifierFlagCommand) != 0;
    default:
      return false;
  }
}

// NSEvent.clickCount is 0 on some mouse-up deliveries. Keep the press count
// so `click.detail` matches a browser (1 for a single click).
int32_t ResolveClickCount(int state, int32_t click_count) {
  static int32_t last_click_count = 1;
  if (state == LAUFEY_MOUSE_PRESSED) {
    if (click_count < 1)
      click_count = 1;
    last_click_count = click_count;
    return click_count;
  }
  return click_count >= 1 ? click_count : last_click_count;
}

uint32_t NSModifierFlagsToLaufey(NSEventModifierFlags flags) {
  uint32_t modifiers = 0;
  if (flags & NSEventModifierFlagShift)
    modifiers |= LAUFEY_MOD_SHIFT;
  if (flags & NSEventModifierFlagControl)
    modifiers |= LAUFEY_MOD_CONTROL;
  if (flags & NSEventModifierFlagOption)
    modifiers |= LAUFEY_MOD_ALT;
  if (flags & NSEventModifierFlagCommand)
    modifiers |= LAUFEY_MOD_META;
  return modifiers;
}

int NSButtonToLaufey(NSInteger buttonNumber) {
  switch (buttonNumber) {
    case 0:
      return LAUFEY_MOUSE_BUTTON_LEFT;
    case 1:
      return LAUFEY_MOUSE_BUTTON_RIGHT;
    case 2:
      return LAUFEY_MOUSE_BUTTON_MIDDLE;
    case 3:
      return LAUFEY_MOUSE_BUTTON_BACK;
    case 4:
      return LAUFEY_MOUSE_BUTTON_FORWARD;
    default:
      return LAUFEY_MOUSE_BUTTON_LEFT;
  }
}

}  // namespace

// --- WKWebViewBackend implementation ---

WKWebViewBackend::WKWebViewBackend() {}

WKWebViewBackend::~WKWebViewBackend() {
  RemoveGlobalMonitors();
  // Close all remaining windows
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    @autoreleasepool {
      if (state.focus_observer)
        [[NSNotificationCenter defaultCenter]
            removeObserver:state.focus_observer];
      if (state.blur_observer)
        [[NSNotificationCenter defaultCenter]
            removeObserver:state.blur_observer];
      if (state.resize_observer)
        [[NSNotificationCenter defaultCenter]
            removeObserver:state.resize_observer];
      if (state.move_observer)
        [[NSNotificationCenter defaultCenter]
            removeObserver:state.move_observer];
      if (state.webview)
        [state.webview.configuration.userContentController
            removeScriptMessageHandlerForName:@"laufey"];
      // Detach the delegate: AppKit may keep an on-screen window alive past
      // this destructor, and its windowWillClose: would otherwise call back
      // into this destroyed backend.
      if (state.window)
        [state.window setDelegate:nil];
      UnregisterNSWindow(state.window);
    }
  }
  windows_.clear();
}

MacWindowState* WKWebViewBackend::GetWindow(uint32_t window_id) {
  auto it = windows_.find(window_id);
  return it != windows_.end() ? &it->second : nullptr;
}

void WKWebViewBackend::RemoveWindowState(uint32_t window_id) {
  auto it = windows_.find(window_id);
  if (it == windows_.end())
    return;

  auto& state = it->second;
  @autoreleasepool {
    if (state.focus_observer)
      [[NSNotificationCenter defaultCenter]
          removeObserver:state.focus_observer];
    if (state.blur_observer)
      [[NSNotificationCenter defaultCenter] removeObserver:state.blur_observer];
    if (state.resize_observer)
      [[NSNotificationCenter defaultCenter]
          removeObserver:state.resize_observer];
    if (state.move_observer)
      [[NSNotificationCenter defaultCenter] removeObserver:state.move_observer];
    if (state.webview && state.title_observer)
      [state.webview removeObserver:state.title_observer forKeyPath:@"title"];
    if (state.webview)
      [state.webview.configuration.userContentController
          removeScriptMessageHandlerForName:@"laufey"];
    if (state.window) {
      [state.window setDelegate:nil];
      laufey_common::MacUnwatchWindowState((__bridge void*)state.window);
    }
    UnregisterNSWindow(state.window);
  }
  laufey_common::ForgetWindow(window_id);
  windows_.erase(it);
}

void WKWebViewBackend::OnWindowClosedByUser(uint32_t window_id) {
  // Main thread (AppKit delivers windowWillClose: there); safe to take the
  // lock and tear down state while the window finishes closing.
  NSWindow* win = nil;
  {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    if (auto* state = GetWindow(window_id))
      win = state->window;
    RemoveWindowState(window_id);
  }
  // A passkey sheet anchored to the window ends with it (`cancelled`).
  // Outside the lock: the result callback may re-enter the backend.
  if (win) {
    laufey_common::PasskeyWindowClosing((__bridge const void*)win);
    // So does an auth session sheet (`cancelled`).
    laufey_common::AuthSessionWindowClosing((__bridge const void*)win);
  }
}

void WKWebViewBackend::InstallGlobalMonitors() {
  if (monitors_installed_)
    return;
  monitors_installed_ = true;

  keyboard_monitor_ = [NSEvent
      addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown |
                                            NSEventMaskKeyUp |
                                            NSEventMaskFlagsChanged)
                                   handler:^NSEvent*(NSEvent* event) {
                                     NSWindow* win = [event window];
                                     uint32_t wid = LaufeyIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     uint32_t modifiers =
                                         NSModifierFlagsToLaufey(
                                             [event modifierFlags]);
                                     if ([event type] ==
                                         NSEventTypeFlagsChanged) {
                                       unsigned short kc = [event keyCode];
                                       std::string key =
                                           ModifierKeyFromKeyCode(kc);
                                       if (key.empty())
                                         return event;
                                       int state =
                                           ModifierFlagIsDown(
                                               kc, [event modifierFlags])
                                               ? LAUFEY_KEY_PRESSED
                                               : LAUFEY_KEY_RELEASED;
                                       std::string code =
                                           NSEventKeyCodeToCode(kc);
                                       RuntimeLoader::GetInstance()
                                           ->DispatchKeyboardEvent(
                                               wid, state, key.c_str(),
                                               code.c_str(), modifiers, false);
                                       return event;
                                     }

                                     int state =
                                         ([event type] == NSEventTypeKeyDown)
                                             ? LAUFEY_KEY_PRESSED
                                             : LAUFEY_KEY_RELEASED;
                                     std::string key =
                                         NSEventKeyToString(event);
                                     std::string code =
                                         NSEventKeyCodeToCode([event keyCode]);
                                     bool repeat = [event isARepeat];

                                     RuntimeLoader::GetInstance()
                                         ->DispatchKeyboardEvent(
                                             wid, state, key.c_str(),
                                             code.c_str(), modifiers, repeat);

                                     return event;
                                   }];

  mouse_monitor_ = [NSEvent
      addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown |
                                            NSEventMaskLeftMouseUp |
                                            NSEventMaskRightMouseDown |
                                            NSEventMaskRightMouseUp |
                                            NSEventMaskOtherMouseDown |
                                            NSEventMaskOtherMouseUp)
                                   handler:^NSEvent*(NSEvent* event) {
                                     NSWindow* win = [event window];
                                     uint32_t wid = LaufeyIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     int state;
                                     switch ([event type]) {
                                       case NSEventTypeLeftMouseDown:
                                       case NSEventTypeRightMouseDown:
                                       case NSEventTypeOtherMouseDown:
                                         state = LAUFEY_MOUSE_PRESSED;
                                         break;
                                       default:
                                         state = LAUFEY_MOUSE_RELEASED;
                                         break;
                                     }

                                     int button =
                                         NSButtonToLaufey([event buttonNumber]);
                                     uint32_t modifiers =
                                         NSModifierFlagsToLaufey(
                                             [event modifierFlags]);
                                     int32_t click_count = ResolveClickCount(
                                         state, (int32_t)[event clickCount]);

                                     NSPoint loc = [event locationInWindow];
                                     double x = loc.x;
                                     double y = 0;
                                     if (win) {
                                       y = [win contentLayoutRect].size.height -
                                           loc.y;
                                     }

                                     RuntimeLoader::GetInstance()
                                         ->DispatchMouseClickEvent(
                                             wid, state, button, x, y,
                                             modifiers, click_count);

                                     return event;
                                   }];

  mouse_move_monitor_ = [NSEvent
      addLocalMonitorForEventsMatchingMask:(NSEventMaskMouseMoved |
                                            NSEventMaskLeftMouseDragged |
                                            NSEventMaskRightMouseDragged |
                                            NSEventMaskOtherMouseDragged)
                                   handler:^NSEvent*(NSEvent* event) {
                                     NSWindow* win = [event window];
                                     uint32_t wid = LaufeyIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     uint32_t modifiers =
                                         NSModifierFlagsToLaufey(
                                             [event modifierFlags]);
                                     NSPoint loc = [event locationInWindow];
                                     double x = loc.x;
                                     double y = 0;
                                     if (win) {
                                       y = [win contentLayoutRect].size.height -
                                           loc.y;
                                     }

                                     RuntimeLoader::GetInstance()
                                         ->DispatchMouseMoveEvent(wid, x, y,
                                                                  modifiers);
                                     return event;
                                   }];

  scroll_monitor_ = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                   handler:^NSEvent*(NSEvent* event) {
                                     NSWindow* win = [event window];
                                     uint32_t wid = LaufeyIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     double delta_x = [event scrollingDeltaX];
                                     double delta_y = [event scrollingDeltaY];
                                     uint32_t modifiers =
                                         NSModifierFlagsToLaufey(
                                             [event modifierFlags]);

                                     int32_t delta_mode =
                                         [event hasPreciseScrollingDeltas]
                                             ? LAUFEY_WHEEL_DELTA_PIXEL
                                             : LAUFEY_WHEEL_DELTA_LINE;

                                     NSPoint loc = [event locationInWindow];
                                     double x = loc.x;
                                     double y = 0;
                                     if (win) {
                                       y = [win contentLayoutRect].size.height -
                                           loc.y;
                                     }

                                     RuntimeLoader::GetInstance()
                                         ->DispatchWheelEvent(
                                             wid, delta_x, delta_y, x, y,
                                             modifiers, delta_mode);
                                     return event;
                                   }];
}

void WKWebViewBackend::RemoveGlobalMonitors() {
  @autoreleasepool {
    if (keyboard_monitor_) {
      [NSEvent removeMonitor:keyboard_monitor_];
      keyboard_monitor_ = nil;
    }
    if (mouse_monitor_) {
      [NSEvent removeMonitor:mouse_monitor_];
      mouse_monitor_ = nil;
    }
    if (mouse_move_monitor_) {
      [NSEvent removeMonitor:mouse_move_monitor_];
      mouse_move_monitor_ = nil;
    }
    if (scroll_monitor_) {
      [NSEvent removeMonitor:scroll_monitor_];
      scroll_monitor_ = nil;
    }
    if (forward_mouse_monitor_) {
      [NSEvent removeMonitor:forward_mouse_monitor_];
      forward_mouse_monitor_ = nil;
    }
    if (forward_mouse_move_monitor_) {
      [NSEvent removeMonitor:forward_mouse_move_monitor_];
      forward_mouse_move_monitor_ = nil;
    }
    if (forward_scroll_monitor_) {
      [NSEvent removeMonitor:forward_scroll_monitor_];
      forward_scroll_monitor_ = nil;
    }
  }
}

void WKWebViewBackend::CreateWindow(uint32_t window_id, int width, int height) {
  CreateWindowEx(window_id, width, height, 0);
}

void WKWebViewBackend::CreateWindowEx(uint32_t window_id, int width, int height,
                                      uint32_t flags) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      InstallGlobalMonitors();

      bool frameless = (flags & LAUFEY_WINDOW_FLAG_FRAMELESS) != 0;
      bool no_activate = (flags & LAUFEY_WINDOW_FLAG_NO_ACTIVATE) != 0;
      bool transparent = (flags & LAUFEY_WINDOW_FLAG_TRANSPARENT) != 0;
      // Ignored on a frameless window, which has no title bar to begin with.
      bool transparent_titlebar =
          !frameless && (flags & LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR) != 0;

      NSRect frame = NSMakeRect(0, 0, width, height);
      NSWindowStyleMask style = NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable |
                                NSWindowStyleMaskResizable;
      if (frameless) {
        // Drop the title bar and standard chrome; borderless content area.
        style = NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable;
      } else {
        style |= NSWindowStyleMaskTitled;
      }
      if (transparent_titlebar) {
        // Let the content view extend the full height of the window, up under
        // the title bar (made transparent below).
        style |= NSWindowStyleMaskFullSizeContentView;
      }

      NSWindow* window;
      if (no_activate) {
        // A real non-activating NSPanel floats above normal windows and can
        // take key focus without activating the app — the native menu-bar /
        // tray popover behavior.
        style |= NSWindowStyleMaskNonactivatingPanel;
        NSPanel* panel =
            [[NSPanel alloc] initWithContentRect:frame
                                       styleMask:style
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
        [panel setBecomesKeyOnlyIfNeeded:YES];
        [panel setFloatingPanel:YES];
        [panel setHidesOnDeactivate:NO];
        [panel setLevel:NSPopUpMenuWindowLevel];
        [panel
            setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                  NSWindowCollectionBehaviorTransient |
                                  NSWindowCollectionBehaviorIgnoresCycle];
        window = panel;
      } else if (frameless) {
        // Borderless windows must be told they can become key/main, otherwise
        // they receive no keyboard or mouse input (see LaufeyKeyableWindow).
        window = [[LaufeyKeyableWindow alloc]
            initWithContentRect:frame
                      styleMask:style
                        backing:NSBackingStoreBuffered
                          defer:NO];
      } else {
        window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:style
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
      }
      // Under ARC this object's lifetime is owned entirely by our strong
      // references (the windows_ map, the local `win` in CloseWindow, and the
      // event-monitor / notification blocks that capture it). AppKit's legacy
      // default is releasedWhenClosed == YES, which makes `[window close]`
      // perform an *extra* release that ARC never balances. That over-release
      // deallocates the window while it is still referenced, and the stale
      // pointer is drained on the next main-thread autorelease-pool cleanup —
      // an EXC_BAD_ACCESS (freed 0xa1a1a1a1 pattern) seconds after the window
      // closes. Opt out so ARC is the sole owner of the window's lifetime.
      [window setReleasedWhenClosed:NO];

      if (transparent_titlebar) {
        // Hidden/inset title bar (Electron `titleBarStyle: 'hidden'`): the web
        // content fills the whole window, including the strip behind the title
        // bar, which is made transparent with its text hidden so the standard
        // traffic-light buttons float over the page. The app insets its own UI
        // to clear the buttons. Mirrors the CEF backend's transparent-titlebar
        // path (ConfigureNSWindowTransparentTitlebarForCefHandle).
        window.titlebarAppearsTransparent = YES;
        window.titleVisibility = NSWindowTitleHidden;
      }

      [window center];

      LaufeyWindowDelegate* delegate = [[LaufeyWindowDelegate alloc] init];
      delegate.backend = this;
      delegate.windowId = window_id;
      [window setDelegate:delegate];

      LaufeyScriptMessageHandler* handler =
          [[LaufeyScriptMessageHandler alloc] init];
      handler.backend = this;
      handler.windowId = window_id;

      WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
      if (WKWebsiteDataStore* store = LaufeyWebsiteDataStore()) {
        config.websiteDataStore = store;
      }
      [config.userContentController addScriptMessageHandler:handler
                                                       name:@"laufey"];

      // Install the in-process scheme handler for "app" and for every scheme
      // the embedder registered through register_scheme_handler (must be set
      // on the configuration before the WKWebView is created, which is why a
      // scheme has to be registered before its first window). One handler
      // serves them all: requests are bridged into the runtime's memory
      // transport, which dispatches on the URL; if no runtime handler is
      // registered the exchange is finished immediately. WKWebView treats
      // handled schemes as secure contexts with `<scheme>://<host>` origins,
      // so nothing else is needed for them to behave like https origins.
      LaufeyURLSchemeHandler* schemeHandler =
          [[LaufeyURLSchemeHandler alloc] init];
      schemeHandler.windowId = window_id;
      for (const std::string& scheme :
           laufey_common::SchemeRegistry::GetInstance()->Snapshot()) {
        NSString* name = @(scheme.c_str());
        // WebKit owns http/https/file/data/... and -setURLSchemeHandler:
        // raises for them; skip rather than crash on a bad registration.
        if ([WKWebView handlesURLScheme:name]) {
          std::cerr << "laufey: scheme \"" << scheme
                    << "\" is handled natively by WebKit; not installing the "
                       "in-process handler for it"
                    << std::endl;
          continue;
        }
        [config setURLSchemeHandler:schemeHandler forURLScheme:name];
      }
      any_window_created_.store(true);

      std::string initScript =
          BuildInitScript(RuntimeLoader::GetInstance()->GetJsNamespace(),
                          "window.webkit.messageHandlers.laufey.postMessage({\n"
                          "            callId: callId,\n"
                          "            method: path.join('.'),\n"
                          "            args: processedArgs\n"
                          "          });");
      WKUserScript* script = [[WKUserScript alloc]
            initWithSource:[NSString stringWithUTF8String:initScript.c_str()]
             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
          forMainFrameOnly:YES];
      [config.userContentController addUserScript:script];

      // DevTools (API 40): on unless the app launched with
      // LAUFEY_INSPECTABLE=0 / "inspectable": false. `inspectable` (macOS
      // 13.3+, set below) governs Safari's Web Inspector and the context
      // menu's Inspect Element; developerExtrasEnabled (read when the web
      // view is created) is the switch before 13.3.
      const bool inspectable = laufey_common::LaunchInspectable();
      @try {
        [config.preferences setValue:@(inspectable)
                              forKey:@"developerExtrasEnabled"];
      } @catch (NSException*) {
      }
      LaufeyWebView* webview = [[LaufeyWebView alloc] initWithFrame:frame
                                                      configuration:config];
      webview.laufeyWindowId = window_id;
      if ([webview respondsToSelector:@selector(setInspectable:)]) {
        [webview setInspectable:inspectable];
      }
      LaufeyUIDelegate* uiDelegate = [[LaufeyUIDelegate alloc] init];
      webview.UIDelegate = uiDelegate;
      LaufeyNavigationDelegate* navDelegate =
          [[LaufeyNavigationDelegate alloc] init];
      navDelegate.windowId = window_id;
      webview.navigationDelegate = navDelegate;

      if (transparent) {
        // Make the window non-opaque with a clear backdrop and stop the
        // WKWebView from painting its own opaque background, so any region the
        // page leaves transparent shows whatever is behind the window.
        [window setOpaque:NO];
        [window setBackgroundColor:[NSColor clearColor]];
        // `drawsBackground` is an undocumented but long-stable WKWebView
        // property; KVC keeps us off the private @interface.
        @try {
          [webview setValue:@NO forKey:@"drawsBackground"];
        } @catch (NSException*) {
          // Older/newer WebKit without the key: fall back to a clear layer.
        }
      }

      [window setContentView:webview];
      // Route input into the web content once the window becomes key.
      [window makeFirstResponder:webview];

      RegisterNSWindow(window, window_id);
      // Window-state events and the normal-bounds tracker (API 38).
      laufey_common::MacWatchWindowState((__bridge void*)window, window_id);

      // Per-window notification observers
      id focus_obs = [[NSNotificationCenter defaultCenter]
          addObserverForName:NSWindowDidBecomeKeyNotification
                      object:window
                       queue:nil
                  usingBlock:^(NSNotification*) {
                    RuntimeLoader::GetInstance()->DispatchFocusedEvent(
                        window_id, 1);
                    // Swap to this window's menu
                    std::lock_guard<std::mutex> lock(windows_mutex_);
                    auto* state = GetWindow(window_id);
                    if (state && state->menu) {
                      [NSApp setMainMenu:state->menu];
                    }
                  }];

      id blur_obs = [[NSNotificationCenter defaultCenter]
          addObserverForName:NSWindowDidResignKeyNotification
                      object:window
                       queue:nil
                  usingBlock:^(NSNotification*) {
                    RuntimeLoader::GetInstance()->DispatchFocusedEvent(
                        window_id, 0);
                  }];

      id resize_obs = [[NSNotificationCenter defaultCenter]
          addObserverForName:NSWindowDidResizeNotification
                      object:window
                       queue:nil
                  usingBlock:^(NSNotification* note) {
                    NSWindow* w = [note object];
                    if (w) {
                      NSRect f = [[w contentView] frame];
                      RuntimeLoader::GetInstance()->DispatchResizeEvent(
                          window_id, (int)f.size.width, (int)f.size.height);
                    }
                  }];

      id move_obs = [[NSNotificationCenter defaultCenter]
          addObserverForName:NSWindowDidMoveNotification
                      object:window
                       queue:nil
                  usingBlock:^(NSNotification* note) {
                    NSWindow* w = [note object];
                    if (w) {
                      // Report top-left global coordinates, matching the
                      // convention SetWindowPosition/GetWindowPosition use —
                      // the raw Cocoa bottom-left origin previously leaked
                      // through here (denoland/deno#36119).
                      NSRect f = [w frame];
                      NSScreen* primary = [[NSScreen screens] firstObject];
                      CGFloat screenH = primary ? primary.frame.size.height : 0;
                      RuntimeLoader::GetInstance()->DispatchMoveEvent(
                          window_id, (int)f.origin.x,
                          (int)(screenH - f.origin.y - f.size.height));
                    }
                  }];

      LaufeyTitleObserver* titleObserver = [[LaufeyTitleObserver alloc] init];
      titleObserver.window = window;
      [webview addObserver:titleObserver
                forKeyPath:@"title"
                   options:NSKeyValueObservingOptionNew
                   context:nil];

      MacWindowState state;
      state.window_id = window_id;
      state.window = window;
      state.webview = webview;
      state.message_handler = handler;
      state.window_delegate = delegate;
      state.ui_delegate = uiDelegate;
      state.navigation_delegate = navDelegate;
      state.focus_observer = focus_obs;
      state.blur_observer = blur_obs;
      state.resize_observer = resize_obs;
      state.move_observer = move_obs;
      state.title_observer = titleObserver;

      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        windows_[window_id] = state;
      }

      bool hidden = (flags & LAUFEY_WINDOW_FLAG_HIDDEN) != 0;
      if (hidden) {
        // Leave the window unmapped; the embedder reveals it later (typically
        // from a page-load handler) so the empty initial frame is never shown.
      } else if (no_activate) {
        // Show without activating the app / stealing focus.
        [window orderFrontRegardless];
      } else {
        laufey_common::MacRevealWindowAtLaunch((__bridge void*)window);
      }
    }
  });
}

void WKWebViewBackend::CloseWindow(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSWindow* win = nil;
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        auto it = windows_.find(window_id);
        if (it == windows_.end())
          return;
        win = it->second.window;
        RemoveWindowState(window_id);
      }
      if (!win)
        return;
      // See OnWindowClosedByUser.
      laufey_common::PasskeyWindowClosing((__bridge const void*)win);
      laufey_common::AuthSessionWindowClosing((__bridge const void*)win);
      [win close];
    }
  });
}

void WKWebViewBackend::OpenExternalURL(const std::string& url) {
  std::string urlCopy = url;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSURL* nsurl =
          [NSURL URLWithString:[NSString stringWithUTF8String:urlCopy.c_str()]];
      if (nsurl) {
        [[NSWorkspace sharedWorkspace] openURL:nsurl];
      }
    }
  });
}

void WKWebViewBackend::RegisterSchemeHandler(const std::string& scheme) {
  // Any thread (the runtime registers from its own thread). The registry is
  // read when each WKWebViewConfiguration is built, so a scheme registered
  // after a window exists only reaches windows created later — the documented
  // contract is to register before the first window.
  if (!laufey_common::IsValidSchemeName(scheme)) {
    std::cerr << "laufey: ignoring invalid URL scheme name \"" << scheme
              << "\" passed to register_scheme_handler" << std::endl;
    return;
  }
  bool added = laufey_common::SchemeRegistry::GetInstance()->Add(scheme);
  if (added && any_window_created_.load()) {
    std::cerr << "laufey: scheme \"" << scheme
              << "\" was registered after a window was created; existing "
                 "windows will not serve it (register schemes before the "
                 "first window)"
              << std::endl;
  }
}

void WKWebViewBackend::Navigate(uint32_t window_id, const std::string& url) {
  std::string urlCopy = url;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state)
        return;

      if (urlCopy.find("data:text/html,") == 0) {
        NSString* html = [NSString stringWithUTF8String:urlCopy.c_str() + 15];
        html = [html stringByRemovingPercentEncoding];
        [state->webview loadHTMLString:html baseURL:nil];
        return;
      }

      NSURL* nsurl =
          [NSURL URLWithString:[NSString stringWithUTF8String:urlCopy.c_str()]];
      if (nsurl && nsurl.scheme && nsurl.scheme.length > 0) {
        NSURLRequest* request = [NSURLRequest requestWithURL:nsurl];
        [state->webview loadRequest:request];
      } else {
        NSString* path = [NSString stringWithUTF8String:urlCopy.c_str()];
        NSURL* fileURL = [NSURL fileURLWithPath:path];
        if (fileURL) {
          [state->webview loadFileURL:fileURL allowingReadAccessToURL:fileURL];
        }
      }
    }
  });
}

void WKWebViewBackend::SetTitle(uint32_t window_id, const std::string& title) {
  std::string titleCopy = title;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        [state->window
            setTitle:[NSString stringWithUTF8String:titleCopy.c_str()]];
      }
    }
  });
}

void WKWebViewBackend::ExecuteJs(uint32_t window_id, const std::string& script,
                                 laufey_js_result_fn callback,
                                 void* callback_data) {
  std::string scriptCopy = script;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state) {
        if (callback)
          callback(nullptr, nullptr, callback_data);
        return;
      }
      if (!callback) {
        [state->webview
            evaluateJavaScript:[NSString
                                   stringWithUTF8String:scriptCopy.c_str()]
             completionHandler:nil];
        return;
      }
      [state->webview
          evaluateJavaScript:[NSString stringWithUTF8String:scriptCopy.c_str()]
           completionHandler:^(id result, NSError* error) {
             if (error) {
               std::string errMsg = [[error localizedDescription] UTF8String];
               auto errVal = laufey::Value::String(errMsg);
               laufey_value errLaufey(errVal);
               callback(nullptr, &errLaufey, callback_data);
               return;
             }
             if (!result || [result isKindOfClass:[NSNull class]]) {
               callback(nullptr, nullptr, callback_data);
               return;
             }
             // Convert the result to JSON, then parse it back into a
             // laufey::Value
             NSError* jsonError = nil;
             NSData* jsonData = nil;
             if ([NSJSONSerialization isValidJSONObject:@[ result ]]) {
               // Wrap in array to handle primitives
               jsonData = [NSJSONSerialization dataWithJSONObject:@[ result ]
                                                          options:0
                                                            error:&jsonError];
             }
             if (jsonData) {
               NSString* jsonStr =
                   [[NSString alloc] initWithData:jsonData
                                         encoding:NSUTF8StringEncoding];
               // Parse the wrapped array, extract first element
               auto parsed = json::ParseJson([jsonStr UTF8String]);
               if (parsed && parsed->IsList() && !parsed->GetList().empty()) {
                 laufey_value resultLaufey(parsed->GetList()[0]);
                 callback(&resultLaufey, nullptr, callback_data);
               } else {
                 callback(nullptr, nullptr, callback_data);
               }
             } else if ([result isKindOfClass:[NSNumber class]]) {
               // Handle numbers that aren't valid JSON objects on their own
               NSNumber* num = (NSNumber*)result;
               const char* objcType = [num objCType];
               if (strcmp(objcType, @encode(BOOL)) == 0 ||
                   strcmp(objcType, @encode(char)) == 0) {
                 auto val = laufey::Value::Bool([num boolValue]);
                 laufey_value laufey(val);
                 callback(&laufey, nullptr, callback_data);
               } else if (strcmp(objcType, @encode(int)) == 0 ||
                          strcmp(objcType, @encode(long)) == 0 ||
                          strcmp(objcType, @encode(long long)) == 0) {
                 auto val = laufey::Value::Int([num intValue]);
                 laufey_value laufey(val);
                 callback(&laufey, nullptr, callback_data);
               } else {
                 auto val = laufey::Value::Double([num doubleValue]);
                 laufey_value laufey(val);
                 callback(&laufey, nullptr, callback_data);
               }
             } else if ([result isKindOfClass:[NSString class]]) {
               auto val = laufey::Value::String([(NSString*)result UTF8String]);
               laufey_value laufey(val);
               callback(&laufey, nullptr, callback_data);
             } else {
               callback(nullptr, nullptr, callback_data);
             }
           }];
    }
  });
}

void WKWebViewBackend::PrintToPdf(uint32_t window_id,
                                  laufey_pdf_result_fn callback,
                                  void* callback_data) {
  if (!callback)
    return;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      // Scope the lock to the lookup only: the callback is an arbitrary user
      // FnOnce that may re-enter backend APIs taking this same non-recursive
      // mutex on this thread. Using the webview after unlock is safe because
      // window teardown also runs on this (main) thread.
      WKWebView* webview = nil;
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        auto* state = GetWindow(window_id);
        if (state)
          webview = state->webview;
      }
      if (!webview) {
        callback(nullptr, 0, "window not found", callback_data);
        return;
      }
      if (@available(macOS 11.0, *)) {
        WKPDFConfiguration* config = [[WKPDFConfiguration alloc] init];
        [webview
            createPDFWithConfiguration:config
                     completionHandler:^(NSData* pdfData, NSError* error) {
                       if (error || !pdfData) {
                         std::string msg =
                             error ? [[error localizedDescription] UTF8String]
                                   : "failed to create PDF";
                         callback(nullptr, 0, msg.c_str(), callback_data);
                         return;
                       }
                       const uint8_t* bytes =
                           static_cast<const uint8_t*>([pdfData bytes]);
                       size_t len = [pdfData length];
                       callback(len ? bytes : nullptr, len, nullptr,
                                callback_data);
                     }];
      } else {
        callback(nullptr, 0, "print_to_pdf requires macOS 11 or newer",
                 callback_data);
      }
    }
  });
}

void WKWebViewBackend::PasskeyRequest(uint32_t window_id, uint32_t kind,
                                      const char* options_json,
                                      laufey_passkey_result_fn callback,
                                      void* user_data) {
  // Any thread. Refusals (no API, invalid options, busy) answer here,
  // synchronously; a started ceremony moves to the main thread.
  if (laufey_common::PasskeyCapabilitiesMac() == 0) {
    laufey_common::PasskeyReportNotSupported(callback, user_data);
    return;
  }
  std::shared_ptr<laufey_common::PasskeyCeremony> ceremony =
      laufey_common::PasskeyBegin(kind, options_json, callback, user_data);
  if (!ceremony)
    return;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSWindow* win = nil;
      bool found = window_id == 0;
      if (window_id != 0) {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        if (auto* state = GetWindow(window_id)) {
          win = state->window;
          found = win != nil;
        }
      }
      if (!found) {
        ceremony->Finish(laufey_common::PasskeyErrorEnvelope(
            laufey_common::kPasskeyUnknown,
            "window " + std::to_string(window_id) + " not found"));
        return;
      }
      // nil: the key / main window (see PasskeyStartMac).
      laufey_common::PasskeyStartMac(ceremony, (__bridge void*)win);
    }
  });
}

void WKWebViewBackend::AuthSessionStart(uint32_t window_id, const char* url,
                                        const char* callback, uint32_t flags,
                                        laufey_auth_session_result_fn on_result,
                                        void* user_data) {
  // Any thread. Refusals (invalid arguments, busy) answer here,
  // synchronously; a started session moves to the main thread.
  std::shared_ptr<laufey_common::AuthSession> session =
      laufey_common::AuthSessionBegin(laufey_common::AuthSessionCapabilities(),
                                      url, callback, flags, on_result,
                                      user_data);
  if (!session)
    return;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSWindow* win = nil;
      if (window_id != 0) {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        if (auto* state = GetWindow(window_id))
          win = state->window;
      }
      if (window_id != 0 && !win) {
        session->Finish(LAUFEY_AUTH_SESSION_INVALID,
                        "window " + std::to_string(window_id) + " not found");
        return;
      }
      // nil: the key / main window (see AuthSessionStartMac).
      laufey_common::AuthSessionStartMac(session, (__bridge void*)win);
    }
  });
}

void WKWebViewBackend::Quit() {
  laufey_common::MarkQuitting();
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp stop:nil];
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:YES];
  });
}

// AppKit's global coordinate space is bottom-left-anchored to the *primary*
// screen ([[NSScreen screens] firstObject], the one with the menu bar), while
// the laufey API speaks top-left global coordinates. Converting against the
// window's *current* screen — its local height, ignoring its global origin
// offset — lands windows in the wrong place on any multi-display setup
// (denoland/deno#36119). Always convert against the primary screen.
static CGFloat PrimaryScreenHeight() {
  NSScreen* primary = [[NSScreen screens] firstObject];
  return primary ? primary.frame.size.height : 0;
}

void WKWebViewBackend::SetWindowSize(uint32_t window_id, int width,
                                     int height) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        // Content size, matching CreateWindow's initWithContentRect: —
        // setting the *frame* size here made setSize(w, h) produce a window
        // whose page area was smaller than an identically-sized CreateWindow
        // by the title-bar height (denoland/deno#36119).
        int w = width, h = height;
        // -setContentSize: ignores contentMinSize / contentMaxSize.
        laufey_common::ClampSizeForWindow(window_id, &w, &h);
        [state->window setContentSize:NSMakeSize(w, h)];
      }
    }
  });
}

double WKWebViewBackend::GetWindowScaleFactor(uint32_t window_id) {
  __block double result = 1.0;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = (double)[state->window backingScaleFactor];
    }
  });
  return result;
}

void WKWebViewBackend::GetWindowSize(uint32_t window_id, int* width,
                                     int* height) {
  __block int w = 0, h = 0;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      // Content size, for symmetry with CreateWindow / SetWindowSize and the
      // resize event.
      NSRect content =
          [state->window contentRectForFrameRect:[state->window frame]];
      w = static_cast<int>(content.size.width);
      h = static_cast<int>(content.size.height);
    }
  });
  if (width)
    *width = w;
  if (height)
    *height = h;
}

void WKWebViewBackend::GetWindowOuterSize(uint32_t window_id, int* width,
                                          int* height) {
  __block int w = 0, h = 0;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      NSRect frame = [state->window frame];
      w = static_cast<int>(frame.size.width);
      h = static_cast<int>(frame.size.height);
    }
  });
  if (width)
    *width = w;
  if (height)
    *height = h;
}

void WKWebViewBackend::SetWindowPosition(uint32_t window_id, int x, int y) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        NSRect frame = [state->window frame];
        CGFloat flippedY = PrimaryScreenHeight() - y - frame.size.height;
        [state->window setFrameOrigin:NSMakePoint(x, flippedY)];
      }
    }
  });
}

void WKWebViewBackend::GetWindowInnerPosition(uint32_t window_id, int* x,
                                              int* y) {
  __block int px = 0, py = 0;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      NSRect content =
          [state->window contentRectForFrameRect:[state->window frame]];
      px = static_cast<int>(content.origin.x);
      py = static_cast<int>(PrimaryScreenHeight() - content.origin.y -
                            content.size.height);
    }
  });
  if (x)
    *x = px;
  if (y)
    *y = py;
}

void WKWebViewBackend::GetWindowPosition(uint32_t window_id, int* x, int* y) {
  __block int px = 0, py = 0;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      NSRect frame = [state->window frame];
      px = static_cast<int>(frame.origin.x);
      py = static_cast<int>(PrimaryScreenHeight() - frame.origin.y -
                            frame.size.height);
    }
  });
  if (x)
    *x = px;
  if (y)
    *y = py;
}

void WKWebViewBackend::SetResizable(uint32_t window_id, bool resizable) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        NSWindowStyleMask mask = [state->window styleMask];
        if (resizable) {
          mask |= NSWindowStyleMaskResizable;
        } else {
          mask &= ~NSWindowStyleMaskResizable;
        }
        [state->window setStyleMask:mask];
      }
    }
  });
}

bool WKWebViewBackend::IsResizable(uint32_t window_id) {
  __block bool result = false;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = ([state->window styleMask] & NSWindowStyleMaskResizable) != 0;
    }
  });
  return result;
}

void WKWebViewBackend::SetAlwaysOnTop(uint32_t window_id, bool always_on_top) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        [state->window setLevel:always_on_top ? NSFloatingWindowLevel
                                              : NSNormalWindowLevel];
      }
    }
  });
}

bool WKWebViewBackend::IsAlwaysOnTop(uint32_t window_id) {
  __block bool result = false;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = [state->window level] >= NSFloatingWindowLevel;
    }
  });
  return result;
}

void WKWebViewBackend::SetWindowOpacity(uint32_t window_id, double opacity) {
  if (opacity < 0.0)
    opacity = 0.0;
  if (opacity > 1.0)
    opacity = 1.0;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        [state->window setAlphaValue:(CGFloat)opacity];
      }
    }
  });
}

double WKWebViewBackend::GetWindowOpacity(uint32_t window_id) {
  __block double result = 1.0;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = (double)[state->window alphaValue];
    }
  });
  return result;
}

void WKWebViewBackend::SetClickPassthrough(uint32_t window_id, bool enabled) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        [state->window setIgnoresMouseEvents:enabled];
      }
    }
  });
}

bool WKWebViewBackend::IsClickPassthrough(uint32_t window_id) {
  __block bool result = false;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = [state->window ignoresMouseEvents];
    }
  });
  return result;
}

void WKWebViewBackend::SetClickPassthroughForward(uint32_t window_id,
                                                  bool forward) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        auto* state = GetWindow(window_id);
        if (!state)
          return;
        state->click_passthrough_forward = forward;
      }
      UpdateForwardMonitors();
    }
  });
}

bool WKWebViewBackend::IsClickPassthroughForward(uint32_t window_id) {
  __block bool result = false;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = state->click_passthrough_forward;
    }
  });
  return result;
}

uint32_t WKWebViewBackend::ForwardTargetAt(NSPoint screen_point,
                                           NSWindow** out_win) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  // orderedWindows is front-to-back, so overlapping forwarding windows
  // resolve to the one visually on top.
  for (NSWindow* win in [NSApp orderedWindows]) {
    uint32_t wid = LaufeyIdForNSWindow(win);
    if (wid == 0)
      continue;
    auto it = windows_.find(wid);
    if (it == windows_.end())
      continue;
    // Forwarding only reports events the OS routed elsewhere *because of
    // passthrough*; without ignoresMouseEvents an event over this frame that
    // reached another app was simply over an occluding window.
    if (!it->second.click_passthrough_forward || ![win ignoresMouseEvents] ||
        ![win isVisible])
      continue;
    if (!NSPointInRect(screen_point, [win frame]))
      continue;
    if (out_win)
      *out_win = win;
    return wid;
  }
  return 0;
}

void WKWebViewBackend::UpdateForwardMonitors() {
  bool any_forwarding = false;
  {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    for (auto& [id, state] : windows_) {
      if (state.click_passthrough_forward) {
        any_forwarding = true;
        break;
      }
    }
  }

  if (!any_forwarding) {
    if (forward_mouse_monitor_) {
      [NSEvent removeMonitor:forward_mouse_monitor_];
      forward_mouse_monitor_ = nil;
    }
    if (forward_mouse_move_monitor_) {
      [NSEvent removeMonitor:forward_mouse_move_monitor_];
      forward_mouse_move_monitor_ = nil;
    }
    if (forward_scroll_monitor_) {
      [NSEvent removeMonitor:forward_scroll_monitor_];
      forward_scroll_monitor_ = nil;
    }
    return;
  }
  if (forward_mouse_monitor_)
    return;

  // Global monitors observe events delivered to OTHER applications — exactly
  // what a passthrough window's events become. (They never fire for events
  // this app receives itself, so there is no double dispatch with the local
  // monitors above.) A global monitor event carries no NSWindow, so
  // locationInWindow is already in screen coordinates.
  forward_mouse_monitor_ = [NSEvent
      addGlobalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown |
                                             NSEventMaskLeftMouseUp |
                                             NSEventMaskRightMouseDown |
                                             NSEventMaskRightMouseUp |
                                             NSEventMaskOtherMouseDown |
                                             NSEventMaskOtherMouseUp)
                                    handler:^(NSEvent* event) {
                                      NSPoint screen_point =
                                          [event window]
                                              ? [[event window]
                                                    convertPointToScreen:
                                                        [event
                                                            locationInWindow]]
                                              : [event locationInWindow];
                                      NSWindow* win = nil;
                                      uint32_t wid =
                                          ForwardTargetAt(screen_point, &win);
                                      if (wid == 0)
                                        return;

                                      int state;
                                      switch ([event type]) {
                                        case NSEventTypeLeftMouseDown:
                                        case NSEventTypeRightMouseDown:
                                        case NSEventTypeOtherMouseDown:
                                          state = LAUFEY_MOUSE_PRESSED;
                                          break;
                                        default:
                                          state = LAUFEY_MOUSE_RELEASED;
                                          break;
                                      }
                                      int button = NSButtonToLaufey(
                                          [event buttonNumber]);
                                      uint32_t modifiers =
                                          NSModifierFlagsToLaufey(
                                              [event modifierFlags]);
                                      int32_t click_count = ResolveClickCount(
                                          state, (int32_t)[event clickCount]);

                                      NSPoint local = [win
                                          convertPointFromScreen:screen_point];
                                      double x = local.x;
                                      double y =
                                          [win contentLayoutRect].size.height -
                                          local.y;

                                      RuntimeLoader::GetInstance()
                                          ->DispatchMouseClickEvent(
                                              wid, state, button, x, y,
                                              modifiers, click_count);
                                    }];

  forward_mouse_move_monitor_ = [NSEvent
      addGlobalMonitorForEventsMatchingMask:(NSEventMaskMouseMoved |
                                             NSEventMaskLeftMouseDragged |
                                             NSEventMaskRightMouseDragged |
                                             NSEventMaskOtherMouseDragged)
                                    handler:^(NSEvent* event) {
                                      NSPoint screen_point =
                                          [event window]
                                              ? [[event window]
                                                    convertPointToScreen:
                                                        [event
                                                            locationInWindow]]
                                              : [event locationInWindow];
                                      NSWindow* win = nil;
                                      uint32_t wid =
                                          ForwardTargetAt(screen_point, &win);
                                      if (wid == 0)
                                        return;

                                      uint32_t modifiers =
                                          NSModifierFlagsToLaufey(
                                              [event modifierFlags]);
                                      NSPoint local = [win
                                          convertPointFromScreen:screen_point];
                                      double x = local.x;
                                      double y =
                                          [win contentLayoutRect].size.height -
                                          local.y;

                                      RuntimeLoader::GetInstance()
                                          ->DispatchMouseMoveEvent(wid, x, y,
                                                                   modifiers);
                                    }];

  forward_scroll_monitor_ = [NSEvent
      addGlobalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                    handler:^(NSEvent* event) {
                                      NSPoint screen_point =
                                          [event window]
                                              ? [[event window]
                                                    convertPointToScreen:
                                                        [event
                                                            locationInWindow]]
                                              : [event locationInWindow];
                                      NSWindow* win = nil;
                                      uint32_t wid =
                                          ForwardTargetAt(screen_point, &win);
                                      if (wid == 0)
                                        return;

                                      double delta_x = [event scrollingDeltaX];
                                      double delta_y = [event scrollingDeltaY];
                                      uint32_t modifiers =
                                          NSModifierFlagsToLaufey(
                                              [event modifierFlags]);
                                      int32_t delta_mode =
                                          [event hasPreciseScrollingDeltas]
                                              ? LAUFEY_WHEEL_DELTA_PIXEL
                                              : LAUFEY_WHEEL_DELTA_LINE;

                                      NSPoint local = [win
                                          convertPointFromScreen:screen_point];
                                      double x = local.x;
                                      double y =
                                          [win contentLayoutRect].size.height -
                                          local.y;

                                      RuntimeLoader::GetInstance()
                                          ->DispatchWheelEvent(
                                              wid, delta_x, delta_y, x, y,
                                              modifiers, delta_mode);
                                    }];
}

bool WKWebViewBackend::IsVisible(uint32_t window_id) {
  __block bool result = false;
  laufey_common::RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = [state->window isVisible];
    }
  });
  return result;
}

void WKWebViewBackend::Show(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSWindow* win = nil;
      WKWebView* web = nil;
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        auto* state = GetWindow(window_id);
        if (state) {
          win = state->window;
          web = state->webview;
        }
      }
      if (!win)
        return;
      if ([win styleMask] & NSWindowStyleMaskNonactivatingPanel) {
        // A tray / menu-bar panel never activates the app.
        [win makeKeyAndOrderFront:nil];
      } else {
        // The reveal of a window created hidden (Deno Desktop shows its
        // first window once the page has loaded): at launch it must come
        // in front of the app the user started it from.
        laufey_common::MacRevealWindowAtLaunch((__bridge void*)win);
      }
      if (web)
        [win makeFirstResponder:web];
    }
  });
}

void WKWebViewBackend::Hide(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSWindow* win = nil;
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        auto* state = GetWindow(window_id);
        if (state) {
          win = state->window;
        }
      }
      if (!win)
        return;
      [win orderOut:nil];
    }
  });
}

void WKWebViewBackend::Focus(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSWindow* win = nil;
      WKWebView* web = nil;
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        auto* state = GetWindow(window_id);
        if (state) {
          win = state->window;
          web = state->webview;
        }
      }
      if (win) {
        [NSApp activateIgnoringOtherApps:YES];
        [win makeKeyAndOrderFront:nil];
        if (web)
          [win makeFirstResponder:web];
      }
    }
  });
}

bool WKWebViewBackend::PostUiTask(void (*task)(void*), void* data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    task(data);
  });
  return true;
}

void WKWebViewBackend::InvokeJsCallback(uint32_t window_id,
                                        uint64_t callback_id,
                                        laufey::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  std::string script = BuildInvokeCallbackScript(callback_id, argsJson);
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      NSString* js = [NSString stringWithUTF8String:script.c_str()];
      // window_id == 0 means broadcast to all windows
      if (window_id == 0) {
        for (auto& [wid, state] : windows_) {
          [state.webview evaluateJavaScript:js completionHandler:nil];
        }
      } else {
        auto* state = GetWindow(window_id);
        if (state) {
          [state->webview evaluateJavaScript:js completionHandler:nil];
        }
      }
    }
  });
}

void WKWebViewBackend::ReleaseJsCallback(uint32_t window_id,
                                         uint64_t callback_id) {
  std::string script = BuildReleaseCallbackScript(callback_id);
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      NSString* js = [NSString stringWithUTF8String:script.c_str()];
      if (window_id == 0) {
        for (auto& [wid, state] : windows_) {
          [state.webview evaluateJavaScript:js completionHandler:nil];
        }
      } else {
        auto* state = GetWindow(window_id);
        if (state) {
          [state->webview evaluateJavaScript:js completionHandler:nil];
        }
      }
    }
  });
}

void WKWebViewBackend::RespondToJsCall(uint32_t window_id, uint64_t call_id,
                                       laufey::ValuePtr result,
                                       laufey::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson = error ? json::Serialize(error) : "null";
  std::string script = BuildRespondScript(call_id, resultJson, errorJson,
                                          static_cast<bool>(error));
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state)
        return;
      NSString* js = [NSString stringWithUTF8String:script.c_str()];
      [state->webview evaluateJavaScript:js completionHandler:nil];
    }
  });
}

void WKWebViewBackend::Run() {
  @autoreleasepool {
    [NSApp run];
  }
}

void WKWebViewBackend::HandleJsMessage(uint32_t window_id, uint64_t call_id,
                                       const std::string& method,
                                       laufey::ValuePtr args) {
  RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args);
}

// --- Application Menu / Context Menu ---
//
// Menu construction lives in backend-common
// (laufey_common::BuildNSMenuFromValue).

void WKWebViewBackend::SetApplicationMenu(uint32_t window_id,
                                          laufey_value_t* menu_template,
                                          const laufey_backend_api_t* api,
                                          laufey_menu_click_fn on_click,
                                          void* on_click_data) {
  // Parsed here: the template is the caller's only for this call.
  auto entries = std::make_shared<std::vector<laufey_common::MenuEntry>>(
      laufey_common::ParseMenuTemplate(menu_template, api, true));
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menubar = laufey_common::BuildNSMenuFromEntries(
        *entries, on_click, on_click_data, window_id);
    EnsureEditMenu(menubar);
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state)
        return;
      state->menu = menubar;
    }
    laufey_common::RegisterWindowMenuMac(window_id, menubar);
    // If this window is currently the key window, apply immediately
    NSWindow* keyWin = [NSApp keyWindow];
    uint32_t keyWid = LaufeyIdForNSWindow(keyWin);
    if (keyWid == window_id) {
      [NSApp setMainMenu:menubar];
    }
  });
}

void WKWebViewBackend::ShowContextMenu(uint32_t window_id, int x, int y,
                                       laufey_value_t* menu_template,
                                       const laufey_backend_api_t* api,
                                       laufey_menu_click_fn on_click,
                                       void* on_click_data) {
  ShowContextMenuEx(window_id, x, y, menu_template, api, on_click,
                    on_click_data, nullptr, nullptr);
}

void WKWebViewBackend::ShowContextMenuEx(uint32_t window_id, int x, int y,
                                         laufey_value_t* menu_template,
                                         const laufey_backend_api_t* api,
                                         laufey_menu_click_fn on_click,
                                         void* on_click_data,
                                         laufey_menu_closed_fn on_closed,
                                         void* on_closed_data) {
  auto entries = std::make_shared<std::vector<laufey_common::MenuEntry>>(
      laufey_common::ParseMenuTemplate(menu_template, api, true));
  // Not a main-queue block: the menu's tracking loop must leave the main
  // queue free for the UI-thread calls made while it is open.
  laufey_common::RunFromMainRunLoopMac([this, window_id, x, y, entries,
                                        on_click, on_click_data, on_closed,
                                        on_closed_data] {
    NSWindow* win = nil;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state)
        win = state->window;
    }
    // The content view is the WKWebView (flipped: top-left, as the page).
    laufey_common::ShowContextMenuMac(
        win ? (__bridge void*)[win contentView] : nullptr, x, y, *entries,
        on_click, on_click_data, on_closed, on_closed_data, window_id);
  });
}

// Runs `block` on the main thread and waits (inline when already there; see
// laufey_ui_tasks.h).
using laufey_common::RunOnMainSync;

// The web view's _WKInspector (private; what Safari's Develop menu drives),
// or nil.
static id InspectorOf(WKWebView* webview) {
  @try {
    return [webview valueForKey:@"_inspector"];
  } @catch (NSException*) {
    return nil;
  }
}

// The window's web view, read under the lock. Callers act on it after the
// lock is released: opening or closing the inspector changes the key window,
// and the key-window observers take the same lock.
WKWebView* WKWebViewBackend::WebViewOf(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? state->webview : nil;
}

void WKWebViewBackend::OpenDevTools(uint32_t window_id) {
  if (!laufey_common::LaunchInspectable())
    return;
  dispatch_async(dispatch_get_main_queue(), ^{
    WKWebView* webview = WebViewOf(window_id);
    if (!webview)
      return;
    // WKWebView._inspector.show is available on macOS 13.3+
    @try {
      id inspector = [webview valueForKey:@"_inspector"];
      if (inspector) {
        [inspector performSelector:@selector(show)];
      }
    } @catch (NSException*) {
      // Fallback: not available on this macOS version
    }
  });
}

void WKWebViewBackend::CloseDevTools(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    WKWebView* webview = WebViewOf(window_id);
    if (!webview)
      return;
    id inspector = InspectorOf(webview);
    if (inspector && [inspector respondsToSelector:@selector(close)])
      [inspector performSelector:@selector(close)];
  });
}

bool WKWebViewBackend::IsDevToolsOpen(uint32_t window_id) {
  __block bool open = false;
  RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state || !state->webview)
      return;
    id inspector = InspectorOf(state->webview);
    SEL visible = NSSelectorFromString(@"isVisible");
    if (inspector && [inspector respondsToSelector:visible]) {
      open =
          reinterpret_cast<BOOL (*)(id, SEL)>(objc_msgSend)(inspector, visible);
    }
  });
  return open;
}

bool WKWebViewBackend::IsDevToolsEnabled(uint32_t window_id) {
  if (window_id == 0)
    return laufey_common::LaunchInspectable();
  __block bool enabled = false;
  RunOnMainSync(^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (!state || !state->webview)
      return;
    // Read back what the engine was given.
    if ([state->webview respondsToSelector:@selector(isInspectable)]) {
      enabled = reinterpret_cast<BOOL (*)(id, SEL)>(objc_msgSend)(
          state->webview, @selector(isInspectable));
      return;
    }
    @try {
      enabled = [[state->webview.configuration.preferences
          valueForKey:@"developerExtrasEnabled"] boolValue];
    } @catch (NSException*) {
    }
  });
  return enabled;
}

int WKWebViewBackend::ShowDialog(uint32_t /*window_id*/, int dialog_type,
                                 const std::string& title,
                                 const std::string& message,
                                 const std::string& default_value,
                                 char** out_input_value) {
  return laufey_common::ShowDialogMac(dialog_type, title, message,
                                      default_value, out_input_value);
}

// --- Dock (macOS) ---
//
// Dock menu + reopen handler storage lives in backend-common; the
// AppDelegate in main_mac.mm reads via laufey_common::{Get,Set,Fire}*.

void WKWebViewBackend::SetDockBadge(const char* badge_or_null) {
  laufey_common::SetDockBadgeMac(badge_or_null);
}

void WKWebViewBackend::BounceDock(int type) {
  laufey_common::BounceDockMac(type);
}

void WKWebViewBackend::SetDockMenu(laufey_value_t* menu_template,
                                   const laufey_backend_api_t* api,
                                   laufey_menu_click_fn on_click,
                                   void* on_click_data) {
  if (!menu_template) {
    dispatch_async(dispatch_get_main_queue(), ^{
      laufey_common::SetDockMenuMac(nil);
    });
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    // window_id = 0 because the dock menu is app-scoped.
    NSMenu* menu = laufey_common::BuildNSMenuFromValue(
        menu_template, api, on_click, on_click_data, 0);
    laufey_common::SetDockMenuMac(menu);
  });
}

void WKWebViewBackend::SetDockVisible(bool visible) {
  laufey_common::SetDockVisibleMac(visible);
}

void WKWebViewBackend::SetDockReopenHandler(laufey_dock_reopen_fn handler,
                                            void* user_data) {
  laufey_common::SetDockReopenHandlerMac(handler, user_data);
}

// --- Deep links / custom URL schemes (macOS) ---
//
// Storage and the cold-start buffer live in backend-common; AppDelegate's
// application:openURLs: (main_mac.mm) is what feeds them.

void WKWebViewBackend::SetOpenUrlHandler(laufey_open_url_fn handler,
                                         void* user_data) {
  laufey_common::SetOpenUrlHandlerMac(handler, user_data);
}

bool WKWebViewBackend::TestTriggerOpenUrl(const char* url) {
  return laufey_common::TestTriggerOpenUrlMac(url);
}

// --- Tray / status-bar icon (macOS) ---
//
// Thin trampolines over backend-common/src/tray_mac.mm.

// --- Window state, constraints, screens and chrome (API >= 38) ---
//
// The NSWindow work is laufey_common's (window_mac.mm, shared with CEF);
// these resolve the window on the main thread and hand it over.

// RunOnMainSync (above, with the DevTools code) runs the block on the main
// thread and waits.

// ---------------------------------------------------------------------------
// Drag and drop, file dialogs (API >= 39)
// ---------------------------------------------------------------------------

bool WKWebViewBackend::TestTriggerFileDrop(uint32_t window_id, int phase,
                                           double x, double y,
                                           const char* const* paths,
                                           size_t count) {
  // The OS path dispatches on the main thread; so does the hook.
  __block bool delivered = false;
  void (^body)(void) = ^{
    delivered = laufey_common::TestTriggerFileDrop(window_id, phase, x, y,
                                                   paths, count);
  };
  laufey_common::RunOnMainSync(body);
  return delivered;
}

void WKWebViewBackend::StartFileDrag(uint32_t window_id,
                                     const char* const* paths, size_t count,
                                     const uint8_t* icon_png, size_t icon_len,
                                     laufey_drag_result_fn callback,
                                     void* user_data) {
  auto* req = new laufey_common::DragOutRequest();
  req->callback = callback;
  req->user_data = user_data;
  WKWebView* webview = nil;
  {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    if (auto* state = GetWindow(window_id))
      webview = state->webview;
  }
  if (!webview ||
      !laufey_common::ValidateDragPaths(paths, count, &req->paths)) {
    req->Finish(LAUFEY_DRAG_RESULT_FAILED);
    delete req;
    return;
  }
  if (icon_png && icon_len > 0)
    req->icon_png.assign(icon_png, icon_png + icon_len);
  laufey_common::StartFileDragMac((__bridge void*)webview, req);
}

uint32_t WKWebViewBackend::ShowFileDialog(
    uint32_t window_id, const laufey_file_dialog_options_t* options,
    laufey_file_dialog_result_fn callback, void* user_data) {
  laufey_common::ParentResolver parent;
  if (window_id != 0) {
    parent = [this, window_id]() -> void* {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      return state ? (__bridge void*)state->window : nullptr;
    };
  }
  return laufey_common::ShowFileDialogMac(std::move(parent), options, callback,
                                          user_data);
}

uint32_t WKWebViewBackend::WindowCapabilities() {
  return LAUFEY_WINDOW_CAP_STATE | LAUFEY_WINDOW_CAP_STATE_EVENTS |
         LAUFEY_WINDOW_CAP_SIZE_CONSTRAINTS | LAUFEY_WINDOW_CAP_SCREENS |
         LAUFEY_WINDOW_CAP_DISPLAY_EVENTS | LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN |
         LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN_INSET |
         LAUFEY_WINDOW_CAP_TRAFFIC_LIGHT_POSITION | LAUFEY_WINDOW_CAP_VIBRANCY |
         LAUFEY_WINDOW_CAP_NORMAL_BOUNDS | LAUFEY_WINDOW_CAP_KEEP_ALIVE |
         LAUFEY_WINDOW_CAP_SET_POSITION | LAUFEY_WINDOW_CAP_FILE_DROP |
         LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS |
         LAUFEY_WINDOW_CAP_FILE_DRAG_OUT | LAUFEY_WINDOW_CAP_FILE_DIALOGS |
         LAUFEY_WINDOW_CAP_FILE_DIALOG_FILES_AND_DIRECTORIES |
         LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL;
}

void WKWebViewBackend::SetWindowState(uint32_t window_id, int action) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSWindow* window = nil;
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        if (auto* state = GetWindow(window_id))
          window = state->window;
      }
      // Outside the lock: zoom / miniaturize post notifications whose
      // observers take it.
      if (window)
        laufey_common::MacSetWindowState((__bridge void*)window, action);
    }
  });
}

uint32_t WKWebViewBackend::GetWindowState(uint32_t window_id) {
  __block uint32_t result = 0;
  RunOnMainSync(^{
    NSWindow* window = nil;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      if (auto* state = GetWindow(window_id))
        window = state->window;
    }
    if (window)
      result = laufey_common::MacGetWindowState((__bridge void*)window);
  });
  return result;
}

void WKWebViewBackend::SetWindowStateHandler(laufey_window_state_fn handler,
                                             void* user_data) {
  laufey_common::SetWindowStateHandler(handler, user_data);
}

void WKWebViewBackend::SetWindowSizeConstraints(uint32_t window_id,
                                                int min_width, int min_height,
                                                int max_width, int max_height) {
  laufey_common::SizeConstraints c = laufey_common::SetSizeConstraints(
      window_id, min_width, min_height, max_width, max_height);
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSWindow* window = nil;
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        if (auto* state = GetWindow(window_id))
          window = state->window;
      }
      if (window)
        laufey_common::MacApplySizeConstraints((__bridge void*)window, c);
    }
  });
}

void WKWebViewBackend::GetWindowSizeConstraints(uint32_t window_id,
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

size_t WKWebViewBackend::GetScreens(laufey_screen_t* out, size_t capacity) {
  return laufey_common::CopyScreens(laufey_common::MacGetScreens(), out,
                                    capacity);
}

int64_t WKWebViewBackend::GetWindowScreen(uint32_t window_id) {
  __block int64_t result = 0;
  RunOnMainSync(^{
    NSWindow* window = nil;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      if (auto* state = GetWindow(window_id))
        window = state->window;
    }
    if (window)
      result = laufey_common::MacScreenForWindow((__bridge void*)window);
  });
  return result;
}

void WKWebViewBackend::SetDisplayChangedHandler(
    laufey_display_changed_fn handler, void* user_data) {
  laufey_common::SetDisplayChangedHandler(handler, user_data);
  if (handler)
    laufey_common::MacInstallDisplayWatcher();
}

bool WKWebViewBackend::SetWindowTitlebarStyle(uint32_t window_id, int style) {
  __block bool result = false;
  RunOnMainSync(^{
    NSWindow* window = nil;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      if (auto* state = GetWindow(window_id))
        window = state->window;
    }
    if (window)
      result =
          laufey_common::MacSetTitlebarStyle((__bridge void*)window, style);
  });
  return result;
}

bool WKWebViewBackend::SetWindowTrafficLightPosition(uint32_t window_id, int x,
                                                     int y) {
  __block bool result = false;
  RunOnMainSync(^{
    NSWindow* window = nil;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      if (auto* state = GetWindow(window_id))
        window = state->window;
    }
    if (window)
      result = laufey_common::MacSetTrafficLightPosition((__bridge void*)window,
                                                         x, y);
  });
  return result;
}

bool WKWebViewBackend::SetWindowBackdrop(uint32_t window_id, int backdrop,
                                         int material) {
  __block bool result = false;
  RunOnMainSync(^{
    NSWindow* window = nil;
    WKWebView* webview = nil;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      if (auto* state = GetWindow(window_id)) {
        window = state->window;
        webview = state->webview;
      }
    }
    if (window && webview)
      result = laufey_common::MacSetVibrancy(
          (__bridge void*)window, (__bridge void*)webview, backdrop, material);
  });
  return result;
}

bool WKWebViewBackend::GetWindowNormalBounds(uint32_t window_id, int* x, int* y,
                                             int* width, int* height) {
  __block bool found = false;
  __block laufey_common::Bounds bounds;
  RunOnMainSync(^{
    NSWindow* window = nil;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      if (auto* state = GetWindow(window_id))
        window = state->window;
    }
    if (!window)
      return;
    found = true;
    if (laufey_common::MacGetWindowState((__bridge void*)window) == 0) {
      bounds = laufey_common::MacWindowBounds((__bridge void*)window);
    } else if (!laufey_common::GetCommittedNormalBounds(window_id, &bounds)) {
      bounds = laufey_common::MacWindowBounds((__bridge void*)window);
    }
  });
  if (!found)
    return false;
  if (x)
    *x = bounds.x;
  if (y)
    *y = bounds.y;
  if (width)
    *width = bounds.width;
  if (height)
    *height = bounds.height;
  return true;
}

void WKWebViewBackend::SetQuitOnLastWindowClosed(bool quit) {
  laufey_common::SetQuitOnLastWindowClosed(quit);
}

uint32_t WKWebViewBackend::CreateTrayIcon() {
  return laufey_common::CreateTrayIconMac();
}

void WKWebViewBackend::DestroyTrayIcon(uint32_t tray_id) {
  laufey_common::DestroyTrayIconMac(tray_id);
}

void WKWebViewBackend::SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                                   size_t len) {
  laufey_common::SetTrayIconMac(tray_id, png_bytes, len);
}

void WKWebViewBackend::SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                                       size_t len) {
  laufey_common::SetTrayIconDarkMac(tray_id, png_bytes, len);
}

bool WKWebViewBackend::GetTrayIconBounds(uint32_t tray_id, int* x, int* y,
                                         int* width, int* height) {
  return laufey_common::GetTrayIconBoundsMac(tray_id, x, y, width, height);
}

void WKWebViewBackend::SetTrayDoubleClickHandler(uint32_t tray_id,
                                                 laufey_tray_click_fn handler,
                                                 void* user_data) {
  laufey_common::SetTrayDoubleClickHandlerMac(tray_id, handler, user_data);
}

void WKWebViewBackend::SetTrayTooltip(uint32_t tray_id,
                                      const char* tooltip_or_null) {
  laufey_common::SetTrayTooltipMac(tray_id, tooltip_or_null);
}

void WKWebViewBackend::SetTrayMenu(uint32_t tray_id,
                                   laufey_value_t* menu_template,
                                   const laufey_backend_api_t* api,
                                   laufey_menu_click_fn on_click,
                                   void* on_click_data) {
  laufey_common::SetTrayMenuMac(tray_id, menu_template, api, on_click,
                                on_click_data);
}

void WKWebViewBackend::SetTrayClickHandler(uint32_t tray_id,
                                           laufey_tray_click_fn handler,
                                           void* user_data) {
  laufey_common::SetTrayClickHandlerMac(tray_id, handler, user_data);
}
// --- Notifications (macOS WebView) ---
//
// Thin trampolines over backend-common/src/notifications_mac.mm
// (UNUserNotificationCenter-backed). Migrated from NSUserNotification
// (deprecated in macOS 11) to align with the CEF backend.

uint32_t WKWebViewBackend::ShowNotification(
    laufey_value_t* options, const laufey_backend_api_t* api,
    laufey_notification_event_fn on_event, void* user_data) {
  laufey_common::NotificationOptions opts =
      laufey_common::ParseNotificationOptions(options, api);
  return laufey_common::ShowNotification(opts, on_event, user_data);
}

void WKWebViewBackend::CloseNotification(uint32_t notification_id) {
  laufey_common::CloseNotification(notification_id);
}

// Notification permissions: UNUserNotificationCenter, app-scoped (the
// process, not the page).
void WKWebViewBackend::QueryPermission(int kind,
                                       laufey_permission_callback_fn cb,
                                       void* user_data) {
  laufey_common::QueryNotificationPermission(kind, cb, user_data);
}

void WKWebViewBackend::RequestPermission(int kind,
                                         laufey_permission_callback_fn cb,
                                         void* user_data) {
  laufey_common::RequestNotificationPermission(kind, cb, user_data);
}

LaufeyBackend* CreateLaufeyBackend() {
  return new WKWebViewBackend();
}
