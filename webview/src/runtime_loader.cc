// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"

#include "laufey_backend_common.h"
#include "laufey_external_links.h"
#include "laufey_auth_session.h"
#include "laufey_ui_tasks.h"

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#else
#include <windows.h>
// windows.h defines CreateWindow as a macro which conflicts with
// LaufeyBackend::CreateWindow
#undef CreateWindow
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <iostream>
#include <cstring>
#include <vector>

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
  std::string candidate = base + ".dll";
#elif defined(__APPLE__)
  std::string candidate = base + ".dylib";
#else
  std::string candidate = base + ".so";
#endif

  if (PathExists(candidate))
    return candidate;
  return "";
}

static void Backend_Navigate(void* data, uint32_t window_id, const char* url) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && url) {
    backend->Navigate(window_id, url);
  }
}

static void Backend_SetTitle(void* data, uint32_t window_id,
                             const char* title) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && title) {
    backend->SetTitle(window_id, title);
  }
}

static void Backend_ExecuteJs(void* data, uint32_t window_id,
                              const char* script, laufey_js_result_fn callback,
                              void* callback_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && script) {
    backend->ExecuteJs(window_id, script, callback, callback_data);
  }
}

static void Backend_Quit(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Quit();
  }
}

static void Backend_SetWindowSize(void* data, uint32_t window_id, int width,
                                  int height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetWindowSize(window_id, width, height);
  }
}

static void Backend_GetWindowSize(void* data, uint32_t window_id, int* width,
                                  int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowSize(window_id, width, height);
  }
}

static void Backend_GetWindowOuterSize(void* data, uint32_t window_id,
                                       int* width, int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowOuterSize(window_id, width, height);
  }
}

static void Backend_SetWindowPosition(void* data, uint32_t window_id, int x,
                                      int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetWindowPosition(window_id, x, y);
  }
}

static void Backend_GetWindowPosition(void* data, uint32_t window_id, int* x,
                                      int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowPosition(window_id, x, y);
  }
}

static void Backend_GetWindowInnerPosition(void* data, uint32_t window_id,
                                           int* x, int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowInnerPosition(window_id, x, y);
  }
}

static void Backend_SetResizable(void* data, uint32_t window_id,
                                 bool resizable) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetResizable(window_id, resizable);
  }
}

static bool Backend_IsResizable(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsResizable(window_id);
  }
  return false;
}

static void Backend_SetAlwaysOnTop(void* data, uint32_t window_id,
                                   bool always_on_top) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetAlwaysOnTop(window_id, always_on_top);
  }
}

static bool Backend_IsAlwaysOnTop(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsAlwaysOnTop(window_id);
  }
  return false;
}

static void Backend_SetWindowOpacity(void* data, uint32_t window_id,
                                     double opacity) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetWindowOpacity(window_id, opacity);
  }
}

static double Backend_GetWindowOpacity(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->GetWindowOpacity(window_id);
  }
  return 1.0;
}

static double Backend_GetWindowScaleFactor(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->GetWindowScaleFactor(window_id);
  }
  return 1.0;
}

static void Backend_SetClickPassthrough(void* data, uint32_t window_id,
                                        bool enabled) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetClickPassthrough(window_id, enabled);
  }
}

static bool Backend_IsClickPassthrough(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsClickPassthrough(window_id);
  }
  return false;
}

static void Backend_SetClickPassthroughForward(void* data, uint32_t window_id,
                                               bool forward) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetClickPassthroughForward(window_id, forward);
  }
}

static bool Backend_IsClickPassthroughForward(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsClickPassthroughForward(window_id);
  }
  return false;
}

static bool Backend_IsVisible(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsVisible(window_id);
  }
  return false;
}

static void Backend_Show(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Show(window_id);
  }
}

static void Backend_Hide(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Hide(window_id);
  }
}

static void Backend_Focus(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Focus(window_id);
  }
}

static void Backend_PostUiTask(void* data, void (*task)(void*),
                               void* task_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && task) {
    backend->PostUiTask(task, task_data);
  }
}

static void Backend_SetJsCallHandler(void* data, laufey_js_call_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandler(handler, user_data);
}

