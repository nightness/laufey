// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_RUNTIME_LOADER_H_
#define LAUFEY_RUNTIME_LOADER_H_

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <map>

#include "laufey.h"
#include "scheme_exchange.h"
#include "webview_value.h"

#ifdef _WIN32
// <windows.h> defines CreateWindow/CreateWindowEx as macros that collide with
// our backend methods; pull it in once and drop them.
#include <windows.h>
#undef CreateWindow
#undef CreateWindowEx
#endif

class LaufeyBackend;

class RuntimeLoader {
 public:
  static RuntimeLoader* GetInstance();

  bool Load(const std::string& path);

  // Static-link path (iOS): the runtime is linked into the app binary, so the
  // entry points are resolved at link time instead of via dlopen.
  void LoadStatic(laufey_runtime_init_fn init, laufey_runtime_start_fn start,
                  laufey_runtime_shutdown_fn shutdown) {
    init_fn_ = init;
    start_fn_ = start;
    shutdown_fn_ = shutdown;
  }

  bool Start();

  void Shutdown();

  void SetBackend(LaufeyBackend* backend) {
    backend_ = backend;
  }
  LaufeyBackend* GetBackend() {
    return backend_;
  }
  const laufey_backend_api_t& GetBackendApi() const {
    return backend_api_;
  }

  uint32_t AllocateWindowId() {
    return next_window_id_.fetch_add(1);
  }

  void StoreCallWindow(uint64_t call_id, uint32_t window_id) {
    std::lock_guard<std::mutex> lock(call_map_mutex_);
    call_to_window_[call_id] = window_id;
  }

  uint32_t ConsumeCallWindow(uint64_t call_id) {
    std::lock_guard<std::mutex> lock(call_map_mutex_);
    auto it = call_to_window_.find(call_id);
    if (it != call_to_window_.end()) {
      uint32_t wid = it->second;
      call_to_window_.erase(it);
      return wid;
    }
    return 0;
  }

  void OnJsCall(uint32_t window_id, uint64_t call_id,
                const std::string& method_path, laufey::ValuePtr args);

  void PollPendingJsCalls();

  void JsCallRespond(uint32_t window_id, uint64_t call_id,
                     laufey::ValuePtr result, laufey::ValuePtr error);

  void SetJsCallHandler(laufey_js_call_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    js_call_handler_ = handler;
    js_call_user_data_ = user_data;
  }

  // --- Custom URL scheme handler (API >= 26) ---
  // Store the runtime's scheme request handler and ask the platform backend to
  // install its native handler for `scheme`.
  void SetSchemeRequestHandler(const std::string& scheme,
                               laufey_scheme_request_fn handler,
                               laufey_scheme_cancel_fn on_cancel,
                               void* user_data);

  // Invoke the registered scheme request handler. Called by the platform
  // backend when the webview issues a request for the registered scheme.
  void DispatchSchemeRequest(uint32_t window_id, SchemeExchangeBase* exchange,
                             const std::string& method, const std::string& url,
                             const std::string& flat_headers);

  void SetKeyboardEventHandler(laufey_keyboard_event_fn handler,
                               void* user_data) {
    std::lock_guard<std::mutex> lock(keyboard_mutex_);
    keyboard_handler_ = handler;
    keyboard_user_data_ = user_data;
  }

  void DispatchKeyboardEvent(uint32_t window_id, int state, const char* key,
                             const char* code, uint32_t modifiers,
                             bool repeat) {
    std::lock_guard<std::mutex> lock(keyboard_mutex_);
    if (keyboard_handler_) {
      keyboard_handler_(keyboard_user_data_, window_id, state, key, code,
                        modifiers, repeat);
    }
  }

  void SetMouseClickHandler(laufey_mouse_click_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(mouse_mutex_);
    mouse_click_handler_ = handler;
    mouse_click_user_data_ = user_data;
  }

  void DispatchMouseClickEvent(uint32_t window_id, int state, int button,
                               double x, double y, uint32_t modifiers,
                               int32_t click_count) {
    std::lock_guard<std::mutex> lock(mouse_mutex_);
    if (mouse_click_handler_) {
      mouse_click_handler_(mouse_click_user_data_, window_id, state, button, x,
                           y, modifiers, click_count);
    }
  }

