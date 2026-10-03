// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Public header for backend-common. Each backend (CEF, webview) pre-parses
// its backend-specific laufey_value_t into the plain structs declared here,
// then calls into the shared per-platform implementations.

#ifndef LAUFEY_BACKEND_COMMON_H_
#define LAUFEY_BACKEND_COMMON_H_

#include <laufey.h>

#include "laufey_menu.h"

#include <cstdint>
#include <string>
#include <vector>

// Forward-declare AppKit types for Obj-C++ callers. Must live outside
// any namespace because `@class` is only valid at global scope.
#if defined(__APPLE__) && defined(__OBJC__)
@class NSMenu;
#endif

namespace laufey_common {

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Win32 string conversion
// ---------------------------------------------------------------------------
//
// Strings cross the C API and flow through both backends as UTF-8, while the
// wide (*W) Win32 APIs want UTF-16; the ANSI (*A) APIs would garble non-ASCII
// text in the active codepage. Every conversion goes through this one pair of
// helpers so a future fix (e.g. long-path prefixing) lands in one place.

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& w);
#endif

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

struct NotificationAction {
  std::string id;
  std::string title;
};

struct NotificationOptions {
  std::string title;
  std::string body;
  // If non-empty, replaces any existing notification with the same tag —
  // matches Web Notifications "tag" semantics.
  std::string tag;
  bool silent = false;
  bool require_interaction = false;
  std::vector<NotificationAction> actions;
  // PNG bytes: the toast's app logo on Windows, the image-data hint on
  // Linux. Not shown on macOS (the app icon is the bundle's).
  std::vector<uint8_t> icon_png;
  // (API >= 41) Unix time in milliseconds to deliver at; 0 = now.
  int64_t schedule_at_ms = 0;
  // (API >= 41) Opaque, handed back in notification responses.
  bool has_data = false;
  std::string data;
};

// Parses a laufey_value_t dict into a plain NotificationOptions. Takes
// ownership of `options` (calls `api->value_free` before returning).
// Returns a default-initialized NotificationOptions if `options` is null
// or not a dict.
NotificationOptions ParseNotificationOptions(laufey_value_t* options,
                                             const laufey_backend_api_t* api);

// The notification entry points live in laufey_notifications.h.

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Tray / status-bar icon (Windows, Shell_NotifyIcon)
// ---------------------------------------------------------------------------
//
// Hidden message-only window + Shell_NotifyIcon + WIC PNG decode.
// Light/dark icons are resolved against the AppsUseLightTheme registry
// value and re-applied automatically on WM_SETTINGCHANGE
// (ImmersiveColorSet) notifications.
//
// All functions are synchronous — callers that need to dispatch to a
// UI thread (e.g. CEF's TID_UI) should marshal before calling.
//
// CreateTrayIconWin returns the new id immediately (allocated atomically).
// FinalizeTrayIconWin must then be called on the thread that pumps the
// tray's message loop (CEF: TID_UI; webview: WebView2 UI thread) to do
// the Shell_NotifyIcon setup. Splitting these lets the trampoline
// return the id synchronously even when it has to marshal the actual
// Shell_NotifyIcon work onto the UI thread.