static void Backend_JsCallRespond(void* data, uint64_t call_id,
                                  laufey_value_t* result,
                                  laufey_value_t* error) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->ConsumeCallWindow(call_id);
  laufey::ValuePtr resultPtr =
      (result && result->value) ? result->value : laufey::Value::Null();
  // Keep the absent-error case as a genuine null pointer. RespondToJsCall on
  // macOS/Windows decides resolve-vs-reject by the pointer's truthiness, so
  // fabricating a Value::Null() here would make every response look like a
  // rejection and resolve the JS promise with null.
  laufey::ValuePtr errorPtr = (error && error->value) ? error->value : nullptr;
  loader->JsCallRespond(window_id, call_id, resultPtr, errorPtr);
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
  return reinterpret_cast<SchemeExchangeBase*>(exchange)->ReadRequestBody(buf,
                                                                          cap);
}

static void Backend_SchemeResponseBegin(void* /*data*/,
                                        laufey_scheme_exchange_t* exchange,
                                        int status, const char* headers,
                                        size_t headers_len) {
  reinterpret_cast<SchemeExchangeBase*>(exchange)->Begin(status, headers,
                                                         headers_len);
}

static intptr_t Backend_SchemeResponseWrite(void* /*data*/,
                                            laufey_scheme_exchange_t* exchange,
                                            const uint8_t* buf, size_t len) {
  return reinterpret_cast<SchemeExchangeBase*>(exchange)->WriteResponse(buf,
                                                                        len);
}

static void Backend_SchemeResponseFinish(void* /*data*/,
                                         laufey_scheme_exchange_t* exchange) {
  reinterpret_cast<SchemeExchangeBase*>(exchange)->Finish();
}

static void Backend_InvokeJsCallback(void* data, uint64_t callback_id,
                                     laufey_value_t* args) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    laufey::ValuePtr argsPtr =
        (args && args->value) ? args->value : laufey::Value::List();
    // Broadcast to window 0 (all windows) since callback_id isn't tied to a
    // window
    backend->InvokeJsCallback(0, callback_id, argsPtr);
  }
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
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->ReleaseJsCallback(0, callback_id);
  }
}

static void Backend_PollJsCalls(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->PollPendingJsCalls();
}

static void Backend_SetJsCallNotify(void* data, void (*notify_fn)(void*),
                                    void* notify_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallNotify(notify_fn, notify_data);
}

static void Backend_SetApplicationMenu(void* data, uint32_t window_id,
                                       laufey_value_t* menu_template,
                                       laufey_menu_click_fn on_click,
                                       void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && menu_template) {
    backend->SetApplicationMenu(window_id, menu_template,
                                &loader->GetBackendApi(), on_click,
                                on_click_data);
  }
}

static void Backend_ShowContextMenu(void* data, uint32_t window_id, int x,
                                    int y, laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && menu_template) {
    backend->ShowContextMenu(window_id, x, y, menu_template,
                             &loader->GetBackendApi(), on_click, on_click_data);
  }
}

static void Backend_OpenDevTools(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->OpenDevTools(window_id);
  }
}

static void Backend_PrintToPdf(void* data, uint32_t window_id,
                               laufey_pdf_result_fn callback,
                               void* callback_data) {
  if (!callback)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->PrintToPdf(window_id, callback, callback_data);
  } else {
    callback(nullptr, 0, "backend not initialized", callback_data);
  }
}

static void Backend_SetJsNamespace(void* data, const char* name) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (name) {
    loader->SetJsNamespace(name);
  }
}

static uint32_t Backend_CreateWindow(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->AllocateWindowId();
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->CreateWindow(window_id, 800, 600);
  }
  return window_id;
}

static uint32_t Backend_CreateWindowEx(void* data, uint32_t flags) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->AllocateWindowId();
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->CreateWindowEx(window_id, 800, 600, flags);
  }
  return window_id;
}

static bool Backend_GetTrayIconBounds(void* data, uint32_t tray_id, int* x,
                                      int* y, int* width, int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend) {
    return false;
  }
  return backend->GetTrayIconBounds(tray_id, x, y, width, height);
}

static void Backend_CloseWindow(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->CloseWindow(window_id);
  }
}

static void Backend_SetCloseRequestedHandler(void* data,
                                             laufey_close_requested_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCloseRequestedHandler(handler, user_data);
}

// Test hook (API >= 31): synthesize a close-requested event through the same
// dispatch code a real OS close click runs. Returns true if a registered
// handler deferred the close; false means the close proceeded. Proceeds
// through Backend_CloseWindow — the real close entry point — rather than
// re-inlining its body, so the hook can't silently diverge from the
// shipping close path.
static bool Backend_TestTriggerCloseRequested(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  bool proceed = loader->DispatchCloseRequestedEvent(window_id);
  if (proceed) {
    Backend_CloseWindow(data, window_id);
    return false;
  }
  return true;
}