  void SetMouseMoveHandler(laufey_mouse_move_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(mouse_move_mutex_);
    mouse_move_handler_ = handler;
    mouse_move_user_data_ = user_data;
  }

  void DispatchMouseMoveEvent(uint32_t window_id, double x, double y,
                              uint32_t modifiers) {
    std::lock_guard<std::mutex> lock(mouse_move_mutex_);
    if (mouse_move_handler_) {
      mouse_move_handler_(mouse_move_user_data_, window_id, x, y, modifiers);
    }
  }

  void SetWheelHandler(laufey_wheel_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(wheel_mutex_);
    wheel_handler_ = handler;
    wheel_user_data_ = user_data;
  }

  void DispatchWheelEvent(uint32_t window_id, double delta_x, double delta_y,
                          double x, double y, uint32_t modifiers,
                          int32_t delta_mode) {
    std::lock_guard<std::mutex> lock(wheel_mutex_);
    if (wheel_handler_) {
      wheel_handler_(wheel_user_data_, window_id, delta_x, delta_y, x, y,
                     modifiers, delta_mode);
    }
  }

  void SetCursorEnterLeaveHandler(laufey_cursor_enter_leave_fn handler,
                                  void* user_data) {
    std::lock_guard<std::mutex> lock(cursor_enter_leave_mutex_);
    cursor_enter_leave_handler_ = handler;
    cursor_enter_leave_user_data_ = user_data;
  }

  void DispatchCursorEnterLeaveEvent(uint32_t window_id, int entered, double x,
                                     double y, uint32_t modifiers) {
    std::lock_guard<std::mutex> lock(cursor_enter_leave_mutex_);
    if (cursor_enter_leave_handler_) {
      cursor_enter_leave_handler_(cursor_enter_leave_user_data_, window_id,
                                  entered, x, y, modifiers);
    }
  }

  void SetFocusedHandler(laufey_focused_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(focused_mutex_);
    focused_handler_ = handler;
    focused_user_data_ = user_data;
  }

  void DispatchFocusedEvent(uint32_t window_id, int focused) {
    std::lock_guard<std::mutex> lock(focused_mutex_);
    if (focused_handler_) {
      focused_handler_(focused_user_data_, window_id, focused);
    }
  }

  void SetResizeHandler(laufey_resize_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(resize_mutex_);
    resize_handler_ = handler;
    resize_user_data_ = user_data;
  }

  void DispatchResizeEvent(uint32_t window_id, int width, int height) {
    std::lock_guard<std::mutex> lock(resize_mutex_);
    if (resize_handler_) {
      resize_handler_(resize_user_data_, window_id, width, height);
    }
  }

  void SetMoveHandler(laufey_move_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(move_mutex_);
    move_handler_ = handler;
    move_user_data_ = user_data;
  }

  void DispatchMoveEvent(uint32_t window_id, int x, int y) {
    std::lock_guard<std::mutex> lock(move_mutex_);
    if (move_handler_) {
      move_handler_(move_user_data_, window_id, x, y);
    }
  }

  void SetCloseRequestedHandler(laufey_close_requested_fn handler,
                                void* user_data) {
    std::lock_guard<std::mutex> lock(close_requested_mutex_);
    close_requested_handler_ = handler;
    close_requested_user_data_ = user_data;
  }

  // Returns true if the caller should proceed to actually close the window.
  // A registered handler always defers the close (API >= 31): the app
  // decides later, out of band, by calling close_window. No handler means
  // proceed, unchanged from backends predating API 31.
  bool DispatchCloseRequestedEvent(uint32_t window_id) {
    // Copy the handler out and release the mutex before invoking it: the
    // handler may block (e.g. a modal confirm dialog) and pump OS events,
    // which can re-enter this dispatch on the same thread — with a
    // non-recursive mutex still held, that would self-deadlock.
    laufey_close_requested_fn handler;
    void* user_data;
    {
      std::lock_guard<std::mutex> lock(close_requested_mutex_);
      handler = close_requested_handler_;
      user_data = close_requested_user_data_;
    }
    if (handler) {
      handler(user_data, window_id);
      return false;
    }
    return true;
  }

  void SetPageLoadHandler(laufey_page_load_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(page_load_mutex_);
    page_load_handler_ = handler;
    page_load_user_data_ = user_data;
  }