uint32_t CreateTrayIconWin();
void FinalizeTrayIconWin(uint32_t tray_id);
void DestroyTrayIconWin(uint32_t tray_id);
void SetTrayIconWin(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayIconDarkWin(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayTooltipWin(uint32_t tray_id, const char* tooltip_or_null);
void SetTrayMenuWin(uint32_t tray_id, laufey_value_t* menu_template,
                    const laufey_backend_api_t* api,
                    laufey_menu_click_fn on_click, void* on_click_data);
void SetTrayClickHandlerWin(uint32_t tray_id, laufey_tray_click_fn handler,
                            void* user_data);
void SetTrayDoubleClickHandlerWin(uint32_t tray_id,
                                  laufey_tray_click_fn handler,
                                  void* user_data);
// Writes the tray icon's screen rectangle (top-left origin, DIP) into the
// out-params (any may be NULL) and returns true, or false if the id is
// unknown or the shell can't report the position.
bool GetTrayIconBoundsWin(uint32_t tray_id, int* x, int* y, int* width,
                          int* height);
#endif

// ---------------------------------------------------------------------------
// Title-prefix badge (Windows/Linux dock fallback)
// ---------------------------------------------------------------------------
//
// macOS has NSDockTile.setBadgeLabel; Windows and Linux don't. The
// Slack/Discord/Telegram convention is to prepend "(N) " to each
// window's title. This helper centralizes the saved-titles bookkeeping
// so each backend just iterates its windows, reads the current title
// via its native API, calls this to compute what to set, then writes
// the result back via its native API.
//
// `window_key` is any backend-chosen unique-per-window value (HWND
// cast, GtkWindow* cast, internal id — anything). `badge` is the new
// badge text; empty means clear. Returns the title to apply to the
// window.
//
//   - badge non-empty, first time we see this window: save
//     `current_title`, return "(badge) current_title".
//   - badge non-empty, window already has a saved title: return
//     "(badge) saved_title" (replaces any prior badge prefix).
//   - badge empty, window has a saved title: forget it, return the
//     saved title.
//   - badge empty, no saved title: return `current_title` unchanged.
std::string ApplyTitlePrefixBadge(uint64_t window_key,
                                  const std::string& current_title,
                                  const std::string& badge);

// Forget the saved title for a window (call when a window closes so
// the map doesn't grow unbounded).
void ForgetTitlePrefixBadge(uint64_t window_key);

// ---------------------------------------------------------------------------
// Dialogs (alert / confirm / prompt)
// ---------------------------------------------------------------------------
//
// `dialog_type` is one of LAUFEY_DIALOG_* from laufey.h. All implementations
// block until the user dismisses the dialog. The native modal pumps OS
// events so other laufey windows keep responding.
//
// Returns 1 if OK / confirmed, 0 otherwise. For LAUFEY_DIALOG_PROMPT, on a
// confirmed result `*out_input_value` is set to a strdup'd UTF-8 string
// the caller must free with `free()`.

#ifdef __APPLE__
int ShowDialogMac(int dialog_type, const std::string& title,
                  const std::string& message, const std::string& default_value,
                  char** out_input_value);
#endif

#ifdef _WIN32
// `owner` is the HWND the dialog is modal to, or null for an unowned dialog.
// Every string is shown as text (never passed to a shell or a script).
int ShowDialogWin(int dialog_type, const std::string& title,
                  const std::string& message, const std::string& default_value,
                  char** out_input_value, void* owner = nullptr);
#endif

#ifdef __linux__
int ShowDialogLinux(int dialog_type, const std::string& title,
                    const std::string& message,
                    const std::string& default_value, char** out_input_value);
#endif

// ---------------------------------------------------------------------------
// Clipboard (system)
// ---------------------------------------------------------------------------
//
// Plain-text access to the system clipboard, backing the `read_clipboard_text`
// / `write_clipboard_text` entries of laufey_backend_api. `ClipboardReadText*`
// returns a `strdup`'d / `malloc`'d UTF-8 string the caller frees with
// `free()`, or NULL when the clipboard is empty or holds no text. Must be
// called on the UI thread.

#ifdef __APPLE__
char* ClipboardReadTextMac();
void ClipboardWriteTextMac(const std::string& text);
#endif

#ifdef _WIN32
char* ClipboardReadTextWin();
void ClipboardWriteTextWin(const std::string& text);
#endif

#ifdef __linux__
char* ClipboardReadTextLinux();
void ClipboardWriteTextLinux(const std::string& text);
#endif

// ---------------------------------------------------------------------------
// Permissions / runtime authorization
// ---------------------------------------------------------------------------
//
// `kind` is one of LAUFEY_PERMISSION_* from laufey.h. Results are reported via
// the callback (status one of LAUFEY_PERMISSION_STATUS_*).

// Notification permissions: QueryNotificationPermission /
// RequestNotificationPermission in laufey_notifications.h.

// ---------------------------------------------------------------------------
// Keyboard event key/code mapping (W3C UI Events)
// ---------------------------------------------------------------------------
//
// `key` is the logical value (e.g. "a", "Enter", " "). `code` is the
// physical position (e.g. "KeyA", "Enter", "Space").
//
// CEF normalizes keyboard events to Windows VK codes on every platform,
// so the VK-based mappings below cover CEF Mac / Win / Linux as well as
// the webview Windows backend. The webview macOS and Linux backends use
// platform-native event types and have their own helpers.

// Windows VK → "key" (logical).
//   `character`: the Unicode codepoint typed (0 if not a char event /
//                unknown). When set to a printable ASCII byte, returned
//                directly (matches CEF behavior).
//   `shift_held` / `caps_on`: Windows shift/caps state for case
//                determination when `character` is 0 (matches webview
//                Windows behavior). Pass false on non-Windows callers.
std::string VkToKey(int vk, uint32_t character, bool shift_held, bool caps_on);

// Windows VK → "code" (physical).
//   `is_extended`: WM_KEYDOWN lParam bit 24, distinguishes NumpadEnter
//                  vs Enter, ControlRight/Left, AltRight/Left.
//   `scancode`: WM_KEYDOWN lParam bits 16-23. Used on Windows only via
//               MapVirtualKey to distinguish ShiftLeft vs ShiftRight;
//               0 means default to ShiftLeft (CEF path).
std::string VkToCode(int vk, bool is_extended, uint32_t scancode);

#ifdef __APPLE__
// Cocoa NSEvent key code → W3C "key". `event` is an `NSEvent*` from
// keyboard NSEvents; declared as `void*` so non-Obj-C++ callers can also
// pass through without an Obj-C runtime dependency.
std::string NSEventKeyToKey(void* event_nsevent);
std::string NSEventKeyToCode(unsigned short key_code);

// ---------------------------------------------------------------------------
// Dock / taskbar (macOS app-scoped operations)
// ---------------------------------------------------------------------------
//
// macOS-only dock primitives. The Windows/Linux fallbacks (title-prefix
// badge, FlashWindowEx, GTK urgency hint) need per-window iteration and
// stay in each backend.

// Sets the application dock badge. nullptr or "" clears it.
void SetDockBadgeMac(const char* badge_or_null);

// `type` is one of LAUFEY_DOCK_BOUNCE_INFORMATIONAL /
// LAUFEY_DOCK_BOUNCE_CRITICAL.
void BounceDockMac(int type);

// true → NSApplicationActivationPolicyRegular (dock + menu bar)
// false → NSApplicationActivationPolicyAccessory (background, no dock)
void SetDockVisibleMac(bool visible);

// Stores the dock menu set by Backend_SetDockMenu_Mac /
// WKWebViewBackend::SetDockMenu. Both backends' AppDelegate read this
// value in applicationDockMenu:. Pass nil to clear.
#ifdef __OBJC__
void SetDockMenuMac(NSMenu* menu);
NSMenu* GetDockMenuMac();
#endif

// Stores the dock-reopen handler set by Backend_SetDockReopenHandler_Mac /
// WKWebViewBackend::SetDockReopenHandler. AppDelegate calls
// FireDockReopenMac() from applicationShouldHandleReopen:hasVisibleWindows:.
void SetDockReopenHandlerMac(laufey_dock_reopen_fn handler, void* user_data);
void FireDockReopenMac(bool has_visible_windows);

// ---------------------------------------------------------------------------
// Deep links / custom URL schemes (macOS)
// ---------------------------------------------------------------------------
//
// Stores the open-url handler set by Backend_SetOpenUrlHandler_Mac /
// WKWebViewBackend::SetOpenUrlHandler. Each backend's AppDelegate calls
// FireOpenUrlMac() once per URL from application:openURLs:.
//
// A launch URL always arrives before the runtime (loaded on a worker thread)
// can register anything, so FireOpenUrlMac buffers up to
// LAUFEY_MAX_PENDING_OPEN_URLS URLs while no handler is set and
// SetOpenUrlHandlerMac flushes them, in order, on registration. Passing a
// null handler clears it and re-arms buffering.
void SetOpenUrlHandlerMac(laufey_open_url_fn handler, void* user_data);
void FireOpenUrlMac(const char* url);

// Test-only. Backs the C ABI `test_trigger_open_url` hook: routes `url`
// through FireOpenUrlMac and reports whether a handler consumed it (true) or
// it was buffered for a later registration (false).
bool TestTriggerOpenUrlMac(const char* url);

// ---------------------------------------------------------------------------
// NSMenu builder (macOS)
// ---------------------------------------------------------------------------
//
// Walks a laufey_value_t menu template and produces an NSMenu. Click events
// on non-role items invoke on_click(on_click_data, window_id, item_id).
// Role items (copy/paste/cut/quit/minimize/...) wire to First
// Responder selectors and don't reach on_click.
//
// Returns the menu through an opaque void* on non-Obj-C callers and as
// NSMenu* on Obj-C++ callers — header double-declared so the typed form
// flows through .mm files without forcing AppKit on plain .cc.
// (NSMenu is forward-declared at the top of this header.)
#ifdef __OBJC__
NSMenu* BuildNSMenuFromValue(laufey_value_t* val,
                             const laufey_backend_api_t* api,
                             laufey_menu_click_fn on_click, void* on_click_data,
                             uint32_t window_id);
#else
void* BuildNSMenuFromValue(laufey_value_t* val, const laufey_backend_api_t* api,
                           laufey_menu_click_fn on_click, void* on_click_data,
                           uint32_t window_id);
#endif

#ifdef __OBJC__
// The same from parsed entries (laufey_menu.h): items with an accelerator
// get it as their key equivalent.
NSMenu* BuildNSMenuFromEntries(const std::vector<MenuEntry>& entries,
                               laufey_menu_click_fn on_click,
                               void* on_click_data, uint32_t window_id);
// Records window `window_id`'s app menu (nil forgets it) for
// TestTriggerMenuAcceleratorMac. Any thread.
void RegisterWindowMenuMac(uint32_t window_id, NSMenu* menu);
#endif

// Backs test_trigger_menu_accelerator: a key-down NSEvent for the
// accelerator through -[NSMenu performKeyEquivalent:] on the window's app
// menu, the matching AppKit does for a key press. Any thread.
bool TestTriggerMenuAcceleratorMac(uint32_t window_id, const char* accelerator);
// Called by the menu builder's target for each item click (main thread).
void NoteMenuItemClickedMac();

// Pops a context menu up at (x, y) in `nsview` (an NSView*, top-left origin
// as for show_context_menu) and blocks in its tracking loop until it closes;
// `on_closed` fires (from the main queue) after it closed. Main thread.
void ShowContextMenuMac(void* nsview, int x, int y,
                        const std::vector<MenuEntry>& entries,
                        laufey_menu_click_fn on_click, void* on_click_data,
                        laufey_menu_closed_fn on_closed, void* on_closed_data,
                        uint32_t window_id);

// Runs `fn` on the main thread from the main run loop, in the common modes,
// and not as a block of the main dispatch queue. For work that runs a
// tracking loop of its own (ShowContextMenuMac): the main queue is serial,
// so while one of its blocks is running, none of the others does, and every
// dispatch_sync / dispatch_async to the main queue (a backend's UI-thread
// calls, dispatch_ui_task) would wait for the menu to close. Any thread.
void RunFromMainRunLoopMac(std::function<void()> fn);

// ---------------------------------------------------------------------------
// Tray / status-bar icon (macOS, NSStatusItem)
// ---------------------------------------------------------------------------

uint32_t CreateTrayIconMac();
void DestroyTrayIconMac(uint32_t tray_id);
void SetTrayIconMac(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayIconDarkMac(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayTooltipMac(uint32_t tray_id, const char* tooltip_or_null);
void SetTrayMenuMac(uint32_t tray_id, laufey_value_t* menu_template,
                    const laufey_backend_api_t* api,
                    laufey_menu_click_fn on_click, void* on_click_data);
void SetTrayClickHandlerMac(uint32_t tray_id, laufey_tray_click_fn handler,
                            void* user_data);
void SetTrayDoubleClickHandlerMac(uint32_t tray_id,
                                  laufey_tray_click_fn handler,
                                  void* user_data);
// Writes the tray icon's screen rectangle (top-left origin, points/DIP) into
// the out-params (any may be NULL) and returns true. Returns false if the id
// is unknown or the icon has no on-screen button yet.
bool GetTrayIconBoundsMac(uint32_t tray_id, int* x, int* y, int* width,
                          int* height);
#endif

#ifdef __linux__
// GDK keyval → W3C "key". `keyval` is a GDK keyval (gdk_event_key.keyval).
// `evdev_keycode` is GDK's evdev hardware keycode
// (gdk_event_key.hardware_keycode), used for "code" mapping.
std::string GdkKeyvalToKey(unsigned int keyval);
std::string GdkKeycodeToCode(unsigned int evdev_keycode);

// Build a GtkMenu (or GtkMenuBar when `is_menu_bar` is true) from a
// laufey_value_t menu template. Non-role items trigger
// on_click(on_click_data, window_id, item_id). Role items map to
// labels and forward the role as the item id. Returns NULL if `val`
// is null or not a list.
//
// Returned as void* on non-GTK callers and as GtkWidget* on GTK-aware
// translation units (gtk.h must be included before this header for the
// typed form).
#ifdef __GTK_H__
GtkWidget* BuildGtkMenuFromValue(laufey_value_t* val,
                                 const laufey_backend_api_t* api,
                                 uint32_t window_id,
                                 laufey_menu_click_fn on_click,
                                 void* on_click_data, bool is_menu_bar);
#else
void* BuildGtkMenuFromValue(laufey_value_t* val,
                            const laufey_backend_api_t* api, uint32_t window_id,
                            laufey_menu_click_fn on_click, void* on_click_data,
                            bool is_menu_bar);
#endif

#ifdef __GTK_H__
// The same from parsed entries. With an `accel_group` (the window's) the
// enabled items' accelerators are bound in it; without one they are only
// shown.
GtkWidget* BuildGtkMenuFromEntries(const std::vector<MenuEntry>& entries,
                                   uint32_t window_id,
                                   laufey_menu_click_fn on_click,
                                   void* on_click_data, bool is_menu_bar,
                                   GtkAccelGroup* accel_group);
// Pops a context menu up at (x, y) in `relative_to`'s coordinates (the web
// view). Non-blocking; `on_closed` fires from an idle after the menu closed
// (after the chosen item's click), or right away when it couldn't be shown.
// GTK thread.
void ShowGtkContextMenu(GtkWidget* relative_to, int x, int y,
                        const std::vector<MenuEntry>& entries,
                        uint32_t window_id, laufey_menu_click_fn on_click,
                        void* on_click_data, laufey_menu_closed_fn on_closed,
                        void* on_closed_data);
// Backs test_trigger_menu_accelerator: gtk_accel_groups_activate on the
// window, what GtkWindow's key-press handler runs first. GTK thread.
bool TestTriggerMenuAcceleratorGtk(GtkWidget* window, const char* accelerator);
#endif

// ---------------------------------------------------------------------------
// Tray / status-bar icon (Linux, appindicator)
// ---------------------------------------------------------------------------
//
// All functions must be called on the GTK main thread; backends with
// off-main-thread callers should marshal first (CEF uses CefPostTask).
// The Ayatana or legacy appindicator library is dlopen()ed at runtime;
// when neither is present on the system, CreateTrayIconLinux returns 0
// and all other calls no-op.
//
// AppIndicator has no left-click event — a click anywhere pops the
// indicator's menu — so SetTrayClickHandlerLinux and
// SetTrayDoubleClickHandlerLinux accept handlers for API symmetry but
// don't surface clicks back.

uint32_t CreateTrayIconLinux();
void DestroyTrayIconLinux(uint32_t tray_id);
void SetTrayIconLinux(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayIconDarkLinux(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayTooltipLinux(uint32_t tray_id, const char* tooltip_or_null);
void SetTrayMenuLinux(uint32_t tray_id, laufey_value_t* menu_template,
                      const laufey_backend_api_t* api,
                      laufey_menu_click_fn on_click, void* on_click_data);
void SetTrayClickHandlerLinux(uint32_t tray_id, laufey_tray_click_fn handler,
                              void* user_data);
void SetTrayDoubleClickHandlerLinux(uint32_t tray_id,
                                    laufey_tray_click_fn handler,
                                    void* user_data);
#endif

// ---------------------------------------------------------------------------
// App data directory (per-app persistent web storage)
// ---------------------------------------------------------------------------
//
// One directory per process holds the web engine's profile (localStorage,
// IndexedDB, cookies, caches), so storage is per app and survives relaunches.
// Resolved at process start, because CEF and WebView2 need it before the
// runtime library is loaded. Each of LAUFEY_DATA_DIR / LAUFEY_APP_ID below is
// the environment variable if set, else the launch file's "dataDir" / "appId"
// (laufey_launch_config.h):
//
//   1. LAUFEY_DATA_DIR, if set and absolute (a relative value is ignored with
//      a warning);
//   2. else <per-user app data>/<LAUFEY_APP_ID>: ~/Library/Application Support
//      (macOS), $XDG_DATA_HOME or ~/.local/share (Linux), %LOCALAPPDATA%
//      (Windows). An id outside [A-Za-z0-9._-], ".", or ".." is rejected with
//      a warning;
//   3. else none: each backend keeps its previous default storage.
//
// Each backend stores its profile in its own subdirectory (CEF/, WebView2/,
// WebKitGTK/) so two engines never share one profile. See docs/app-data.md.

// The value of environment variable `name` as UTF-8, or "" when it is unset
// (or empty). On Windows it is read wide (GetEnvironmentVariableW) and
// converted, so non-ASCII values survive whatever the active codepage is.
// Shared by the app data directory and launch file readers.
std::string GetEnvUtf8(const char* name);

// The resolved directory (UTF-8, absolute, no trailing separator), or "" when
// none is configured. Resolved once per process; warnings go to stderr on the
// first call. Does not create the directory.
const std::string& AppDataDir();

// `AppDataDir()/name`, created (with parents, owner-only on Unix) if missing.
// Returns "" when no directory is configured or it can't be created (with a
// warning), in which case the caller keeps its default storage.
std::string AppDataSubdir(const char* name);

// Pure resolution step behind AppDataDir(), exposed for tests. Reads nothing
// from the environment or filesystem: `platform_base` is the per-user app-data
// directory ("" if unknown). Appends a message to `warnings` (if non-null) for
// every rejected input.
std::string ResolveAppDataDirFrom(const std::string& data_dir_env,
                                  const std::string& app_id_env,
                                  const std::string& platform_base,
                                  std::vector<std::string>* warnings);

// True if `id` is usable as a single path component: non-empty, not "." or
// "..", and only [A-Za-z0-9._-].
bool IsSafeAppId(const std::string& id);

// True for an absolute path on this platform ("/x"; "C:\x", "C:/x" or
// "\\server\share" on Windows). Rejects embedded NULs.
bool IsAbsolutePath(const std::string& path);

// `base` + platform separator + `child`.
std::string JoinPath(const std::string& base, const std::string& child);

// mkdir -p. New components are created 0700 on Unix. Returns true if `path`
// is (now) a directory.
bool EnsureDirectory(const std::string& path);

// ---------------------------------------------------------------------------
// Test hooks (API >= 30)
// ---------------------------------------------------------------------------

// Records the on_click handler for a menu/tray item id when a menu is built, so
// TestClickMenuItem can synthesize a click. Called by the menu builders; no-op
// for empty id or null fn. Later registrations for the same id win (matching
// the "last menu wins" behavior of a re-set menu).
void RegisterMenuClick(const std::string& id, laufey_menu_click_fn fn,
                       void* data, uint32_t window_id);

// Test-only. Synthesizes a click on the menu/tray item with id `item_id` by
// invoking its registered on_click handler with the right window id. Returns
// true if an item with that id was registered. Backs the C ABI
// `test_click_menu_item` hook used by automated e2e tests.
bool TestClickMenuItem(const char* item_id);

// Dispatch table so CEF and WebView share the inject implementation
// without depending on each other's RuntimeLoader type.
struct TestInjectSink {
  void (*key)(void* ctx, uint32_t window_id, int state, const char* key,
              const char* code, uint32_t modifiers, bool repeat);
  void (*click)(void* ctx, uint32_t window_id, int state, int button, double x,
                double y, uint32_t modifiers, int32_t click_count);
  void (*move)(void* ctx, uint32_t window_id, double x, double y,
               uint32_t modifiers);
  void (*wheel)(void* ctx, uint32_t window_id, double delta_x, double delta_y,
                double x, double y, uint32_t modifiers, int32_t delta_mode);
  void (*enter_leave)(void* ctx, uint32_t window_id, int entered, double x,
                      double y, uint32_t modifiers);
  void* ctx;
};

// Test-only. Posts `event` through `sink`. Thin path: already-DOM values,
// click_count 1. Returns false on a null event or unknown kind.
bool TestInjectInput(uint32_t window_id, const laufey_test_input_t* event,
                     const TestInjectSink& sink);

}  // namespace laufey_common

#endif  // LAUFEY_BACKEND_COMMON_H_