static void Backend_SetPageLoadHandler(void* data, laufey_page_load_fn handler,
                                       void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetPageLoadHandler(handler, user_data);
}

static int Backend_ShowDialog(void* data, uint32_t window_id, int dialog_type,
                              const char* title, const char* message,
                              const char* default_value,
                              char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend)
    return 0;
  std::string t = title ? title : "";
  std::string m = message ? message : "";
  std::string d = default_value ? default_value : "";
  return backend->ShowDialog(window_id, dialog_type, t, m, d, out_input_value);
}

static void Backend_StringFree(void* /*backend_data*/, char* s) {
  if (s)
    free(s);
}

static char* Backend_ReadClipboardText(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->ReadClipboardText();
  return nullptr;
}

static void Backend_WriteClipboardText(void* data, const char* text) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->WriteClipboardText(text ? text : "");
}

// --- Dock / taskbar ---

static void Backend_SetDockBadge(void* data, const char* badge_or_null) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockBadge(badge_or_null);
  }
}

static void Backend_BounceDock(void* data, int type) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->BounceDock(type);
  }
}

static void Backend_SetDockMenu(void* data, laufey_value_t* menu_template,
                                laufey_menu_click_fn on_click,
                                void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockMenu(menu_template, &loader->GetBackendApi(), on_click,
                         on_click_data);
  }
}

static void Backend_SetDockVisible(void* data, bool visible) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockVisible(visible);
  }
}

static void Backend_SetDockReopenHandler(void* data,
                                         laufey_dock_reopen_fn handler,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockReopenHandler(handler, user_data);
  }
}

// --- Deep links / custom URL schemes (macOS only) ---
#if defined(__APPLE__)

static void Backend_SetOpenUrlHandler(void* data, laufey_open_url_fn handler,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetOpenUrlHandler(handler, user_data);
  }
}

static bool Backend_TestTriggerOpenUrl(void* data, const char* url) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    return backend->TestTriggerOpenUrl(url);
  }
  return false;
}

#endif  // defined(__APPLE__)

// --- Single instance ---

static void Backend_SetSecondInstanceHandler(void* data,
                                             laufey_second_instance_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetSecondInstanceHandler(handler, user_data);
  }
}

// --- Passkeys ---

static uint32_t Backend_PasskeyCapabilities(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->PasskeyCapabilities();
  return 0;
}

static void Backend_PasskeyRequest(void* data, uint32_t window_id,
                                   uint32_t kind, const char* options_json,
                                   laufey_passkey_result_fn callback,
                                   void* user_data) {
  if (!callback)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->PasskeyRequest(window_id, kind, options_json, callback, user_data);
  } else {
    callback(user_data,
             "{\"ok\":false,\"error\":{\"code\":\"unknown\","
             "\"message\":\"backend not initialized\"}}");
  }
}

// --- Drag and drop, file dialogs, rich clipboard (API >= 39) ---

static LaufeyBackend* BackendOf(void* data) {
  return static_cast<RuntimeLoader*>(data)->GetBackend();
}

static void Backend_SetFileDropHandler(void* data, laufey_file_drop_fn handler,
                                       void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->SetFileDropHandler(handler, user_data);
}

static bool Backend_TestTriggerFileDrop(void* data, uint32_t window_id,
                                        int phase, double x, double y,
                                        const char* const* paths,
                                        size_t count) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestTriggerFileDrop(window_id, phase, x, y, paths, count);
  return false;
}

static void Backend_StartFileDrag(void* data, uint32_t window_id,
                                  const char* const* paths, size_t count,
                                  const uint8_t* icon_png, size_t icon_len,
                                  laufey_drag_result_fn callback,
                                  void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data)) {
    backend->StartFileDrag(window_id, paths, count, icon_png, icon_len,
                           callback, user_data);
  } else if (callback) {
    callback(user_data, LAUFEY_DRAG_RESULT_FAILED);
  }
}

static uint32_t Backend_ShowFileDialog(
    void* data, uint32_t window_id, const laufey_file_dialog_options_t* options,
    laufey_file_dialog_result_fn callback, void* user_data) {
  if (!callback)
    return 0;
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ShowFileDialog(window_id, options, callback, user_data);
  callback(user_data, 0, LAUFEY_FILE_DIALOG_FAILED, nullptr, 0);
  return 0;
}