  void DispatchPageLoadEvent(uint32_t window_id) {
    std::lock_guard<std::mutex> lock(page_load_mutex_);
    if (page_load_handler_) {
      page_load_handler_(page_load_user_data_, window_id);
    }
  }

  void SetJsCallNotify(void (*notify_fn)(void*), void* notify_data) {
    std::lock_guard<std::mutex> lock(notify_mutex_);
    js_call_notify_fn_ = notify_fn;
    js_call_notify_data_ = notify_data;
  }

  void SetJsNamespace(const std::string& name) {
    std::lock_guard<std::mutex> lock(js_namespace_mutex_);
    js_namespace_ = name;
  }
  std::string GetJsNamespace() const {
    std::lock_guard<std::mutex> lock(js_namespace_mutex_);
    return js_namespace_;
  }

 private:
  RuntimeLoader();
  ~RuntimeLoader();

  void RuntimeThread();
  void InitializeBackendApi();

  void* library_handle_ = nullptr;
  laufey_runtime_init_fn init_fn_ = nullptr;
  laufey_runtime_start_fn start_fn_ = nullptr;
  laufey_runtime_shutdown_fn shutdown_fn_ = nullptr;

  std::thread runtime_thread_;
  std::atomic<bool> running_{false};

  LaufeyBackend* backend_ = nullptr;
  laufey_backend_api_t backend_api_;

  laufey_js_call_fn js_call_handler_ = nullptr;
  void* js_call_user_data_ = nullptr;
  std::mutex handler_mutex_;

  laufey_scheme_request_fn scheme_request_handler_ = nullptr;
  laufey_scheme_cancel_fn scheme_cancel_handler_ = nullptr;
  void* scheme_user_data_ = nullptr;
  std::mutex scheme_mutex_;

  laufey_keyboard_event_fn keyboard_handler_ = nullptr;
  void* keyboard_user_data_ = nullptr;
  std::mutex keyboard_mutex_;

  laufey_mouse_click_fn mouse_click_handler_ = nullptr;
  void* mouse_click_user_data_ = nullptr;
  std::mutex mouse_mutex_;

  laufey_mouse_move_fn mouse_move_handler_ = nullptr;
  void* mouse_move_user_data_ = nullptr;
  std::mutex mouse_move_mutex_;

  laufey_wheel_fn wheel_handler_ = nullptr;
  void* wheel_user_data_ = nullptr;
  std::mutex wheel_mutex_;

  laufey_cursor_enter_leave_fn cursor_enter_leave_handler_ = nullptr;
  void* cursor_enter_leave_user_data_ = nullptr;
  std::mutex cursor_enter_leave_mutex_;

  laufey_focused_fn focused_handler_ = nullptr;
  void* focused_user_data_ = nullptr;
  std::mutex focused_mutex_;

  laufey_resize_fn resize_handler_ = nullptr;
  void* resize_user_data_ = nullptr;
  std::mutex resize_mutex_;

  laufey_move_fn move_handler_ = nullptr;
  void* move_user_data_ = nullptr;
  std::mutex move_mutex_;

  laufey_close_requested_fn close_requested_handler_ = nullptr;
  void* close_requested_user_data_ = nullptr;
  std::mutex close_requested_mutex_;

  laufey_page_load_fn page_load_handler_ = nullptr;
  void* page_load_user_data_ = nullptr;
  std::mutex page_load_mutex_;

  std::atomic<uint32_t> next_window_id_{1};
  std::map<uint64_t, uint32_t> call_to_window_;
  std::mutex call_map_mutex_;

  void (*js_call_notify_fn_)(void*) = nullptr;
  void* js_call_notify_data_ = nullptr;
  std::mutex notify_mutex_;

  std::string js_namespace_ = "Laufey";
  mutable std::mutex js_namespace_mutex_;

  struct PendingJsCall {
    uint32_t window_id;
    uint64_t call_id;
    std::string method_path;
    laufey::ValuePtr args;
  };
  std::queue<PendingJsCall> pending_js_calls_;
  std::mutex pending_mutex_;

  static RuntimeLoader* instance_;
};

class LaufeyBackend {
 public:
  virtual ~LaufeyBackend() = default;