static bool Backend_CancelFileDialog(void* data, uint32_t dialog_id) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->CancelFileDialog(dialog_id);
  return false;
}

static bool Backend_TestFileDialogRespond(void* data, int action,
                                          const char* path) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestFileDialogRespond(action, path);
  return false;
}

static uint32_t Backend_ClipboardCapabilities(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ClipboardCapabilities();
  return 0;
}

static char* Backend_ReadClipboardHtml(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ReadClipboardHtml();
  return nullptr;
}

static bool Backend_WriteClipboardHtml(void* data, const char* html,
                                       const char* text_or_null) {
  if (!html)
    return false;
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->WriteClipboardHtml(html, text_or_null);
  return false;
}

static uint8_t* Backend_ReadClipboardImage(void* data, size_t* len_out) {
  if (len_out)
    *len_out = 0;
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ReadClipboardImage(len_out);
  return nullptr;
}

static bool Backend_WriteClipboardImage(void* data, const uint8_t* png,
                                        size_t len) {
  if (!png || len == 0)
    return false;
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->WriteClipboardImage(png, len);
  return false;
}

static char* Backend_ReadClipboardFormats(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ReadClipboardFormats();
  return nullptr;
}

static void Backend_SetClipboardChangeHandler(void* data,
                                              laufey_clipboard_change_fn fn,
                                              void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->SetClipboardChangeHandler(fn, user_data);
}

static void Backend_BufferFree(void* /*data*/, void* buffer) {
  free(buffer);
}

// --- Global shortcuts, launch at login, DevTools (API >= 40) ---

static uint32_t Backend_SystemCapabilities(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->SystemCapabilities();
  return 0;
}

static void Backend_SetShortcutHandler(void* data, laufey_shortcut_fn handler,
                                       void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->SetShortcutHandler(handler, user_data);
}

static void Backend_RegisterShortcut(void* data, const char* accelerator,
                                     laufey_shortcut_result_fn callback,
                                     void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data)) {
    backend->RegisterShortcut(accelerator, callback, user_data);
  } else if (callback) {
    callback(user_data, LAUFEY_SHORTCUT_NOT_SUPPORTED, nullptr);
  }
}

static bool Backend_UnregisterShortcut(void* data, const char* accelerator) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->UnregisterShortcut(accelerator);
  return false;
}

static void Backend_UnregisterAllShortcuts(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->UnregisterAllShortcuts();
}

static char* Backend_ListShortcuts(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ListShortcuts();
  return nullptr;
}

static char* Backend_CanonicalizeAccelerator(void* data,
                                             const char* accelerator) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->CanonicalizeAccelerator(accelerator);
  return nullptr;
}

static bool Backend_TestTriggerShortcut(void* data, const char* accelerator) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestTriggerShortcut(accelerator);
  return false;
}

static int Backend_GetLaunchAtLogin(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->GetLaunchAtLogin();
  return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
}

static int Backend_SetLaunchAtLogin(void* data, bool enabled,
                                    char** error_out) {
  if (error_out)
    *error_out = nullptr;
  LaufeyBackend* backend = BackendOf(data);
  if (!backend)
    return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
  std::string error;
  int state = backend->SetLaunchAtLogin(enabled, &error);
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
  if (LaufeyBackend* backend = BackendOf(data))
    backend->CloseDevTools(window_id);
}

static bool Backend_IsDevToolsOpen(void* data, uint32_t window_id) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->IsDevToolsOpen(window_id);
  return false;
}

static bool Backend_IsDevToolsEnabled(void* data, uint32_t window_id) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->IsDevToolsEnabled(window_id);
  return false;
}

// --- Window state, constraints, screens and chrome (API >= 38) ---

static uint32_t Backend_WindowCapabilities(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->WindowCapabilities();
  return 0;
}

static void Backend_SetWindowState(void* data, uint32_t window_id, int action) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetWindowState(window_id, action);
}

static uint32_t Backend_GetWindowState(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->GetWindowState(window_id);
  return 0;
}

static void Backend_SetWindowStateHandler(void* data,
                                          laufey_window_state_fn handler,
                                          void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetWindowStateHandler(handler, user_data);
}

static void Backend_SetWindowSizeConstraints(void* data, uint32_t window_id,
                                             int min_width, int min_height,
                                             int max_width, int max_height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetWindowSizeConstraints(window_id, min_width, min_height,
                                      max_width, max_height);
}

static void Backend_GetWindowSizeConstraints(void* data, uint32_t window_id,
                                             int* min_width, int* min_height,
                                             int* max_width, int* max_height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->GetWindowSizeConstraints(window_id, min_width, min_height,
                                      max_width, max_height);
    return;
  }
  for (int* p : {min_width, min_height, max_width, max_height}) {
    if (p)
      *p = 0;
  }
}

static size_t Backend_GetScreens(void* data, laufey_screen_t* out,
                                 size_t capacity) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->GetScreens(out, capacity);
  return 0;
}

static int64_t Backend_GetWindowScreen(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->GetWindowScreen(window_id);
  return 0;
}

static void Backend_SetDisplayChangedHandler(void* data,
                                             laufey_display_changed_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetDisplayChangedHandler(handler, user_data);
}

static bool Backend_SetWindowTitlebarStyle(void* data, uint32_t window_id,
                                           int style) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->SetWindowTitlebarStyle(window_id, style);
  return false;
}

static bool Backend_SetWindowTrafficLightPosition(void* data,
                                                  uint32_t window_id, int x,
                                                  int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->SetWindowTrafficLightPosition(window_id, x, y);
  return false;
}

static bool Backend_SetWindowBackdrop(void* data, uint32_t window_id,
                                      int backdrop, int material) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->SetWindowBackdrop(window_id, backdrop, material);
  return false;
}

static bool Backend_GetWindowNormalBounds(void* data, uint32_t window_id,
                                          int* x, int* y, int* width,
                                          int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->GetWindowNormalBounds(window_id, x, y, width, height);
  return false;
}

static void Backend_SetQuitOnLastWindowClosed(void* data, bool quit) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetQuitOnLastWindowClosed(quit);
}

// --- UI-thread tasks (API >= 42) ---

static void Backend_DispatchUiTask(void* /*data*/, laufey_ui_task_fn task,
                                   void* task_data) {
  laufey_common::UiTaskDispatcher::Get().Dispatch(task, task_data);
}

static bool Backend_IsUiThread(void* /*data*/) {
  return laufey_common::UiTaskDispatcher::Get().IsUiThread();
}

// --- Auth session (API >= 42) ---

static uint32_t Backend_AuthSessionCapabilities(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->AuthSessionCapabilities();
  return 0;
}

static void Backend_AuthSessionStart(void* data, uint32_t window_id,
                                     const char* url, const char* callback,
                                     uint32_t flags,
                                     laufey_auth_session_result_fn on_result,
                                     void* user_data) {
  if (!on_result)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->AuthSessionStart(window_id, url, callback, flags, on_result,
                              user_data);
  } else {
    on_result(user_data, LAUFEY_AUTH_SESSION_FAILED, "backend not initialized");
  }
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

// --- Tray / status bar ---

static uint32_t Backend_CreateTrayIcon(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->CreateTrayIcon();
  return 0;
}

static void Backend_DestroyTrayIcon(void* data, uint32_t tray_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->DestroyTrayIcon(tray_id);
}

static void Backend_SetTrayIcon(void* data, uint32_t tray_id,
                                const void* png_bytes, size_t len) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayIcon(tray_id, png_bytes, len);
}

static void Backend_SetTrayTooltip(void* data, uint32_t tray_id,
                                   const char* tooltip_or_null) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayTooltip(tray_id, tooltip_or_null);
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

static void Backend_SetTrayMenu(void* data, uint32_t tray_id,
                                laufey_value_t* menu_template,
                                laufey_menu_click_fn on_click,
                                void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayMenu(tray_id, menu_template, &loader->GetBackendApi(),
                         on_click, on_click_data);
}

static void Backend_SetTrayClickHandler(void* data, uint32_t tray_id,
                                        laufey_tray_click_fn handler,
                                        void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayClickHandler(tray_id, handler, user_data);
}

static void Backend_SetTrayDoubleClickHandler(void* data, uint32_t tray_id,
                                              laufey_tray_click_fn handler,
                                              void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayDoubleClickHandler(tray_id, handler, user_data);
}

static void Backend_SetTrayIconDark(void* data, uint32_t tray_id,
                                    const void* png_bytes, size_t len) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayIconDark(tray_id, png_bytes, len);
}