  // Window lifecycle
  virtual void CreateWindow(uint32_t window_id, int width, int height) = 0;
  // Create with creation-time style flags (LAUFEY_WINDOW_FLAG_*). Default
  // ignores the flags and creates a plain window; platform backends override
  // to honor frameless / non-activating-panel flags.
  virtual void CreateWindowEx(uint32_t window_id, int width, int height,
                              uint32_t /*flags*/) {
    CreateWindow(window_id, width, height);
  }
  virtual void CloseWindow(uint32_t window_id) = 0;

  // Per-window operations
  virtual void Navigate(uint32_t window_id, const std::string& url) = 0;
  virtual void SetTitle(uint32_t window_id, const std::string& title) = 0;
  virtual void ExecuteJs(uint32_t window_id, const std::string& script,
                         laufey_js_result_fn callback, void* callback_data) = 0;
  virtual void SetWindowSize(uint32_t window_id, int width, int height) = 0;
  virtual void GetWindowSize(uint32_t window_id, int* width, int* height) = 0;
  // Chrome-inclusive size in the same space as GetWindowSize. Default is
  // the content size (no client chrome).
  virtual void GetWindowOuterSize(uint32_t window_id, int* width, int* height) {
    GetWindowSize(window_id, width, height);
  }
  // Physical pixels per DIP (`window.devicePixelRatio`). Default 1.0.
  virtual double GetWindowScaleFactor(uint32_t /*window_id*/) {
    return 1.0;
  }
  virtual void SetWindowPosition(uint32_t window_id, int x, int y) = 0;
  virtual void GetWindowPosition(uint32_t window_id, int* x, int* y) = 0;
  // Content-view origin in the same space as GetWindowPosition. Default is
  // the frame origin (no chrome offset).
  virtual void GetWindowInnerPosition(uint32_t window_id, int* x, int* y) {
    GetWindowPosition(window_id, x, y);
  }
  virtual void SetResizable(uint32_t window_id, bool resizable) = 0;
  virtual bool IsResizable(uint32_t window_id) = 0;
  virtual void SetAlwaysOnTop(uint32_t window_id, bool always_on_top) = 0;
  virtual bool IsAlwaysOnTop(uint32_t window_id) = 0;
  // Overall window opacity in [0.0, 1.0] (1.0 == fully opaque). Fades the whole
  // window uniformly, unlike LAUFEY_WINDOW_FLAG_TRANSPARENT. Default no-op /
  // reports fully opaque; platform backends override.
  virtual void SetWindowOpacity(uint32_t /*window_id*/, double /*opacity*/) {}
  virtual double GetWindowOpacity(uint32_t /*window_id*/) {
    return 1.0;
  }
  // Click passthrough (API >= 33): while enabled the window ignores all mouse
  // input and events fall through to whatever is beneath it. Default no-op /
  // reports disabled; platform backends override.
  virtual void SetClickPassthrough(uint32_t /*window_id*/, bool /*enabled*/) {}
  virtual bool IsClickPassthrough(uint32_t /*window_id*/) {
    return false;
  }
  // Click passthrough forwarding (API >= 34): while passthrough is enabled,
  // keep the embedder's mouse handlers fed for this window from a global OS
  // observer even though the OS delivers the events elsewhere. Observation
  // only. Default no-op / reports disabled; platforms with global input
  // observation (macOS) override.
  virtual void SetClickPassthroughForward(uint32_t /*window_id*/,
                                          bool /*forward*/) {}
  virtual bool IsClickPassthroughForward(uint32_t /*window_id*/) {
    return false;
  }
  virtual bool IsVisible(uint32_t window_id) = 0;
  virtual void Show(uint32_t window_id) = 0;
  virtual void Hide(uint32_t window_id) = 0;
  virtual void Focus(uint32_t window_id) = 0;

  // Global operations
  virtual void Quit() = 0;
  virtual void PostUiTask(void (*task)(void*), void* data) = 0;
  virtual void Run() = 0;

  // JS interop (broadcast to all windows for callback operations)
  virtual void InvokeJsCallback(uint32_t window_id, uint64_t callback_id,
                                laufey::ValuePtr args) = 0;
  virtual void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) = 0;
  virtual void RespondToJsCall(uint32_t window_id, uint64_t call_id,
                               laufey::ValuePtr result,
                               laufey::ValuePtr error) = 0;

  virtual void SetApplicationMenu(uint32_t window_id,
                                  laufey_value_t* menu_template,
                                  const laufey_backend_api_t* api,
                                  laufey_menu_click_fn on_click,
                                  void* on_click_data) = 0;

  virtual void ShowContextMenu(uint32_t window_id, int x, int y,
                               laufey_value_t* menu_template,
                               const laufey_backend_api_t* api,
                               laufey_menu_click_fn on_click,
                               void* on_click_data) = 0;

  virtual void OpenDevTools(uint32_t window_id) = 0;

  // Render the window's current page to a PDF (API >= 32). The PDF bytes are
  // delivered through `callback` on success; file output is handled by the
  // capi layer, never here. Asynchronous: `callback` fires on the UI thread
  // once rendering completes, and implementations must invoke it exactly once
  // on every path. Default: report "unsupported" through the callback rather
  // than crashing; platform backends override.
  virtual void PrintToPdf(uint32_t /*window_id*/, laufey_pdf_result_fn callback,
                          void* callback_data) {
    if (callback)
      callback(nullptr, 0, "print_to_pdf is not supported by this backend",
               callback_data);
  }

  // Open `url` in the user's default OS browser. Used to honor the
  // external-link redirect policy (see laufey_external_links.h) so clicked
  // links and `target="_blank"` popups leave the app's webview. Default no-op;
  // platform backends override with the native open-in-browser call.
  virtual void OpenExternalURL(const std::string& /*url*/) {}

  // --- Custom URL scheme handler (API >= 26) ---
  // Install a native handler for `scheme` so requests to <scheme>://… are
  // delivered to RuntimeLoader::DispatchSchemeRequest. Default no-op: backends
  // that don't implement it leave the scheme unhandled.
  virtual void RegisterSchemeHandler(const std::string& /*scheme*/) {}

  // Show a modal dialog and BLOCK until the user dismisses it. Backends
  // run the platform's native modal loop (`runModal` / `MessageBoxW` /
  // `gtk_dialog_run`), which itself pumps OS events while the dialog is
  // up so other LAUFEY windows continue to render and respond.
  // Returns 1 if OK was pressed, 0 otherwise. For prompts, on a confirmed
  // result `*out_input_value` is set to a `strdup`'d UTF-8 string the
  // caller frees via `BackendStringFree`. NULL otherwise.
  virtual int ShowDialog(uint32_t window_id, int dialog_type,
                         const std::string& title, const std::string& message,
                         const std::string& default_value,
                         char** out_input_value) = 0;

  // --- Clipboard (system) ---
  // Plain-text access to the system clipboard. `ReadClipboardText` returns a
  // `malloc`'d / `strdup`'d UTF-8 string the caller frees with `free()` (via
  // BackendStringFree), or NULL when the clipboard is empty or holds no text.
  // Default implementations are inert so platforms without support inherit
  // silently.
  virtual char* ReadClipboardText() {
    return nullptr;
  }
  virtual void WriteClipboardText(const std::string& /*text*/) {}

  // --- Dock / taskbar ---
  // Default implementations are no-ops so platforms that don't support a
  // given operation inherit silently. The canonical macOS implementation is
  // in WKWebViewBackend; Windows/Linux override Bounce only.
  virtual void SetDockBadge(const char* /*badge_or_null*/) {}
  virtual void BounceDock(int /*type*/) {}
  virtual void SetDockMenu(laufey_value_t* /*menu_template*/,
                           const laufey_backend_api_t* /*api*/,
                           laufey_menu_click_fn /*on_click*/,
                           void* /*on_click_data*/) {}
  virtual void SetDockVisible(bool /*visible*/) {}
  virtual void SetDockReopenHandler(laufey_dock_reopen_fn /*handler*/,
                                    void* /*user_data*/) {}

  // --- Deep links / custom URL schemes ---
  // macOS only (AppKit's application:openURLs:); Windows/Linux get the URL as
  // argv in a brand-new process, which is the embedder's to handle. The
  // default no-op leaves the C ABI pointer inert on those platforms.
  virtual void SetOpenUrlHandler(laufey_open_url_fn /*handler*/,
                                 void* /*user_data*/) {}
  virtual bool TestTriggerOpenUrl(const char* /*url*/) {
    return false;
  }

  // --- Single instance ---
  // Desktop backends forward to laufey_common::SetSecondInstanceHandler
  // (backend-common, which iOS doesn't link); the default no-op leaves the
  // handler inert.
  virtual void SetSecondInstanceHandler(laufey_second_instance_fn /*handler*/,
                                        void* /*user_data*/) {}

  // --- Passkeys (API >= 37) ---
  // The macOS and Windows backends override both with laufey_passkey.h
  // (backend-common, which iOS doesn't link). The defaults are the answer of
  // a platform without a passkey API (Linux, iOS): no capabilities, and every
  // request refused with not_supported before its options are read (the text
  // of laufey_common::PasskeyReportNotSupported).
  virtual uint32_t PasskeyCapabilities() {
    return 0;
  }
  virtual void PasskeyRequest(uint32_t /*window_id*/, uint32_t /*kind*/,
                              const char* /*options_json*/,
                              laufey_passkey_result_fn callback,
                              void* user_data) {
    if (callback) {
      callback(user_data,
               "{\"ok\":false,\"error\":{\"code\":\"not_supported\","
               "\"message\":\"Native passkeys are not supported on this "
               "platform.\"}}");
    }
  }

  // --- Drag and drop, file dialogs, rich clipboard (API >= 39) ---
  // See laufey.h. The desktop backends override these with laufey_io.h
  // (backend-common, which iOS doesn't link). The defaults are a backend that
  // has none of it: no drop events, drag-out and dialogs answering FAILED
  // (exactly once, on the calling thread), plain-text clipboard only.
  virtual void SetFileDropHandler(laufey_file_drop_fn /*handler*/,
                                  void* /*user_data*/) {}
  virtual bool TestTriggerFileDrop(uint32_t /*window_id*/, int /*phase*/,
                                   double /*x*/, double /*y*/,
                                   const char* const* /*paths*/,
                                   size_t /*count*/) {
    return false;
  }
  virtual void StartFileDrag(uint32_t /*window_id*/,
                             const char* const* /*paths*/, size_t /*count*/,
                             const uint8_t* /*icon_png*/, size_t /*icon_len*/,
                             laufey_drag_result_fn callback, void* user_data) {
    if (callback)
      callback(user_data, LAUFEY_DRAG_RESULT_FAILED);
  }
  virtual uint32_t ShowFileDialog(uint32_t /*window_id*/,
                                  const laufey_file_dialog_options_t* /*opts*/,
                                  laufey_file_dialog_result_fn callback,
                                  void* user_data) {
    if (callback)
      callback(user_data, 0, LAUFEY_FILE_DIALOG_FAILED, nullptr, 0);
    return 0;
  }
  virtual bool CancelFileDialog(uint32_t /*dialog_id*/) {
    return false;
  }
  virtual bool TestFileDialogRespond(int /*action*/, const char* /*path*/) {
    return false;
  }
  virtual uint32_t ClipboardCapabilities() {
    return LAUFEY_CLIPBOARD_CAP_TEXT;
  }
  virtual char* ReadClipboardHtml() {
    return nullptr;
  }
  virtual bool WriteClipboardHtml(const std::string& /*html*/,
                                  const char* /*text_or_null*/) {
    return false;
  }
  virtual uint8_t* ReadClipboardImage(size_t* len_out) {
    if (len_out)
      *len_out = 0;
    return nullptr;
  }
  virtual bool WriteClipboardImage(const uint8_t* /*png*/, size_t /*len*/) {
    return false;
  }
  virtual char* ReadClipboardFormats() {
    return nullptr;
  }
  virtual void SetClipboardChangeHandler(laufey_clipboard_change_fn /*fn*/,
                                         void* /*user_data*/) {}

  // --- Global shortcuts, launch at login, DevTools (API >= 40) ---
  // See laufey.h. The desktop backends override these with laufey_system.h
  // (backend-common, which iOS doesn't link). The defaults are a backend that
  // has none of it: registrations answer NOT_SUPPORTED (exactly once, on the
  // calling thread), launch at login is NOT_SUPPORTED, DevTools stay closed.
  virtual uint32_t SystemCapabilities() {
    return 0;
  }
  virtual void SetShortcutHandler(laufey_shortcut_fn /*handler*/,
                                  void* /*user_data*/) {}
  virtual void RegisterShortcut(const char* /*accelerator*/,
                                laufey_shortcut_result_fn callback,
                                void* user_data) {
    if (callback)
      callback(user_data, LAUFEY_SHORTCUT_NOT_SUPPORTED, nullptr);
  }
  virtual bool UnregisterShortcut(const char* /*accelerator*/) {
    return false;
  }
  virtual void UnregisterAllShortcuts() {}
  virtual char* ListShortcuts() {
    char* s = static_cast<char*>(malloc(1));
    if (s)
      s[0] = 0;
    return s;
  }
  virtual char* CanonicalizeAccelerator(const char* /*accelerator*/) {
    return nullptr;
  }
  virtual bool TestTriggerShortcut(const char* /*accelerator*/) {
    return false;
  }
  virtual int GetLaunchAtLogin() {
    return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
  }
  virtual int SetLaunchAtLogin(bool /*enabled*/, std::string* /*error*/) {
    return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
  }
  virtual void CloseDevTools(uint32_t /*window_id*/) {}
  virtual bool IsDevToolsOpen(uint32_t /*window_id*/) {
    return false;
  }
  virtual bool IsDevToolsEnabled(uint32_t /*window_id*/) {
    return false;
  }

  // --- Window state, constraints, screens and chrome (API >= 38) ---
  // See laufey.h. The defaults are a backend that can do none of it (iOS):
  // no capabilities, every setter a no-op or false, getters "unknown".
  virtual uint32_t WindowCapabilities() {
    return 0;
  }
  virtual void SetWindowState(uint32_t /*window_id*/, int /*action*/) {}
  virtual uint32_t GetWindowState(uint32_t /*window_id*/) {
    return 0;
  }
  virtual void SetWindowStateHandler(laufey_window_state_fn /*handler*/,
                                     void* /*user_data*/) {}
  virtual void SetWindowSizeConstraints(uint32_t /*window_id*/,
                                        int /*min_width*/, int /*min_height*/,
                                        int /*max_width*/, int /*max_height*/) {
  }
  virtual void GetWindowSizeConstraints(uint32_t /*window_id*/, int* min_width,
                                        int* min_height, int* max_width,
                                        int* max_height) {
    for (int* p : {min_width, min_height, max_width, max_height}) {
      if (p)
        *p = 0;
    }
  }
  virtual size_t GetScreens(laufey_screen_t* /*out*/, size_t /*capacity*/) {
    return 0;
  }
  virtual int64_t GetWindowScreen(uint32_t /*window_id*/) {
    return 0;
  }
  virtual void SetDisplayChangedHandler(laufey_display_changed_fn /*handler*/,
                                        void* /*user_data*/) {}
  virtual bool SetWindowTitlebarStyle(uint32_t /*window_id*/, int /*style*/) {
    return false;
  }
  virtual bool SetWindowTrafficLightPosition(uint32_t /*window_id*/, int /*x*/,
                                             int /*y*/) {
    return false;
  }
  virtual bool SetWindowBackdrop(uint32_t /*window_id*/, int /*backdrop*/,
                                 int /*material*/) {
    return false;
  }
  virtual bool GetWindowNormalBounds(uint32_t /*window_id*/, int* /*x*/,
                                     int* /*y*/, int* /*width*/,
                                     int* /*height*/) {
    return false;
  }
  virtual void SetQuitOnLastWindowClosed(bool /*quit*/) {}

  // --- Auth session (API >= 42) ---
  // The macOS backend overrides both with laufey_auth_session.h
  // (ASWebAuthenticationSession). The defaults are the answer of a platform
  // without an OS auth session (Windows, Linux, iOS): no capabilities, every
  // request NOT_SUPPORTED (RFC 8252: the embedder opens the system browser).
  virtual uint32_t AuthSessionCapabilities() {
    return 0;
  }
  virtual void AuthSessionStart(uint32_t /*window_id*/, const char* /*url*/,
                                const char* /*callback*/, uint32_t /*flags*/,
                                laufey_auth_session_result_fn on_result,
                                void* user_data) {
    if (on_result) {
      on_result(user_data, LAUFEY_AUTH_SESSION_NOT_SUPPORTED,
                "this platform has no OS auth session; open the system "
                "browser and receive the redirect through a loopback or "
                "custom-scheme listener (RFC 8252)");
    }
  }

  // --- Tray / status-bar icon ---
  virtual uint32_t CreateTrayIcon() {
    return 0;
  }
  virtual void DestroyTrayIcon(uint32_t /*tray_id*/) {}
  virtual void SetTrayIcon(uint32_t /*tray_id*/, const void* /*png_bytes*/,
                           size_t /*len*/) {}
  virtual void SetTrayTooltip(uint32_t /*tray_id*/,
                              const char* /*tooltip_or_null*/) {}
  virtual void SetTrayMenu(uint32_t /*tray_id*/,
                           laufey_value_t* /*menu_template*/,
                           const laufey_backend_api_t* /*api*/,
                           laufey_menu_click_fn /*on_click*/,
                           void* /*on_click_data*/) {}
  virtual void SetTrayClickHandler(uint32_t /*tray_id*/,
                                   laufey_tray_click_fn /*handler*/,
                                   void* /*user_data*/) {}
  virtual void SetTrayDoubleClickHandler(uint32_t /*tray_id*/,
                                         laufey_tray_click_fn /*handler*/,
                                         void* /*user_data*/) {}
  virtual void SetTrayIconDark(uint32_t /*tray_id*/, const void* /*png_bytes*/,
                               size_t /*len*/) {}
  // Tray icon screen bounds (top-left origin, DIP). Default: unsupported.
  virtual bool GetTrayIconBounds(uint32_t /*tray_id*/, int* /*x*/, int* /*y*/,
                                 int* /*width*/, int* /*height*/) {
    return false;
  }

  // --- Notifications ---
  // Default: not supported. Subclasses override per-platform.
  virtual uint32_t ShowNotification(laufey_value_t* options,
                                    const laufey_backend_api_t* /*api*/,
                                    laufey_notification_event_fn /*on_event*/,
                                    void* /*user_data*/) {
    // Subclass owns the options pointer if it accepts the call. Default
    // path frees so we don't leak.
    (void)options;
    return 0;
  }
  virtual void CloseNotification(uint32_t /*notification_id*/) {}

  // --- Notifications: scheduling, actions, responses (API >= 41) ---
  // See laufey.h. The desktop backends override these with
  // laufey_notifications.h (backend-common, which iOS doesn't link). The
  // defaults are a backend without notifications.
  virtual uint32_t NotificationCapabilities() {
    return 0;
  }
  virtual void SetNotificationResponseHandler(
      laufey_notification_response_fn /*handler*/, void* /*user_data*/) {}
  virtual void ListScheduledNotifications(laufey_notification_list_fn cb,
                                          void* user_data) {
    if (cb)
      cb(user_data, "[]");
  }
  virtual void CancelNotification(const char* /*tag*/) {}
  virtual bool TestNotificationRespond(const char* /*tag*/,
                                       const char* /*action_id*/) {
    return false;
  }

  // --- Menus: context-menu close, accelerators (API >= 41) ---
  // The defaults are a backend without them: a context menu request that
  // shows nothing reports its close at once.
  virtual uint32_t MenuCapabilities() {
    return 0;
  }
  virtual void ShowContextMenuEx(uint32_t window_id, int /*x*/, int /*y*/,
                                 laufey_value_t* /*menu_template*/,
                                 const laufey_backend_api_t* /*api*/,
                                 laufey_menu_click_fn /*on_click*/,
                                 void* /*on_click_data*/,
                                 laufey_menu_closed_fn on_closed,
                                 void* on_closed_data) {
    if (on_closed)
      on_closed(on_closed_data, window_id);
  }
  virtual bool TestDismissContextMenu() {
    return false;
  }
  virtual bool TestTriggerMenuAccelerator(uint32_t /*window_id*/,
                                          const char* /*accelerator*/) {
    return false;
  }

  // --- Permissions / runtime authorization ---
  // Default: synchronously report UNSUPPORTED. The desktop backends
  // override it with laufey_notifications.h (UNUserNotificationCenter, the
  // toast setting, a notification server on the session bus).
  virtual void QueryPermission(int /*kind*/, laufey_permission_callback_fn cb,
                               void* user_data) {
    if (cb)
      cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
  virtual void RequestPermission(int /*kind*/, laufey_permission_callback_fn cb,
                                 void* user_data) {
    if (cb)
      cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
};

LaufeyBackend* CreateLaufeyBackend();

// Returns the path to a runtime library co-located with the running executable
// and sharing its base name (e.g. example.exe -> example.dll, ./foo -> foo.so),
// or "" if none exists. Lets a renamed single-exe auto-load its runtime without
// a --runtime flag or wrapper script.
std::string LaufeyFindColocatedRuntime();

#endif  // LAUFEY_RUNTIME_LOADER_H_