static uint32_t Backend_ShowNotification(void* data, laufey_value_t* options,
                                         laufey_notification_event_fn on_event,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend) {
    if (options) {
      loader->GetBackendApi().value_free(options);
    }
    return 0;
  }
  return backend->ShowNotification(options, &loader->GetBackendApi(), on_event,
                                   user_data);
}

static void Backend_CloseNotification(void* data, uint32_t notification_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->CloseNotification(notification_id);
}

static void Backend_QueryPermission(void* data, int kind,
                                    laufey_permission_callback_fn cb,
                                    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->QueryPermission(kind, cb, user_data);
  } else if (cb) {
    cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
}

static void Backend_RequestPermission(void* data, int kind,
                                      laufey_permission_callback_fn cb,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->RequestPermission(kind, cb, user_data);
  } else if (cb) {
    cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
}

// --- Menus and notifications (API >= 41) ---

static uint32_t Backend_MenuCapabilities(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->MenuCapabilities();
  return 0;
}

static void Backend_ShowContextMenuEx(void* data, uint32_t window_id, int x,
                                      int y, laufey_value_t* menu_template,
                                      laufey_menu_click_fn on_click,
                                      void* on_click_data,
                                      laufey_menu_closed_fn on_closed,
                                      void* on_closed_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend || !menu_template) {
    if (on_closed)
      on_closed(on_closed_data, window_id);
    return;
  }
  // Every backend parses the template before returning; it is ours to free.
  backend->ShowContextMenuEx(window_id, x, y, menu_template,
                             &loader->GetBackendApi(), on_click, on_click_data,
                             on_closed, on_closed_data);
  loader->GetBackendApi().value_free(menu_template);
}

static bool Backend_TestDismissContextMenu(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestDismissContextMenu();
  return false;
}

static bool Backend_TestTriggerMenuAccelerator(void* data, uint32_t window_id,
                                               const char* accelerator) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestTriggerMenuAccelerator(window_id, accelerator);
  return false;
}

static uint32_t Backend_NotificationCapabilities(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->NotificationCapabilities();
  return 0;
}

static void Backend_SetNotificationResponseHandler(
    void* data, laufey_notification_response_fn handler, void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->SetNotificationResponseHandler(handler, user_data);
}

static void Backend_ListScheduledNotifications(void* data,
                                               laufey_notification_list_fn cb,
                                               void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data)) {
    backend->ListScheduledNotifications(cb, user_data);
  } else if (cb) {
    cb(user_data, "[]");
  }
}

static void Backend_CancelNotification(void* data, const char* tag) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->CancelNotification(tag);
}

static bool Backend_TestNotificationRespond(void* data, const char* tag,
                                            const char* action_id) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestNotificationRespond(tag, action_id);
  return false;
}

void RuntimeLoader::InitializeBackendApi() {
  memset(&backend_api_, 0, sizeof(backend_api_));
  backend_api_.version = LAUFEY_API_VERSION;
  backend_api_.backend_data = this;
  backend_api_.test_click_menu_item = Backend_TestClickMenuItem;

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
  backend_api_.js_call_respond = Backend_JsCallRespond;

  backend_api_.register_scheme_handler = Backend_RegisterSchemeHandler;
  backend_api_.scheme_request_read_body = Backend_SchemeRequestReadBody;
  backend_api_.scheme_response_begin = Backend_SchemeResponseBegin;
  backend_api_.scheme_response_write = Backend_SchemeResponseWrite;
  backend_api_.scheme_response_finish = Backend_SchemeResponseFinish;

  backend_api_.invoke_js_callback = Backend_InvokeJsCallback;
  backend_api_.release_js_callback = Backend_ReleaseJsCallback;

  backend_api_.get_window_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
  backend_api_.get_display_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return LAUFEY_WINDOW_HANDLE_UNKNOWN;
  };

  backend_api_.set_keyboard_event_handler = Backend_SetKeyboardEventHandler;
  backend_api_.set_mouse_click_handler = Backend_SetMouseClickHandler;
  backend_api_.set_mouse_move_handler = Backend_SetMouseMoveHandler;
  backend_api_.set_wheel_handler = Backend_SetWheelHandler;
  backend_api_.set_cursor_enter_leave_handler =
      Backend_SetCursorEnterLeaveHandler;
  backend_api_.set_focused_handler = Backend_SetFocusedHandler;
  backend_api_.set_resize_handler = Backend_SetResizeHandler;
  backend_api_.set_move_handler = Backend_SetMoveHandler;
  backend_api_.poll_js_calls = Backend_PollJsCalls;
  backend_api_.set_js_call_notify = Backend_SetJsCallNotify;
  backend_api_.set_application_menu = Backend_SetApplicationMenu;
  backend_api_.show_context_menu = Backend_ShowContextMenu;
  backend_api_.open_devtools = Backend_OpenDevTools;
  backend_api_.print_to_pdf = Backend_PrintToPdf;
  backend_api_.set_js_namespace = Backend_SetJsNamespace;
  backend_api_.create_window = Backend_CreateWindow;
  backend_api_.create_window_ex = Backend_CreateWindowEx;
  backend_api_.close_window = Backend_CloseWindow;
  backend_api_.set_close_requested_handler = Backend_SetCloseRequestedHandler;
  backend_api_.test_trigger_close_requested = Backend_TestTriggerCloseRequested;
  backend_api_.test_inject_input = Backend_TestInjectInput;

  // Window state, constraints, screens and chrome (API >= 38): see
  // docs/window-management.md for what each OS / backend supports.
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
  backend_api_.set_page_load_handler = Backend_SetPageLoadHandler;
  backend_api_.show_dialog = Backend_ShowDialog;
  backend_api_.string_free = Backend_StringFree;
  backend_api_.read_clipboard_text = Backend_ReadClipboardText;
  backend_api_.write_clipboard_text = Backend_WriteClipboardText;

  backend_api_.set_dock_badge = Backend_SetDockBadge;
  backend_api_.bounce_dock = Backend_BounceDock;
  backend_api_.set_dock_menu = Backend_SetDockMenu;
  backend_api_.set_dock_visible = Backend_SetDockVisible;
  backend_api_.set_dock_reopen_handler = Backend_SetDockReopenHandler;

  // Deep links are macOS-only (see set_open_url_handler in laufey.h). Leave
  // the pointers NULL elsewhere so an embedder can detect the absence rather
  // than register a handler that silently never fires.
#if defined(__APPLE__)
  backend_api_.set_open_url_handler = Backend_SetOpenUrlHandler;
  backend_api_.test_trigger_open_url = Backend_TestTriggerOpenUrl;
#endif

  // Single instance (API >= 36): see docs/deep-links.md. The desktop
  // backends implement it on every OS; iOS keeps the no-op default.
  backend_api_.set_second_instance_handler = Backend_SetSecondInstanceHandler;

  // Passkeys (API >= 37): see docs/passkeys.md. macOS and Windows run real
  // ceremonies; Linux and iOS answer not_supported.
  backend_api_.passkey_capabilities = Backend_PasskeyCapabilities;
  backend_api_.passkey_request = Backend_PasskeyRequest;

  // Drag and drop, file dialogs and the rich clipboard (API >= 39): see
  // docs/drag-and-drop.md, docs/file-dialogs.md, docs/clipboard.md. The
  // desktop backends implement them over backend-common; iOS keeps the
  // unsupported defaults.
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
  // docs/global-shortcuts.md, docs/launch-at-login.md, docs/devtools.md. The
  // desktop backends implement them over backend-common; iOS keeps the
  // unsupported defaults.
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

  backend_api_.create_tray_icon = Backend_CreateTrayIcon;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon;
  backend_api_.set_tray_icon = Backend_SetTrayIcon;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip;
  backend_api_.set_tray_menu = Backend_SetTrayMenu;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark;
  backend_api_.get_tray_icon_bounds = Backend_GetTrayIconBounds;

  backend_api_.show_notification = Backend_ShowNotification;
  backend_api_.close_notification = Backend_CloseNotification;

  backend_api_.query_permission = Backend_QueryPermission;
  backend_api_.request_permission = Backend_RequestPermission;

  // Menus and notifications (API >= 41): see docs/menus.md and
  // docs/notifications.md. iOS keeps the unsupported defaults.
  backend_api_.menu_capabilities = Backend_MenuCapabilities;
  backend_api_.show_context_menu_ex = Backend_ShowContextMenuEx;
  backend_api_.test_dismiss_context_menu = Backend_TestDismissContextMenu;
  backend_api_.test_trigger_menu_accelerator =
      Backend_TestTriggerMenuAccelerator;
  backend_api_.notification_capabilities = Backend_NotificationCapabilities;
  backend_api_.set_notification_response_handler =
      Backend_SetNotificationResponseHandler;
  backend_api_.list_scheduled_notifications =
      Backend_ListScheduledNotifications;
  backend_api_.cancel_notification = Backend_CancelNotification;
  backend_api_.test_notification_respond = Backend_TestNotificationRespond;

  // UI-thread tasks and auth sessions (API >= 42): see docs/c-abi.md and
  // docs/auth-session.md. The dispatcher is bound to the UI thread in Load.
  backend_api_.dispatch_ui_task = Backend_DispatchUiTask;
  backend_api_.is_ui_thread = Backend_IsUiThread;
  backend_api_.auth_session_capabilities = Backend_AuthSessionCapabilities;
  backend_api_.auth_session_start = Backend_AuthSessionStart;
  backend_api_.test_cancel_auth_session = Backend_TestCancelAuthSession;
  backend_api_.auth_session_cancel = Backend_AuthSessionCancel;
}

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
  // Load runs on the UI thread (each host's main thread, which runs the
  // backend's loop): bind dispatch_ui_task's queue to it before the runtime
  // can dispatch anything.
  laufey_common::UiTaskDispatcher::Get().Bind(
      [this](void (*task)(void*), void* task_data) {
        LaufeyBackend* backend = GetBackend();
        if (!backend)
          return false;  // a headless worker has no UI thread
        backend->PostUiTask(task, task_data);
        return true;
      });
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
    std::cerr << "Failed to load runtime: " << GetLastError() << std::endl;
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
}

void RuntimeLoader::Shutdown() {
  if (shutdown_fn_) {
    shutdown_fn_();
  }

  if (runtime_thread_.joinable()) {
    runtime_thread_.join();
  }
}

void RuntimeLoader::SetSchemeRequestHandler(const std::string& scheme,
                                            laufey_scheme_request_fn handler,
                                            laufey_scheme_cancel_fn on_cancel,
                                            void* user_data) {
  {
    std::lock_guard<std::mutex> lock(scheme_mutex_);
    scheme_request_handler_ = handler;
    scheme_cancel_handler_ = on_cancel;
    scheme_user_data_ = user_data;
  }
  if (handler && backend_) {
    backend_->RegisterSchemeHandler(scheme);
  }
}

void RuntimeLoader::DispatchSchemeRequest(uint32_t window_id,
                                          SchemeExchangeBase* exchange,
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
    // No handler registered: finish so the request doesn't hang.
    exchange->Finish();
  }
}

void RuntimeLoader::OnJsCall(uint32_t window_id, uint64_t call_id,
                             const std::string& method_path,
                             laufey::ValuePtr args) {
  StoreCallWindow(call_id, window_id);
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_js_calls_.push({window_id, call_id, method_path, args});
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
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = js_call_handler_;
    user_data = js_call_user_data_;
  }

  for (auto& call : calls) {
    // Reserved bridge call from the injected external-link interceptor
    // (laufey_external_links.h): open the URL in the OS browser instead of
    // forwarding to the runtime, then resolve the page-side promise.
    if (call.method_path == LAUFEY_OPEN_EXTERNAL_METHOD) {
      std::string url;
      if (call.args && call.args->IsList()) {
        const laufey::ValueList& list = call.args->GetList();
        if (!list.empty() && list[0] && list[0]->IsString()) {
          url = list[0]->GetString();
        }
      }
      if (IsAllowedExternalLinkUrl(url)) {
        if (LaufeyBackend* backend = GetBackend()) {
          backend->OpenExternalURL(url);
        }
      }
      JsCallRespond(call.window_id, call.call_id, laufey::Value::Null(),
                    nullptr);
      continue;
    }

    if (handler) {
      laufey_value_t* argsWrapper = new laufey_value(call.args);
      handler(user_data, call.window_id, call.call_id, call.method_path.c_str(),
              argsWrapper);
    } else {
      JsCallRespond(call.window_id, call.call_id, nullptr,
                    laufey::Value::String("No JS call handler registered"));
    }
  }
}

void RuntimeLoader::JsCallRespond(uint32_t window_id, uint64_t call_id,
                                  laufey::ValuePtr result,
                                  laufey::ValuePtr error) {
  LaufeyBackend* backend = GetBackend();
  if (backend) {
    backend->RespondToJsCall(window_id, call_id, result, error);
  }
}
