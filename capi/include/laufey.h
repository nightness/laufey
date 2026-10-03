// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_H
#define LAUFEY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The version of this header. A backend sets `version` in its API table to
// the value it was built with, and the runtime side (the `laufey` crate's
// init_api) accepts only an exact match: runtime and backend must come from
// the same laufey release. "NULL on backends older than API version N" below
// records when an entry point appeared; a runtime that enforces the exact
// match never meets such a backend, but entry points a backend does not
// implement are still NULL and must be null-checked.
#define LAUFEY_API_VERSION 43

// Window handle types for get_window_handle_type
#define LAUFEY_WINDOW_HANDLE_UNKNOWN 0
#define LAUFEY_WINDOW_HANDLE_APPKIT 1
#define LAUFEY_WINDOW_HANDLE_WIN32 2
#define LAUFEY_WINDOW_HANDLE_X11 3
#define LAUFEY_WINDOW_HANDLE_WAYLAND 4

// Window creation flags for create_window_ex (bitmask).
//
// FRAMELESS removes the title bar and standard window chrome (border,
// traffic-light / caption buttons). NO_ACTIVATE creates a utility "panel"
// window that floats above normal windows and does not activate the app or
// steal key focus from the foreground app when shown — the combination used
// for tray / menu-bar popovers. On macOS NO_ACTIVATE maps to a
// non-activating NSPanel; on Windows to WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
// on Linux to a utility window type hint.
//
// TRANSPARENT_TITLEBAR keeps the standard window frame and traffic-light /
// caption buttons but makes the title bar transparent and lets the web
// content extend underneath it (Electron `titleBarStyle: 'hidden'`). The page
// can then draw its own toolbar in that strip, with the system buttons
// overlaid. The app is responsible for insetting its UI to clear the
// traffic-light buttons. macOS only for now; ignored by other backends.
//
// HIDDEN creates the OS window without mapping/showing it. The window stays
// invisible until the embedder calls `show` (or `focus`). This lets an app
// defer the first reveal until content has painted (see set_page_load_handler)
// so the user never sees the empty/black initial webview frame — particularly
// important on Wayland, where the compositor only ever presents committed
// buffers and the pre-load frame shows as solid black. API >= 27.
//
// TRANSPARENT creates the window with a transparent background so the alpha
// channel of the web content composites against whatever is behind the window
// (Electron `transparent: true`, Tauri `transparent: true`). The OS window is
// made non-opaque and the web engine is told not to paint an opaque backdrop,
// so any page region the document leaves transparent (e.g. `background:
// transparent` on the root) shows the desktop / windows underneath. Pair it
// with FRAMELESS for the common borderless-translucent look. This is distinct
// from `set_window_opacity`, which fades the whole window (chrome included) by
// a uniform factor; TRANSPARENT instead honors the page's own per-pixel alpha.
// Must be decided at creation time. Supported by the system-WebView backend on
// macOS and Linux (WebKitGTK) and by the Winit backend. The Windows WebView2
// and CEF backends paint an opaque window background in windowed mode and
// ignore this flag. API >= 28.
#define LAUFEY_WINDOW_FLAG_FRAMELESS (1u << 0)
#define LAUFEY_WINDOW_FLAG_NO_ACTIVATE (1u << 1)
#define LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR (1u << 2)
#define LAUFEY_WINDOW_FLAG_HIDDEN (1u << 3)
#define LAUFEY_WINDOW_FLAG_TRANSPARENT (1u << 4)

// --- Window state, screens and chrome (API >= 38) ---------------------------
//
// Window state bits returned by get_window_state and passed to the
// window-state handler. A window can be in more than one state at once where
// the OS allows it (e.g. MAXIMIZED and MINIMIZED for a minimized window that
// will restore to maximized, on platforms that report both).
#define LAUFEY_WINDOW_STATE_MAXIMIZED (1u << 0)
#define LAUFEY_WINDOW_STATE_MINIMIZED (1u << 1)
#define LAUFEY_WINDOW_STATE_FULLSCREEN (1u << 2)

// Actions for set_window_state.
#define LAUFEY_WINDOW_ACTION_MAXIMIZE 1
#define LAUFEY_WINDOW_ACTION_UNMAXIMIZE 2
#define LAUFEY_WINDOW_ACTION_MINIMIZE 3
// Un-minimize: the window returns to the state it had before it was
// minimized (maximized windows come back maximized).
#define LAUFEY_WINDOW_ACTION_RESTORE 4
#define LAUFEY_WINDOW_ACTION_ENTER_FULLSCREEN 5
#define LAUFEY_WINDOW_ACTION_LEAVE_FULLSCREEN 6

// Title bar styles for set_window_titlebar_style.
//   DEFAULT       the platform's standard title bar.
//   HIDDEN        a transparent title bar with the content extended under it
//                 and the system buttons overlaid (the runtime form of
//                 LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR; Electron
//                 `titleBarStyle: 'hidden'`).
//   HIDDEN_INSET  HIDDEN with the macOS traffic lights inset from the window
//                 edge (Electron `titleBarStyle: 'hiddenInset'`).
#define LAUFEY_TITLEBAR_DEFAULT 0
#define LAUFEY_TITLEBAR_HIDDEN 1
#define LAUFEY_TITLEBAR_HIDDEN_INSET 2

// Backdrops for set_window_backdrop.
//   NONE       remove any backdrop (the window is opaque again).
//   MICA       Windows 11 Mica (DWMSBT_MAINWINDOW).
//   ACRYLIC    Windows 11 Acrylic (DWMSBT_TRANSIENTWINDOW).
//   MICA_ALT   Windows 11 tabbed Mica (DWMSBT_TABBEDWINDOW).
//   VIBRANCY   macOS NSVisualEffectView behind the web view, with the
//              `material` argument (LAUFEY_VIBRANCY_*).
// A backdrop only shows where the page leaves its background transparent:
// the backend makes the web view's own background transparent while one is
// set and restores it for NONE.
#define LAUFEY_BACKDROP_NONE 0
#define LAUFEY_BACKDROP_MICA 1
#define LAUFEY_BACKDROP_ACRYLIC 2
#define LAUFEY_BACKDROP_MICA_ALT 3
#define LAUFEY_BACKDROP_VIBRANCY 4

// macOS vibrancy materials (the NSVisualEffectMaterial values, so they pass
// through unchanged).
#define LAUFEY_VIBRANCY_TITLEBAR 3
#define LAUFEY_VIBRANCY_SELECTION 4
#define LAUFEY_VIBRANCY_MENU 5
#define LAUFEY_VIBRANCY_POPOVER 6
#define LAUFEY_VIBRANCY_SIDEBAR 7
#define LAUFEY_VIBRANCY_HEADER_VIEW 10
#define LAUFEY_VIBRANCY_SHEET 11
#define LAUFEY_VIBRANCY_WINDOW_BACKGROUND 12
#define LAUFEY_VIBRANCY_HUD 13
#define LAUFEY_VIBRANCY_FULLSCREEN_UI 15
#define LAUFEY_VIBRANCY_TOOLTIP 17
#define LAUFEY_VIBRANCY_CONTENT_BACKGROUND 18
#define LAUFEY_VIBRANCY_UNDER_WINDOW_BACKGROUND 21
#define LAUFEY_VIBRANCY_UNDER_PAGE_BACKGROUND 22

// Capability bits returned by window_capabilities: what this backend can do
// on this OS (and OS build) right now. A setter for a capability that is not
// reported is a no-op (or returns false); a getter returns its "unknown"
// value. Embedders surface this instead of pretending.
#define LAUFEY_WINDOW_CAP_STATE (1u << 0)             // set/get_window_state
#define LAUFEY_WINDOW_CAP_STATE_EVENTS (1u << 1)      // window-state handler
#define LAUFEY_WINDOW_CAP_SIZE_CONSTRAINTS (1u << 2)  // min / max size
#define LAUFEY_WINDOW_CAP_SCREENS (1u << 3)           // get_screens
#define LAUFEY_WINDOW_CAP_DISPLAY_EVENTS (1u << 4)    // display-changed handler
#define LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN (1u << 5)
#define LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN_INSET (1u << 6)
#define LAUFEY_WINDOW_CAP_TRAFFIC_LIGHT_POSITION (1u << 7)
#define LAUFEY_WINDOW_CAP_BACKDROP_MICA (1u << 8)
#define LAUFEY_WINDOW_CAP_BACKDROP_ACRYLIC (1u << 9)
#define LAUFEY_WINDOW_CAP_BACKDROP_MICA_ALT (1u << 10)
#define LAUFEY_WINDOW_CAP_VIBRANCY (1u << 11)
#define LAUFEY_WINDOW_CAP_NORMAL_BOUNDS (1u << 12)  // get_window_normal_bounds
#define LAUFEY_WINDOW_CAP_KEEP_ALIVE \
  (1u << 13)  // set_quit_on_last_window_closed
#define LAUFEY_WINDOW_CAP_SET_POSITION \
  (1u << 14)  // set_window_position works
              // (not on Wayland)
// API >= 39: drag and drop and native file dialogs.
#define LAUFEY_WINDOW_CAP_FILE_DROP (1u << 15)  // file-drop handler fires
#define LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS \
  (1u << 16)  // ENTER / OVER already carry the paths
              // (otherwise only the count until DROP)
#define LAUFEY_WINDOW_CAP_FILE_DRAG_OUT (1u << 17)  // start_file_drag
#define LAUFEY_WINDOW_CAP_FILE_DIALOGS (1u << 18)   // show_file_dialog
#define LAUFEY_WINDOW_CAP_FILE_DIALOG_FILES_AND_DIRECTORIES \
  (1u << 19)  // one open dialog may pick files and directories (macOS)
#define LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL \
  (1u << 20)  // a dialog with a window_id is modal to it (a sheet on macOS)

// --- Drag and drop (API >= 39) ----------------------------------------------
//
// Phases of a file drag over a window, passed to laufey_file_drop_fn.
#define LAUFEY_DRAG_ENTER 0
#define LAUFEY_DRAG_OVER 1
#define LAUFEY_DRAG_LEAVE 2
#define LAUFEY_DRAG_DROP 3

// Most paths one drop reports; a bigger drop is cut to this many.
#define LAUFEY_MAX_DROP_PATHS 4096

// Callback fired while files are dragged over a window and when they are
// dropped on it (see set_file_drop_handler). `x`, `y` are the pointer in
// window content coordinates, the space of the mouse handlers. `paths` holds
// `count` absolute native paths (UTF-8), valid only for the duration of the
// call:
//   ENTER / OVER  `count` is the number of files dragged; `paths` is NULL
//                 unless the backend reports LAUFEY_WINDOW_CAP_FILE_DROP_
//                 ENTER_PATHS (some engines reveal the paths only on drop).
//   LEAVE         the drag left the window or was cancelled: count 0, NULL.
//   DROP          the files were dropped: `paths` is never NULL.
// Only drags that carry files are reported; text, links and file promises
// that are not files yet (e.g. a photo dragged out of Photos) are not.
typedef void (*laufey_file_drop_fn)(void* user_data, uint32_t window_id,
                                    int phase, double x, double y,
                                    const char* const* paths, size_t count);

// Outcome of start_file_drag, passed to laufey_drag_result_fn.
#define LAUFEY_DRAG_RESULT_DROPPED 0    // a target accepted the files
#define LAUFEY_DRAG_RESULT_CANCELLED 1  // the user cancelled, or no target
#define LAUFEY_DRAG_RESULT_FAILED 2     // not started (see start_file_drag)

typedef void (*laufey_drag_result_fn)(void* user_data, int result);

// --- Native file dialogs (API >= 39) ----------------------------------------
//
// Kinds for laufey_file_dialog_options_t.kind.
#define LAUFEY_FILE_DIALOG_OPEN 0
#define LAUFEY_FILE_DIALOG_SAVE 1

// Flags for laufey_file_dialog_options_t.flags. An OPEN dialog with neither
// CHOOSE_FILES nor CHOOSE_DIRECTORIES picks files. Both together pick either
// only where LAUFEY_WINDOW_CAP_FILE_DIALOG_FILES_AND_DIRECTORIES is reported;
// elsewhere the dialog picks directories.
#define LAUFEY_FILE_DIALOG_CHOOSE_FILES (1u << 0)
#define LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES (1u << 1)
#define LAUFEY_FILE_DIALOG_MULTIPLE (1u << 2)     // OPEN only
#define LAUFEY_FILE_DIALOG_SHOW_HIDDEN (1u << 3)  // show dot / hidden files
#define LAUFEY_FILE_DIALOG_NO_OVERWRITE_CONFIRM \
  (1u << 4)  // SAVE: don't ask before replacing a file (where the OS lets us)

// Result statuses passed to laufey_file_dialog_result_fn.
#define LAUFEY_FILE_DIALOG_ACCEPTED 0  // `paths` holds the selection
#define LAUFEY_FILE_DIALOG_CANCELLED \
  1                                  // the user (or cancel_file_dialog)
                                     // dismissed it
#define LAUFEY_FILE_DIALOG_BUSY 2    // another file dialog is open
#define LAUFEY_FILE_DIALOG_FAILED 3  // the OS could not show it

// Most filters / extensions one dialog accepts; the rest are ignored.
#define LAUFEY_FILE_DIALOG_MAX_FILTERS 64
#define LAUFEY_FILE_DIALOG_MAX_EXTENSIONS 256

// One file-type filter: a label ("Images") and extensions without the dot
// ("png", "jpg"); the extension "*" means every file.
typedef struct laufey_file_filter {
  const char* name;
  const char* const* extensions;
  size_t extension_count;
} laufey_file_filter_t;

// Options for show_file_dialog. Every string is UTF-8 and may be NULL for the
// platform default. `default_path` is a directory to start in, or a file path
// (an OPEN dialog starts in its directory; a SAVE dialog also proposes its
// name); a SAVE dialog given only a name proposes that name. The backend copies
// everything it needs before show_file_dialog returns.
typedef struct laufey_file_dialog_options {
  int kind;        // LAUFEY_FILE_DIALOG_OPEN / _SAVE
  uint32_t flags;  // LAUFEY_FILE_DIALOG_* flags
  const char* title;
  const char* default_path;
  const char* button_label;  // the accept button ("Import")
  const laufey_file_filter_t* filters;
  size_t filter_count;
} laufey_file_dialog_options_t;

// Result of show_file_dialog: `status` is LAUFEY_FILE_DIALOG_*; for ACCEPTED
// `paths` holds `count` (>= 1) absolute native paths, otherwise it is NULL and
// `count` is 0. Valid only for the duration of the call.
typedef void (*laufey_file_dialog_result_fn)(void* user_data,
                                             uint32_t dialog_id, int status,
                                             const char* const* paths,
                                             size_t count);

// Actions for test_file_dialog_respond.
#define LAUFEY_TEST_DIALOG_CANCEL 0
#define LAUFEY_TEST_DIALOG_ACCEPT 1

// --- Clipboard: HTML, images, formats and change events (API >= 39) ---------
//
// Capability bits returned by clipboard_capabilities.
#define LAUFEY_CLIPBOARD_CAP_TEXT (1u << 0)     // read/write_clipboard_text
#define LAUFEY_CLIPBOARD_CAP_HTML (1u << 1)     // read/write_clipboard_html
#define LAUFEY_CLIPBOARD_CAP_IMAGE (1u << 2)    // read/write_clipboard_image
#define LAUFEY_CLIPBOARD_CAP_FORMATS (1u << 3)  // read_clipboard_formats
#define LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS \
  (1u << 4)  // set_clipboard_change_handler fires

// Largest clipboard payload a read returns (text, HTML or PNG bytes). Bigger
// content reads as absent, so a huge copy elsewhere can't exhaust memory here.
#define LAUFEY_CLIPBOARD_MAX_READ_BYTES (64u * 1024u * 1024u)

// Callback fired when the system clipboard's content changes (any app,
// including this one). Fires on the backend UI thread; read the clipboard to
// see the new content.
typedef void (*laufey_clipboard_change_fn)(void* user_data);

// --- Global shortcuts, launch at login, DevTools (API >= 40) ----------------
//
// Capability bits returned by system_capabilities.
#define LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS \
  (1u << 0)  // register_shortcut can bind system-wide shortcuts here
#define LAUFEY_SYSTEM_CAP_SHORTCUTS_USER_BINDS \
  (1u << 1)  // the user approves each shortcut and may pick another trigger
             // (the XDG GlobalShortcuts portal on Wayland)
#define LAUFEY_SYSTEM_CAP_LAUNCH_AT_LOGIN (1u << 2)  // get/set_launch_at_login
#define LAUFEY_SYSTEM_CAP_DEVTOOLS \
  (1u << 3)  // open / close / is_devtools_open work per window

// Statuses passed to laufey_shortcut_result_fn.
#define LAUFEY_SHORTCUT_OK 0
#define LAUFEY_SHORTCUT_INVALID \
  1  // the accelerator does not parse, or names a key that can't be global
     // (a printable key with no modifier other than Shift)
#define LAUFEY_SHORTCUT_CONFLICT \
  2  // the OS refused it: another app (or another part of this process,
     // outside this API) holds that combination
#define LAUFEY_SHORTCUT_ALREADY_REGISTERED \
  3  // this app already registered it through register_shortcut
#define LAUFEY_SHORTCUT_NOT_SUPPORTED \
  4  // no global shortcuts here (Wayland without the GlobalShortcuts
     // portal, a backend without them)
#define LAUFEY_SHORTCUT_DENIED 5  // the user declined it (portal dialog)
#define LAUFEY_SHORTCUT_FAILED 6  // any other OS failure

// Most shortcuts one app holds at a time; more answer FAILED.
#define LAUFEY_MAX_SHORTCUTS 256

// Fired when a registered global shortcut is pressed, whichever app has the
// focus. `accelerator` is the canonical form register_shortcut reported
// (UTF-8, valid for the duration of the call). Fires on the backend UI
// thread.
typedef void (*laufey_shortcut_fn)(void* user_data, const char* accelerator);

// Result of register_shortcut: a LAUFEY_SHORTCUT_* status and, for OK and
// ALREADY_REGISTERED, the canonical accelerator (else NULL). Valid only for
// the duration of the call.
typedef void (*laufey_shortcut_result_fn)(void* user_data, int status,
                                          const char* accelerator);

// Launch-at-login states returned by get_launch_at_login and
// set_launch_at_login.
#define LAUFEY_LOGIN_ITEM_DISABLED 0  // the app does not start at login
#define LAUFEY_LOGIN_ITEM_ENABLED 1   // it does
#define LAUFEY_LOGIN_ITEM_REQUIRES_APPROVAL \
  2  // registered, but the user must allow it in the system settings first
     // (macOS Login Items; a Windows startup entry the user turned off)
#define LAUFEY_LOGIN_ITEM_NOT_SUPPORTED 3  // not on this backend / OS version
#define LAUFEY_LOGIN_ITEM_FAILED 4         // set_launch_at_login failed

// --- Menus: context-menu close, accelerators (API >= 41) --------------------
//
// Capability bits returned by menu_capabilities.
#define LAUFEY_MENU_CAP_APP_MENU \
  (1u << 0)  // set_application_menu shows a menu (bar) on this backend / OS
#define LAUFEY_MENU_CAP_ACCELERATORS \
  (1u << 1)  // app-menu items' accelerators fire their item from the keyboard
#define LAUFEY_MENU_CAP_CONTEXT_MENU (1u << 2)  // show_context_menu(_ex) works
#define LAUFEY_MENU_CAP_CONTEXT_CLOSED \
  (1u << 3)  // show_context_menu_ex reports the menu closing
#define LAUFEY_MENU_CAP_ICONS (1u << 4)     // item "icon" is drawn
#define LAUFEY_MENU_CAP_TOOLTIPS (1u << 5)  // item "tooltip" is shown

// Fired EXACTLY ONCE when a context menu shown with show_context_menu_ex
// closes: after the item's laufey_menu_click_fn when one was chosen, or alone
// when it was dismissed. Fires on the backend UI thread (or, for a request
// that never showed a menu, on the calling thread before
// show_context_menu_ex returns).
typedef void (*laufey_menu_closed_fn)(void* user_data, uint32_t window_id);

// --- Notifications: scheduling, actions, responses (API >= 41) -------------
//
// Capability bits returned by notification_capabilities.
#define LAUFEY_NOTIFICATION_CAP_SHOW \
  (1u << 0)  // show_notification can display a notification here
#define LAUFEY_NOTIFICATION_CAP_SCHEDULE \
  (1u << 1)  // "schedule_at" delivers at that time, at least while running
#define LAUFEY_NOTIFICATION_CAP_SCHEDULE_PERSISTS \
  (1u << 2)  // the OS delivers a scheduled notification even if the app is
             // not running then (macOS, Windows; not Linux, where laufey's
             // own timer re-arms the schedule at the next launch)
#define LAUFEY_NOTIFICATION_CAP_ACTIONS (1u << 3)  // action buttons are shown
#define LAUFEY_NOTIFICATION_CAP_CLICKS \
  (1u << 4)  // clicks (body and actions) are reported
#define LAUFEY_NOTIFICATION_CAP_COLD_START \
  (1u << 5)  // a click while the app is not running launches it and the
             // response reaches set_notification_response_handler

// Most responses held while no response handler is registered (a cold-start
// click is the usual one); beyond that the oldest are dropped.
#define LAUFEY_MAX_PENDING_NOTIFICATION_RESPONSES 16

// Longest "tag" (UTF-8 bytes) and "data" accepted by show_notification; a
// longer one makes it fail (return 0).
#define LAUFEY_NOTIFICATION_MAX_TAG_BYTES 256
#define LAUFEY_NOTIFICATION_MAX_DATA_BYTES 4096

// A click on a notification that no live show_notification callback owns:
// one posted by an earlier run of the app (a scheduled one, or one clicked
// in the notification center later), or the click that launched the app.
// `response_json` (UTF-8, valid for the duration of the call) is an object:
//   "tag"     string   the notification's tag
//   "action"  string   the action button's id, or null for the body
//   "data"    string   the notification's "data", or null
//   "launch"  bool     true when the response arrived before any handler was
//                      registered (the click that launched the app, or one
//                      made while it was starting), and on Windows for the
//                      click COM started the app for, however soon a handler
//                      was registered
// Fires on a backend thread (not necessarily the UI thread).
typedef void (*laufey_notification_response_fn)(void* user_data,
                                                const char* response_json);

// Result of list_scheduled_notifications: `list_json` (UTF-8, valid for the
// duration of the call) is an array of objects, soonest first:
//   "tag", "title", "body" strings; "at" number (Unix time, milliseconds);
//   "data" string or null; "actions" [{"id", "title"}].
typedef void (*laufey_notification_list_fn)(void* user_data,
                                            const char* list_json);

// One display, as reported by get_screens. Every rectangle is in the same
// top-left-origin screen space and units as get_window_position /
// set_window_position on this backend, so a window position can be compared
// with (and clamped to) a screen directly. `id` identifies the display while
// it stays connected (CGDirectDisplayID on macOS; a hash of the device name on
// Windows; CEF's display id); it is not guaranteed to survive a reconnect or
// a restart. `scale_factor` is physical pixels per DIP for that display.
typedef struct laufey_screen {
  int64_t id;
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
  int32_t work_x;  // the work area: the bounds minus the menu bar, dock,
  int32_t work_y;  // taskbar and panels
  int32_t work_width;
  int32_t work_height;
  double scale_factor;
  bool is_primary;
} laufey_screen_t;

// Callback fired when a window's state changes (maximized, minimized,
// fullscreen; LAUFEY_WINDOW_STATE_* bits), with the state before the change.
// Fires once per observed change, on the backend UI thread, after the OS has
// applied it (on macOS a fullscreen transition reports when its animation
// completes). Resize / move / focus keep their own handlers.
typedef void (*laufey_window_state_fn)(void* user_data, uint32_t window_id,
                                       uint32_t state, uint32_t previous);

// Callback fired when the set of displays, their arrangement, work areas or
// scale factors change. Fires on the backend UI thread; call get_screens to
// read the new layout.
typedef void (*laufey_display_changed_fn)(void* user_data);

typedef struct laufey_backend_api laufey_backend_api_t;

typedef int (*laufey_runtime_init_fn)(const laufey_backend_api_t* api);
#define LAUFEY_RUNTIME_INIT_SYMBOL "laufey_runtime_init"

typedef int (*laufey_runtime_start_fn)(void);
#define LAUFEY_RUNTIME_START_SYMBOL "laufey_runtime_start"

typedef void (*laufey_runtime_shutdown_fn)(void);
#define LAUFEY_RUNTIME_SHUTDOWN_SYMBOL "laufey_runtime_shutdown"

typedef struct laufey_value laufey_value_t;

typedef void (*laufey_js_call_fn)(void* user_data, uint32_t window_id,
                                  uint64_t call_id, const char* method_path,
                                  laufey_value_t* args);

// Callback for execute_js results. Pass NULL to execute_js for fire-and-forget.
typedef void (*laufey_js_result_fn)(laufey_value_t* result,
                                    laufey_value_t* error, void* user_data);

// Callback for print_to_pdf results (API >= 32). On success `data`/`len` carry
// the PDF bytes and `error` is NULL; on failure `data` is NULL, `len` is 0 and
// `error` is a NUL-terminated UTF-8 message. The bytes are owned by the backend
// and valid only for the duration of the call -- copy them if you need them
// beyond the callback.
typedef void (*laufey_pdf_result_fn)(const uint8_t* data, size_t len,
                                     const char* error, void* user_data);

typedef void (*laufey_menu_click_fn)(void* user_data, uint32_t window_id,
                                     const char* item_id);

// Keyboard event state
#define LAUFEY_KEY_PRESSED 0
#define LAUFEY_KEY_RELEASED 1

// Keyboard modifier flags (bitmask)
#define LAUFEY_MOD_SHIFT (1 << 0)
#define LAUFEY_MOD_CONTROL (1 << 1)
#define LAUFEY_MOD_ALT (1 << 2)
#define LAUFEY_MOD_META (1 << 3)

// Mouse button constants
#define LAUFEY_MOUSE_BUTTON_LEFT 0
#define LAUFEY_MOUSE_BUTTON_RIGHT 1
#define LAUFEY_MOUSE_BUTTON_MIDDLE 2
#define LAUFEY_MOUSE_BUTTON_BACK 3
#define LAUFEY_MOUSE_BUTTON_FORWARD 4

// Mouse event state
#define LAUFEY_MOUSE_PRESSED 0
#define LAUFEY_MOUSE_RELEASED 1

// Synthetic input kinds for test_inject_input (API >= 38).
#define LAUFEY_TEST_INPUT_KEY 0
#define LAUFEY_TEST_INPUT_MOUSE_MOVE 1
#define LAUFEY_TEST_INPUT_MOUSE_BUTTON 2
#define LAUFEY_TEST_INPUT_WHEEL 3
#define LAUFEY_TEST_INPUT_CURSOR_ENTER 4
#define LAUFEY_TEST_INPUT_CURSOR_LEAVE 5
#define LAUFEY_TEST_INPUT_MODIFIERS 6

// Test-only input record. Unused fields are ignored per `kind`. Wheel
// deltas are DOM-signed (positive Y is scroll down); the winit backend
// converts them through the same mapping a real OS event uses.
typedef struct laufey_test_input {
  int kind;            // LAUFEY_TEST_INPUT_*
  uint32_t modifiers;  // bitmask of LAUFEY_MOD_*
  const char* key;     // KEY
  const char* code;    // KEY
  bool pressed;        // KEY / MOUSE_BUTTON
  bool repeat;         // KEY
  int button;          // MOUSE_BUTTON: LAUFEY_MOUSE_BUTTON_*
  double x;            // MOVE / BUTTON / WHEEL / ENTER / LEAVE
  double y;
  double delta_x;  // WHEEL
  double delta_y;
  int delta_mode;  // WHEEL: LAUFEY_WHEEL_DELTA_*
} laufey_test_input_t;

// Dialog types
#define LAUFEY_DIALOG_ALERT 0
#define LAUFEY_DIALOG_CONFIRM 1
#define LAUFEY_DIALOG_PROMPT 2

// Dock / taskbar bounce / flash types. Values match macOS
// NSRequestUserAttentionType so the ABI passes through unchanged.
#define LAUFEY_DOCK_BOUNCE_INFORMATIONAL 10
#define LAUFEY_DOCK_BOUNCE_CRITICAL 0

// How many deep-link URLs a backend buffers while no open-url handler is
// registered (see set_open_url_handler). A cold start delivers one; the cap
// only bounds a pathological burst of links clicked before the runtime is up.
#define LAUFEY_MAX_PENDING_OPEN_URLS 16

// Callback fired when the user clicks the dock / taskbar icon for an app that
// has no visible windows (macOS only; Windows/Linux have no equivalent event).
// has_visible_windows is true if any app window is currently on-screen.
typedef void (*laufey_dock_reopen_fn)(void* user_data,
                                      bool has_visible_windows);

// Callback fired when the OS routes a custom URL scheme (deep link) to this
// app — e.g. `acme://open/document/42` — either at launch or while the app is
// already running (macOS only; see set_open_url_handler). `url` is the full
// URL as the OS delivered it, valid only for the duration of the call, and
// NOT validated: the embedder must check the scheme against the ones it
// registered before acting on it.
typedef void (*laufey_open_url_fn)(void* user_data, const char* url);

// How many second-instance launches a backend buffers while no handler is
// registered (see set_second_instance_handler); older ones are discarded.
#define LAUFEY_MAX_PENDING_SECOND_INSTANCES 16

// Callback fired in the running instance when the app is launched again while
// single-instance mode is on (see set_second_instance_handler). `argv` holds
// the `argc` arguments the new launch got after the executable name, `cwd`
// its working directory, all UTF-8 and valid only for the duration of the
// call. They are NOT validated beyond that: any process of the same user can
// send them.
typedef void (*laufey_second_instance_fn)(void* user_data,
                                          const char* const* argv, size_t argc,
                                          const char* cwd);

// --- Passkeys (API >= 37) ---------------------------------------------------
//
// Request kinds for passkey_request.
#define LAUFEY_PASSKEY_CREATE 0  // registration (navigator.credentials.create)
#define LAUFEY_PASSKEY_GET 1     // authentication (navigator.credentials.get)

// Capability flags returned by passkey_capabilities.
#define LAUFEY_PASSKEY_PLATFORM_AUTHENTICATOR (1u << 0)
#define LAUFEY_PASSKEY_SECURITY_KEYS (1u << 1)

// Largest options document passkey_request accepts, in bytes.
#define LAUFEY_PASSKEY_MAX_OPTIONS_BYTES 65536

// Result of passkey_request: `result_json` is a NUL-terminated UTF-8 JSON
// envelope, valid only for the duration of the call (copy it). See
// passkey_request for the format and the threading.
typedef void (*laufey_passkey_result_fn)(void* user_data,
                                         const char* result_json);

// --- UI-thread tasks (API >= 42) -------------------------------------------
//
// A task for dispatch_ui_task. Called EXACTLY ONCE: with `ran` true on the
// backend's UI thread, or with `ran` false (on any thread) when the backend
// can no longer run UI tasks because its event loop has ended or is ending.
typedef void (*laufey_ui_task_fn)(void* data, bool ran);

// --- Auth session (API >= 42) ----------------------------------------------
//
// Outcome of auth_session_start, passed to laufey_auth_session_result_fn.
#define LAUFEY_AUTH_SESSION_OK 0  // `value`: the callback URL
#define LAUFEY_AUTH_SESSION_CANCELLED \
  1  // the user closed the sheet / declined, the
     // anchor window closed, auth_session_cancel,
     // or test_cancel_auth_session
#define LAUFEY_AUTH_SESSION_NOT_SUPPORTED \
  2  // no OS auth session here (Windows, Linux,
     // Winit): use the system browser (RFC 8252)
#define LAUFEY_AUTH_SESSION_INVALID 3  // bad url / callback / window
#define LAUFEY_AUTH_SESSION_BUSY 4     // another session is in progress
#define LAUFEY_AUTH_SESSION_FAILED 5   // the OS refused or failed

// Capability bits returned by auth_session_capabilities.
#define LAUFEY_AUTH_SESSION_CAP_SUPPORTED (1u << 0)  // auth_session_start works
#define LAUFEY_AUTH_SESSION_CAP_EPHEMERAL \
  (1u << 1)  // LAUFEY_AUTH_SESSION_EPHEMERAL is honored
#define LAUFEY_AUTH_SESSION_CAP_HTTPS_CALLBACK \
  (1u << 2)  // an https:// callback (macOS 14.4+; the app
             // needs the host's associated domain)

// Flags for auth_session_start.
#define LAUFEY_AUTH_SESSION_EPHEMERAL \
  (1u << 0)  // no shared cookies / no consent prompt

// Longest url / callback auth_session_start accepts, in bytes.
#define LAUFEY_AUTH_SESSION_MAX_URL_BYTES 8192

// Result of auth_session_start: `status` is a LAUFEY_AUTH_SESSION_* outcome;
// `value` is the callback URL for LAUFEY_AUTH_SESSION_OK and a human-readable
// message otherwise (NUL-terminated UTF-8, never NULL, valid only for the
// duration of the call).
typedef void (*laufey_auth_session_result_fn)(void* user_data, int32_t status,
                                              const char* value);

// Callback fired when the user left-clicks a tray / status-bar icon.
// (Right-click is reserved for the tray's menu.)
typedef void (*laufey_tray_click_fn)(void* user_data, uint32_t tray_id);

// Notification event reasons (passed as `reason` to
// laufey_notification_event_fn).
#define LAUFEY_NOTIFICATION_SHOWN 0
#define LAUFEY_NOTIFICATION_CLICKED 1
#define LAUFEY_NOTIFICATION_CLOSED 2
#define LAUFEY_NOTIFICATION_ACTION 3

// Callback fired for events on a notification previously shown via
// `show_notification`. `action_id_or_null` is non-NULL only for
// LAUFEY_NOTIFICATION_ACTION (the id of the action button the user clicked).
typedef void (*laufey_notification_event_fn)(void* user_data,
                                             uint32_t notification_id,
                                             int reason,
                                             const char* action_id_or_null);

// --- Permissions / runtime authorization ---
//
// Capability kinds. `0` is reserved as INVALID so that an uninitialized int
// can't be mistaken for a real kind. Add new kinds at the end; this is
// part of the wire ABI.
#define LAUFEY_PERMISSION_INVALID 0
#define LAUFEY_PERMISSION_NOTIFICATIONS 1
// (API >= 41) request_permission only: ask for quiet ("provisional")
// notification authorization, which macOS grants without a prompt; the
// notifications go to the Notification Center without a banner or sound
// until the user keeps them. Elsewhere the same as NOTIFICATIONS.
// query_permission answers as for NOTIFICATIONS.
#define LAUFEY_PERMISSION_NOTIFICATIONS_PROVISIONAL 2

// Authorization state returned by permission callbacks.
//   GRANTED:     the runtime may use the capability.
//   DENIED:      the user (or policy) declined; do not prompt again -- the
//                OS won't show it.
//   PROMPT:      the user has not yet decided; calling request_permission
//                will display a system prompt.
//   UNSUPPORTED: this capability is not authorizable in the current
//                environment (e.g. unbundled macOS process with no
//                CFBundleIdentifier, or a backend that has no concept of
//                the kind on this platform).
#define LAUFEY_PERMISSION_STATUS_GRANTED 0
#define LAUFEY_PERMISSION_STATUS_DENIED 1
#define LAUFEY_PERMISSION_STATUS_PROMPT 2
#define LAUFEY_PERMISSION_STATUS_UNSUPPORTED 3

// Callback invoked with the result of request_permission / query_permission.
// Always fired on the UI thread (backends hop via their UI dispatch if the
// underlying OS API completes on a background queue).
typedef void (*laufey_permission_callback_fn)(void* user_data, int status);

// Callback for mouse click events.
typedef void (*laufey_mouse_click_fn)(
    void* user_data, uint32_t window_id,
    int state,           // LAUFEY_MOUSE_PRESSED or LAUFEY_MOUSE_RELEASED
    int button,          // LAUFEY_MOUSE_BUTTON_*
    double x,            // x position in window coordinates
    double y,            // y position in window coordinates
    uint32_t modifiers,  // bitmask of LAUFEY_MOD_* flags
    int32_t click_count  // 1 = single, 2 = double click
);

// Callback for mouse move events.
typedef void (*laufey_mouse_move_fn)(
    void* user_data, uint32_t window_id,
    double x,           // x position in window coordinates
    double y,           // y position in window coordinates
    uint32_t modifiers  // bitmask of LAUFEY_MOD_* flags
);

// Wheel delta mode
#define LAUFEY_WHEEL_DELTA_PIXEL 0
#define LAUFEY_WHEEL_DELTA_LINE 1
#define LAUFEY_WHEEL_DELTA_PAGE 2

// Callback for wheel (scroll) events.
typedef void (*laufey_wheel_fn)(
    void* user_data, uint32_t window_id,
    double delta_x,      // horizontal scroll; positive = right (DOM WheelEvent)
    double delta_y,      // vertical scroll; positive = down (DOM WheelEvent)
    double x,            // cursor x position in window coordinates
    double y,            // cursor y position in window coordinates
    uint32_t modifiers,  // bitmask of LAUFEY_MOD_* flags
    int32_t delta_mode   // LAUFEY_WHEEL_DELTA_*
);

// Callback for cursor enter/leave events (mouseenter/mouseleave).
typedef void (*laufey_cursor_enter_leave_fn)(
    void* user_data, uint32_t window_id,
    int entered,        // 1 = cursor entered window, 0 = cursor left window
    double x,           // cursor x position in window coordinates
    double y,           // cursor y position in window coordinates
    uint32_t modifiers  // bitmask of LAUFEY_MOD_* flags
);

// Callback for window move events.
typedef void (*laufey_move_fn)(void* user_data, uint32_t window_id,
                               int x,  // new x position
                               int y   // new y position
);

// Callback for window resize events.
typedef void (*laufey_resize_fn)(void* user_data, uint32_t window_id,
                                 int width,  // new width in pixels
                                 int height  // new height in pixels
);

// Callback for window focus/blur events.
typedef void (*laufey_focused_fn)(
    void* user_data, uint32_t window_id,
    int focused  // 1 = window gained focus, 0 = window lost focus
);

// Callback for keyboard events.
typedef void (*laufey_keyboard_event_fn)(
    void* user_data, uint32_t window_id,
    int state,        // LAUFEY_KEY_PRESSED or LAUFEY_KEY_RELEASED
    const char* key,  // logical key (W3C UI Events key value, e.g. "a",
                      // "Enter", "Shift")
    const char*
        code,  // physical key code (W3C UI Events code, e.g. "KeyA", "Enter")
    uint32_t modifiers,  // bitmask of LAUFEY_MOD_* flags
    bool repeat);

// Callback for window close requested events. Registering this handler
// (API >= 31) makes the backend WAIT: the window does not close on its own
// when the user clicks the close button. Do whatever work is needed --
// flush writes, close file handles, ask the user -- then call
// close_window(window_id) -- the existing API, safe to call from any
// thread, at any later time, synchronously or asynchronously -- to
// actually close it. Doing nothing leaves the window open indefinitely.
// With no handler registered, the window closes immediately on click, same
// as backends predating API 31.
//
// The defer is PROCESS-WIDE, not per-window: once registered, the handler
// receives (and holds open) EVERY window's close click. A handler that only
// wants to intercept some windows must call close_window(window_id) itself
// for the rest, or they become unclosable. (The Rust capi's per-window
// on_close_requested wrapper does this automatically for windows without a
// registered handler.)
typedef void (*laufey_close_requested_fn)(void* user_data, uint32_t window_id);

// Callback fired when a window finishes loading a navigation (the document and
// its subresources have loaded and the first content frame has been produced).
// Embedders use this to reveal a window created with LAUFEY_WINDOW_FLAG_HIDDEN
// only once there is real content to show. Fires once per completed navigation,
// so it may be called again on subsequent in-app navigations. API >= 27.
typedef void (*laufey_page_load_fn)(void* user_data, uint32_t window_id);

// --- Custom URL scheme handler (in-process app transport, API >= 26) --------
//
// A custom scheme handler lets the embedder service webview requests for a
// registered URL scheme (e.g. "app://...") entirely in-process, with no network
// socket. The backend delivers each request to a laufey_scheme_request_fn and
// the embedder streams a response back through the scheme_response_* vtable
// functions. Both request and response bodies are streamed.
//
// Every web-engine backend serves the built-in scheme "app". An embedder may
// register further schemes (e.g. its own "myapp"); each registered scheme is
// a real origin in the page: `location.origin` is `<scheme>://<host>`, the
// page is a secure context (crypto.subtle, secure-only APIs), cross-origin
// fetches carry `Origin: <scheme>://<host>`, CORS works, and localStorage /
// IndexedDB are per-origin (and persist wherever the backend keeps a
// persistent profile; the CEF host uses a per-process temporary one). See
// register_scheme_handler for the ordering contract that makes this hold on
// every backend.
//
// Header lists are encoded as a flat buffer of NUL-terminated strings laid out
// as name, value, name, value, ... -- `headers_len` is the total byte length
// including every terminating NUL. Header names are lowercase.

// Opaque handle for one in-flight scheme exchange. Valid from the moment the
// request callback fires until scheme_response_finish releases it.
typedef struct laufey_scheme_exchange laufey_scheme_exchange_t;

// Invoked when the webview issues a request for a registered scheme. Runs on a
// backend-internal thread; the embedder must not block it. `exchange` is used
// for every subsequent streaming call and is owned by the embedder, which must
// eventually call scheme_response_finish to release it. The request body (if
// any) is pulled via scheme_request_read_body.
typedef void (*laufey_scheme_request_fn)(void* user_data, uint32_t window_id,
                                         laufey_scheme_exchange_t* exchange,
                                         const char* method, const char* url,
                                         const char* headers,
                                         size_t headers_len);

// Invoked if the webview cancels the request (navigation away, window closed,
// the fetch aborted) before the response is finished. After this fires the
// embedder must stop writing and call scheme_response_finish to release
// `exchange`. Optional. Called at most once per exchange, on a backend
// thread, never once scheme_response_finish has returned; `exchange` stays
// valid for the call. The embedder may call scheme_response_finish from
// inside it, but must not block in it on a finish made on another thread.
// Where a backend can't observe a cancel (WebKitGTK before the response head
// is sent; WebView2 for a response it takes in one piece) it is not called,
// and the next scheme_response_write fails instead.
typedef void (*laufey_scheme_cancel_fn)(void* user_data,
                                        laufey_scheme_exchange_t* exchange);

struct laufey_backend_api {
  uint32_t version;
  void* backend_data;

  // Window lifecycle
  uint32_t (*create_window)(void* backend_data);
  void (*close_window)(void* backend_data, uint32_t window_id);

  // Create a window with creation-time style flags (see LAUFEY_WINDOW_FLAG_*).
  // Flags that can only be decided when the OS window is constructed
  // (frameless chrome, non-activating panel level) are honored here;
  // post-creation properties (size, position, resizable, always-on-top) are
  // still set via their respective setters. Backends added before API
  // version 25 leave this NULL, in which case callers fall back to
  // create_window and the flags are ignored.
  uint32_t (*create_window_ex)(void* backend_data, uint32_t flags);

  void (*navigate)(void* backend_data, uint32_t window_id, const char* url);
  void (*set_title)(void* backend_data, uint32_t window_id, const char* title);
  void (*execute_js)(void* backend_data, uint32_t window_id, const char* script,
                     laufey_js_result_fn callback, void* callback_data);
  // End the event loop: the backend's run loop returns and the process shuts
  // down the way it does when the last window closes. Windows that are still
  // open close without a close-requested event (quit is app-level; see
  // laufey_close_requested_fn). Any thread. Ends the loop even when
  // set_quit_on_last_window_closed(false) keeps it alive with no window.
  void (*quit)(void* backend_data);
  void (*set_window_size)(void* backend_data, uint32_t window_id, int width,
                          int height);
  void (*get_window_size)(void* backend_data, uint32_t window_id, int* width,
                          int* height);
  void (*set_window_position)(void* backend_data, uint32_t window_id, int x,
                              int y);
  void (*get_window_position)(void* backend_data, uint32_t window_id, int* x,
                              int* y);
  void (*set_resizable)(void* backend_data, uint32_t window_id, bool resizable);
  bool (*is_resizable)(void* backend_data, uint32_t window_id);
  void (*set_always_on_top)(void* backend_data, uint32_t window_id,
                            bool always_on_top);
  bool (*is_always_on_top)(void* backend_data, uint32_t window_id);
  bool (*is_visible)(void* backend_data, uint32_t window_id);
  void (*show)(void* backend_data, uint32_t window_id);
  void (*hide)(void* backend_data, uint32_t window_id);
  void (*focus)(void* backend_data, uint32_t window_id);
  void (*post_ui_task)(void* backend_data, void (*task)(void* data),
                       void* data);

  bool (*value_is_null)(laufey_value_t* val);
  bool (*value_is_bool)(laufey_value_t* val);
  bool (*value_is_int)(laufey_value_t* val);
  bool (*value_is_double)(laufey_value_t* val);
  bool (*value_is_string)(laufey_value_t* val);
  bool (*value_is_list)(laufey_value_t* val);
  bool (*value_is_dict)(laufey_value_t* val);
  bool (*value_is_binary)(laufey_value_t* val);
  bool (*value_is_callback)(laufey_value_t* val);

  bool (*value_get_bool)(laufey_value_t* val);
  int (*value_get_int)(laufey_value_t* val);
  double (*value_get_double)(laufey_value_t* val);

  char* (*value_get_string)(laufey_value_t* val, size_t* len_out);
  void (*value_free_string)(char* str);

  size_t (*value_list_size)(laufey_value_t* val);
  // A new value — a copy of the item at `index` — that the caller owns and
  // frees with value_free; NULL when there is none.
  laufey_value_t* (*value_list_get)(laufey_value_t* val, size_t index);

  // A new value — a copy of the entry under `key` — that the caller owns and
  // frees with value_free; NULL when there is none.
  laufey_value_t* (*value_dict_get)(laufey_value_t* dict, const char* key);
  bool (*value_dict_has)(laufey_value_t* dict, const char* key);
  size_t (*value_dict_size)(laufey_value_t* dict);

  char** (*value_dict_keys)(laufey_value_t* dict, size_t* count_out);
  void (*value_free_keys)(char** keys, size_t count);

  const void* (*value_get_binary)(laufey_value_t* val, size_t* len_out);

  uint64_t (*value_get_callback_id)(laufey_value_t* val);

  laufey_value_t* (*value_null)(void* backend_data);
  laufey_value_t* (*value_bool)(void* backend_data, bool val);
  laufey_value_t* (*value_int)(void* backend_data, int val);
  laufey_value_t* (*value_double)(void* backend_data, double val);
  laufey_value_t* (*value_string)(void* backend_data, const char* val);
  laufey_value_t* (*value_list)(void* backend_data);
  laufey_value_t* (*value_dict)(void* backend_data);
  laufey_value_t* (*value_binary)(void* backend_data, const void* data,
                                  size_t len);

  bool (*value_list_append)(laufey_value_t* list, laufey_value_t* val);
  bool (*value_list_set)(laufey_value_t* list, size_t index,
                         laufey_value_t* val);

  bool (*value_dict_set)(laufey_value_t* dict, const char* key,
                         laufey_value_t* val);

  void (*value_free)(laufey_value_t* val);

  void (*set_js_call_handler)(void* backend_data, laufey_js_call_fn handler,
                              void* user_data);
  void (*js_call_respond)(void* backend_data, uint64_t call_id,
                          laufey_value_t* result, laufey_value_t* error);
  void (*invoke_js_callback)(void* backend_data, uint64_t callback_id,
                             laufey_value_t* args);
  void (*release_js_callback)(void* backend_data, uint64_t callback_id);

  // Raw window/display handles for GPU surface creation.
  // Returns platform-specific handle:
  //   AppKit: NSView*, Win32: HWND, X11: Window (cast to void*), Wayland:
  //   wl_surface*
  void* (*get_window_handle)(void* backend_data, uint32_t window_id);
  // Returns platform-specific display handle:
  //   AppKit: NULL, Win32: NULL (or HINSTANCE), X11: Display*, Wayland:
  //   wl_display*
  void* (*get_display_handle)(void* backend_data, uint32_t window_id);
  // Returns LAUFEY_WINDOW_HANDLE_* constant identifying the platform
  int (*get_window_handle_type)(void* backend_data, uint32_t window_id);

  // Register a handler for keyboard input events (global, receives window_id in
  // callback).
  void (*set_keyboard_event_handler)(void* backend_data,
                                     laufey_keyboard_event_fn handler,
                                     void* user_data);

  // Register a handler for mouse click events.
  void (*set_mouse_click_handler)(void* backend_data,
                                  laufey_mouse_click_fn handler,
                                  void* user_data);

  // Register a handler for mouse move events.
  void (*set_mouse_move_handler)(void* backend_data,
                                 laufey_mouse_move_fn handler, void* user_data);

  // Register a handler for wheel (scroll) events.
  void (*set_wheel_handler)(void* backend_data, laufey_wheel_fn handler,
                            void* user_data);

  // Register a handler for cursor enter/leave events.
  void (*set_cursor_enter_leave_handler)(void* backend_data,
                                         laufey_cursor_enter_leave_fn handler,
                                         void* user_data);

  // Register a handler for window focus/blur events.
  void (*set_focused_handler)(void* backend_data, laufey_focused_fn handler,
                              void* user_data);

  // Register a handler for window resize events.
  void (*set_resize_handler)(void* backend_data, laufey_resize_fn handler,
                             void* user_data);

  // Register a handler for window move events.
  void (*set_move_handler)(void* backend_data, laufey_move_fn handler,
                           void* user_data);

  // Register a handler for window close requested events. See
  // laufey_close_requested_fn's doc comment for the defer-until-close_window
  // contract (API >= 31).
  void (*set_close_requested_handler)(void* backend_data,
                                      laufey_close_requested_fn handler,
                                      void* user_data);

  // Register a handler for page load-finished events (API >= 27). May be NULL
  // on older backends, in which case windows created hidden must be revealed by
  // the embedder through some other signal.
  void (*set_page_load_handler)(void* backend_data, laufey_page_load_fn handler,
                                void* user_data);

  void (*poll_js_calls)(void* backend_data);

  void (*set_js_call_notify)(void* backend_data,
                             void (*notify_fn)(void* notify_data),
                             void* notify_data);

  // Application menu. menu_template is a laufey_value_t list of menu items.
  // Each item is a dict with: label, submenu (list), role, type, id,
  // accelerator, checked (bool), icon (binary, PNG bytes), tooltip. When a
  // custom item (with "id") is clicked, on_click is called with the id. On
  // macOS the menu is applied to the global menu bar and swapped on window
  // focus. On Windows/Linux the menu is attached to the specific window.
  void (*set_application_menu)(void* backend_data, uint32_t window_id,
                               laufey_value_t* menu_template,
                               laufey_menu_click_fn on_click,
                               void* on_click_data);

  // Show a context menu at the given position (in window coordinates).
  // menu_template uses the same format as set_application_menu (list of menu
  // item dicts). on_click is called with the id of the clicked item.
  void (*show_context_menu)(void* backend_data, uint32_t window_id, int x,
                            int y, laufey_value_t* menu_template,
                            laufey_menu_click_fn on_click, void* on_click_data);

  // Open the DevTools inspector for the given window. A no-op when DevTools
  // are disabled (is_devtools_enabled, API >= 40).
  void (*open_devtools)(void* backend_data, uint32_t window_id);

  // Set the global JS namespace name for bindings (default: "Laufey").
  // Must be called before creating any windows.
  void (*set_js_namespace)(void* backend_data, const char* name);

  // Show a native dialog (alert, confirm, or prompt) and BLOCK until the
  // user dismisses it. Must be called on the main / UI thread (the same
  // thread the LAUFEY event loop runs on); backends use the platform's modal
  // run loop (`runModal` / `MessageBoxW` / `gtk_dialog_run`) which itself
  // pumps OS events while the dialog is up, so other LAUFEY windows continue
  // to render and respond.
  //
  // Returns 1 if the user clicked OK/Yes, 0 otherwise.
  // For LAUFEY_DIALOG_PROMPT, on a confirmed result `*out_input_value` is set
  // to a heap-allocated UTF-8 string the caller must free by calling
  // `string_free` (below). For alert/confirm, or on cancel, set to NULL.
  // Pass NULL for `out_input_value` if you don't need the input value.
  int (*show_dialog)(
      void* backend_data, uint32_t window_id,
      int dialog_type,  // LAUFEY_DIALOG_*
      const char* title, const char* message,
      const char* default_value,  // For prompt: default input text. NULL for
                                  // alert/confirm.
      char** out_input_value);

  // Free a string returned by `show_dialog` (the prompt input value).
  // Routed through the backend so the matching allocator is used (avoids
  // cross-runtime free hazards on Windows MSVC). Safe to call with NULL.
  void (*string_free)(void* backend_data, char* s);

  // --- Dock / taskbar ---
  //
  // Semantics are app-scoped on macOS (all operate on the process's Dock
  // tile) and focused-window-scoped on Windows/Linux (taskbar button for the
  // currently-focused LAUFEY window). Backends that don't support an operation
  // on a given platform leave the function pointer NULL.

  // Set or clear a short text badge on the app's dock / taskbar icon.
  // Pass NULL or "" to clear. macOS: NSDockTile badgeLabel. Windows: renders
  // text to a small overlay icon via GDI+ + ITaskbarList3::SetOverlayIcon.
  // Linux: prepends "(text) " to the focused window's title.
  void (*set_dock_badge)(void* backend_data, const char* badge_or_null);

  // Request the user's attention by bouncing the dock icon (macOS) or
  // flashing the focused window's taskbar button (Windows) or setting the
  // urgency hint (Linux). `type` is LAUFEY_DOCK_BOUNCE_*.
  void (*bounce_dock)(void* backend_data, int type);

  // Set a custom menu for the app's dock icon (macOS only).
  // menu_template uses the same format as set_application_menu. on_click is
  // called with the id of the clicked item. window_id in the callback will
  // be 0 since the menu is app-scoped. Windows/Linux: leave NULL.
  void (*set_dock_menu)(void* backend_data, laufey_value_t* menu_template,
                        laufey_menu_click_fn on_click, void* on_click_data);

  // Show or hide the app from the dock / task switcher (macOS activation
  // policy). Windows/Linux: leave NULL (no app-level equivalent).
  void (*set_dock_visible)(void* backend_data, bool visible);

  // Register a callback invoked when the user clicks the dock icon for an
  // app that has no visible windows (macOS). The backend always swallows
  // the default "show hidden window" behavior — the user callback is
  // informational. Windows/Linux: leave NULL.
  void (*set_dock_reopen_handler)(void* backend_data, laufey_dock_reopen_fn fn,
                                  void* user_data);

  // --- Tray / status-bar icon ---
  //
  // A tray icon is an explicitly-created, persistent icon in the OS status
  // area (macOS menu bar extras, Windows system tray, Linux AppIndicator).
  // Each call to create_tray_icon returns a new id; destroy removes it.
  // Backends that don't support tray icons leave these NULL.

  // Create a new empty tray icon. Returns a tray_id > 0, or 0 on failure.
  uint32_t (*create_tray_icon)(void* backend_data);

  // Destroy a tray icon created via create_tray_icon.
  void (*destroy_tray_icon)(void* backend_data, uint32_t tray_id);

  // Set the icon image (PNG-encoded bytes). Required before the icon is
  // visible on most platforms.
  void (*set_tray_icon)(void* backend_data, uint32_t tray_id,
                        const void* png_bytes, size_t len);

  // Set or clear the tooltip shown on hover. Pass NULL or "" to clear.
  void (*set_tray_tooltip)(void* backend_data, uint32_t tray_id,
                           const char* tooltip_or_null);

  // Set the context (right-click) menu. menu_template uses the same format
  // as set_application_menu. on_click is called with the id of the clicked
  // item; the window_id argument of the callback is 0 (tray menus are
  // app-scoped, not window-scoped). Pass NULL menu_template to clear.
  void (*set_tray_menu)(void* backend_data, uint32_t tray_id,
                        laufey_value_t* menu_template,
                        laufey_menu_click_fn on_click, void* on_click_data);

  // Register a handler for left-click on the tray icon.
  void (*set_tray_click_handler)(void* backend_data, uint32_t tray_id,
                                 laufey_tray_click_fn handler, void* user_data);

  // Register a handler for left-double-click. Fires after a quick second
  // click; the single-click handler (if any) still fires for the first
  // click. No-op on Linux (AppIndicator has no click events).
  void (*set_tray_double_click_handler)(void* backend_data, uint32_t tray_id,
                                        laufey_tray_click_fn handler,
                                        void* user_data);

  // Set the icon used when the OS is in dark mode. When set, the backend
  // swaps between the primary (light) icon from set_tray_icon and this
  // one based on the current system appearance. Pass NULL/zero len to
  // clear the dark variant (then the primary icon is used in both modes).
  void (*set_tray_icon_dark)(void* backend_data, uint32_t tray_id,
                             const void* png_bytes, size_t len);

  // Get the tray icon's bounding rectangle in screen coordinates, using the
  // same top-left-origin, density-independent-pixel space as
  // get_window_position / set_window_position so a window can be anchored to
  // the icon. Writes x/y/width/height (any may be NULL) and returns true on
  // success. Returns false if the id is unknown, the icon has no on-screen
  // position yet, or the backend/platform can't report it (NULL fn pointer
  // included). Used to position tray popover panels under the icon.
  bool (*get_tray_icon_bounds)(void* backend_data, uint32_t tray_id, int* x,
                               int* y, int* width, int* height);

  // --- Notifications (system / desktop) ---
  //
  // App-scoped. `options` is a laufey_value_t dict mirroring (a subset of)
  // the Web Notification API constructor options:
  //   "title"               string  (required)
  //   "body"                string
  //   "icon"                binary  (PNG bytes)
  //   "tag"                 string  — replaces an existing notification
  //                                   with the same tag rather than
  //                                   creating a new one
  //   "silent"              bool    — suppress system notification sound
  //   "require_interaction" bool    — keep visible until user dismisses
  //   "actions"             list of dicts, each {"id": string,
  //                                   "title": string} — action buttons.
  //                                   Ignored on platforms that don't
  //                                   support them
  //                                   (LAUFEY_NOTIFICATION_CAP_ACTIONS).
  //   "schedule_at"         number  — (API >= 41) Unix time in milliseconds
  //                                   to deliver at; a time in the past (or
  //                                   none) shows it now. Requires a "tag",
  //                                   which identifies it to
  //                                   cancel_notification and
  //                                   list_scheduled_notifications.
  //   "data"                string  — (API >= 41) opaque, at most
  //                                   LAUFEY_NOTIFICATION_MAX_DATA_BYTES;
  //                                   handed back in notification responses.
  //
  // A "tag" (at most LAUFEY_NOTIFICATION_MAX_TAG_BYTES) also identifies the
  // notification to the response handler and across launches.
  //
  // Ownership: the backend takes ownership of `options` (calls value_free
  // on it), matching the convention used by set_application_menu /
  // show_context_menu / set_tray_menu.
  //
  // Returns a notification_id > 0 used by close_notification and as the
  // first arg to the event callback. Returns 0 on failure (including when
  // notifications aren't supported on the current platform / backend).
  //
  // Pass NULL for `on_event` for fire-and-forget. Backends that don't
  // support tracking events for a given OS API may invoke only some of
  // the LAUFEY_NOTIFICATION_* reasons.
  uint32_t (*show_notification)(void* backend_data, laufey_value_t* options,
                                laufey_notification_event_fn on_event,
                                void* user_data);

  // Close a notification previously shown via show_notification. No-op
  // if the id is unknown or the notification has already been dismissed.
  void (*close_notification)(void* backend_data, uint32_t notification_id);

  // --- Permissions / runtime authorization ---
  //
  // Ask the OS for the current authorization status of a capability
  // (`kind` is LAUFEY_PERMISSION_*). Does NOT prompt the user. The callback
  // is invoked on the UI thread with one of LAUFEY_PERMISSION_STATUS_*.
  // If this function pointer is NULL the runtime treats every kind as
  // UNSUPPORTED.
  void (*query_permission)(void* backend_data, int kind,
                           laufey_permission_callback_fn cb, void* user_data);

  // Request authorization for a capability. If the current status is
  // PROMPT the OS will display a system prompt; otherwise the cached
  // decision is returned without re-prompting (the OS will not show a
  // second prompt for a kind the user has already decided -- this is an
  // OS-level constraint, not a laufey one). The callback fires on the UI
  // thread. NULL function pointer is equivalent to a stub that reports
  // UNSUPPORTED.
  void (*request_permission)(void* backend_data, int kind,
                             laufey_permission_callback_fn cb, void* user_data);

  // --- Clipboard (system, API >= 27) -----------------------------------------
  //
  // App-scoped access to the system clipboard's plain-text content, mirroring
  // the Web `navigator.clipboard.readText()` / `writeText()` surface. Backends
  // added before API version 27 leave these two pointers NULL; callers must
  // null-check.

  // Read the clipboard's text content. Returns a heap-allocated UTF-8 string
  // the caller must free via `string_free`, or NULL if the clipboard is empty
  // or holds no text representation. Any thread from API 39 (see the
  // clipboard section at the end of this table); before that, the UI thread.
  char* (*read_clipboard_text)(void* backend_data);

  // Replace the clipboard's content with `text` (UTF-8). Pass NULL or "" to
  // clear the clipboard. Any thread from API 39; before that, the UI thread.
  void (*write_clipboard_text)(void* backend_data, const char* text);

  // --- Custom URL scheme handler (API >= 26) ---------------------------------
  //
  // Register `handler` to service every request for `scheme` (the scheme name
  // only, e.g. "app", without "://"; RFC 3986 grammar — a letter followed by
  // letters, digits, "+", "-" or "."; case-insensitive, stored lowercase; an
  // invalid name is logged and ignored). Call it once per scheme: the built-in
  // "app" plus any of the embedder's own. One handler serves all registered
  // schemes — a later call replaces the handler for every scheme and adds the
  // new scheme — so dispatch on the request URL. A NULL handler unregisters.
  // `on_cancel` may be NULL.
  //
  // ORDERING CONTRACT: register every scheme BEFORE creating the first window.
  // The engines read their scheme tables when a web view is created (WKWebView
  // configuration, WebKitGTK web context, WebView2 environment — WebView2 in
  // particular fixes the set for the process at its first environment), so a
  // scheme registered after a window exists is not served by that window on
  // WKWebView and not at all on WebView2 (WebKitGTK, whose web context is
  // shared, does apply it to existing windows); the WebView backends log a
  // warning. CEF is different: Chromium learns custom schemes at process
  // start, before the runtime is loaded, so the embedder must declare them
  // when launching the CEF host — `--laufey-custom-schemes=myapp,other` or
  // `LAUFEY_CUSTOM_SCHEMES=myapp,other` (comma-separated; "app" is implicit).
  // A declared scheme is served in every window whenever it is registered; a
  // scheme that is registered but was not declared is still served, but as a
  // non-standard scheme (opaque origin, insecure context), with a warning.
  //
  // Backends added before API version 26 leave these four pointers NULL;
  // callers must null-check and fall back to a socket transport.
  void (*register_scheme_handler)(void* backend_data, const char* scheme,
                                  laufey_scheme_request_fn handler,
                                  laufey_scheme_cancel_fn on_cancel,
                                  void* user_data);

  // Pull up to `cap` bytes of the request body into `buf`. Returns the number
  // of bytes read (>0), 0 at end of body, or -1 on error. May block until body
  // data is available, so the embedder should call it off the critical path of
  // its async runtime (a request with no body returns 0 immediately).
  intptr_t (*scheme_request_read_body)(void* backend_data,
                                       laufey_scheme_exchange_t* exchange,
                                       uint8_t* buf, size_t cap);

  // Send the response status code and headers. Must be called exactly once,
  // before the first scheme_response_write. `headers` uses the flat encoding
  // documented above; pass NULL/0 for none.
  void (*scheme_response_begin)(void* backend_data,
                                laufey_scheme_exchange_t* exchange, int status,
                                const char* headers, size_t headers_len);

  // Append `len` bytes to the response body. May be called repeatedly after
  // scheme_response_begin. Returns the bytes accepted, or -1 if the consumer
  // has gone away (the embedder should then stop and call
  // scheme_response_finish).
  intptr_t (*scheme_response_write)(void* backend_data,
                                    laufey_scheme_exchange_t* exchange,
                                    const uint8_t* buf, size_t len);

  // Complete the response and release `exchange`. After this returns the handle
  // is invalid. Call exactly once per exchange (including after a cancel).
  // Finishing an exchange that never called scheme_response_begin gives the
  // page no response: its request fails (a network error), on every backend.
  void (*scheme_response_finish)(void* backend_data,
                                 laufey_scheme_exchange_t* exchange);

  // --- Window opacity (API >= 28) --------------------------------------------
  //
  // Set the window's overall opacity as a uniform factor in [0.0, 1.0], where
  // 1.0 is fully opaque (the default) and 0.0 is fully transparent. Unlike
  // LAUFEY_WINDOW_FLAG_TRANSPARENT (which honors the page's per-pixel alpha),
  // this fades the *entire* window — web content and native chrome alike — by
  // the same amount, like CSS `opacity` on the whole window. Values are clamped
  // to [0.0, 1.0]. macOS: NSWindow.alphaValue. Windows: a layered window with
  // SetLayeredWindowAttributes(LWA_ALPHA). Linux: gtk_widget_set_opacity. The
  // Winit backend has no window-opacity API and leaves this NULL. Backends
  // added before API version 28 also leave it NULL; callers must null-check.
  void (*set_window_opacity)(void* backend_data, uint32_t window_id,
                             double opacity);

  // Return the window's current opacity in [0.0, 1.0]. Returns 1.0 if the id is
  // unknown or the backend can't report it. NULL on backends older than API
  // version 28.
  double (*get_window_opacity)(void* backend_data, uint32_t window_id);

  // --- Test hooks (API >= 30) ---
  //
  // Test-only. Synthesizes a click on the menu/tray item with id `item_id` by
  // invoking the same click-dispatch path a real click uses (looking up the
  // registered on_click handler by id and calling it). Returns true if an item
  // with that id was registered and its handler ran. Lets automated e2e tests
  // exercise menu/tray click round-trips without OS-level input injection or
  // main-thread UI access. NULL on backends that don't implement it.
  bool (*test_click_menu_item)(void* backend_data, const char* item_id);

  // Test-only. Synthesizes a close-requested event on window_id through the
  // same dispatch code a real OS close click runs. Returns true if a
  // registered close_requested_handler deferred the close (the window is
  // still open), false if the close proceeded (no handler was registered).
  // "Proceeded" means initiated, not necessarily completed: some backends
  // (winit, CEF) queue the actual close, so callers should poll window
  // state rather than assert immediately after a false return. Two
  // deliberate differences from a real close click: the handler runs
  // synchronously on the CALLING thread, not the backend UI thread, and
  // window_id is not validated -- an unknown id still reaches a registered
  // handler (and returns false with no effect otherwise). NULL on backends
  // that don't implement it (API < 31); callers should treat a NULL hook as
  // unavailable, like test_click_menu_item.
  bool (*test_trigger_close_requested)(void* backend_data, uint32_t window_id);

  // --- Print to PDF (API >= 32) ----------------------------------------------
  //
  // Render window `window_id`'s current page to a PDF document. The result is
  // delivered as bytes through `callback`: on success `data`/`len` hold the
  // PDF and the callback's `error` is NULL. Backends never touch the
  // filesystem — writing the bytes to a caller-requested path is handled
  // entirely by the capi layer above this API. Rendering is asynchronous: the
  // callback fires on the UI thread once it completes, and the backend MUST
  // invoke it exactly once on every path, including internal scheduling
  // failures. Backends that cannot produce a PDF invoke the callback with an
  // "unsupported" error rather than crashing. NULL on backends older than API
  // version 32; callers must null-check.
  void (*print_to_pdf)(void* backend_data, uint32_t window_id,
                       laufey_pdf_result_fn callback, void* callback_data);

  // --- Click passthrough (API >= 33) -----------------------------------------
  //
  // When enabled the window ignores ALL mouse input — clicks, moves, wheel —
  // and every event falls through to whatever window is beneath it, like
  // Electron's setIgnoreMouseEvents(true). Keyboard input is unaffected.
  // Intended for frameless/transparent overlay windows (HUDs, notification
  // toasts, screen annotations). A live setter that can be toggled at any
  // time. macOS: NSWindow.ignoresMouseEvents. Windows: WS_EX_TRANSPARENT |
  // WS_EX_LAYERED on the top-level window. Linux: an empty input shape region
  // (best-effort under a reparenting X11 window manager — pair it with a
  // frameless window). NULL on backends older than API version 33; callers
  // must null-check.
  void (*set_click_passthrough)(void* backend_data, uint32_t window_id,
                                bool enabled);

  // Return whether click passthrough is currently enabled for the window.
  // Returns false if the id is unknown or the backend can't report it. NULL
  // on backends older than API version 33.
  bool (*is_click_passthrough)(void* backend_data, uint32_t window_id);

  // --- Click passthrough forwarding (API >= 34) ------------------------------
  //
  // While a window has click passthrough enabled, forwarding keeps the
  // embedder's mouse handlers (set_mouse_click_handler /
  // set_mouse_move_handler / set_wheel_handler) fed for that window even
  // though the OS delivers the events to whatever is beneath it — like
  // Electron's setIgnoreMouseEvents(true, { forward: true }). Observation
  // only: the events cannot be consumed (the window below still receives
  // them), and by the time a handler runs the OS has already routed the
  // event. Delivery comes from a global OS observer that hit-tests the
  // window's frame, so events are reported only while passthrough is active
  // and the cursor is over the (visible) window; while passthrough is
  // disabled the flag has no effect — normal per-window delivery already
  // fires the handlers. Enables the standard interactive-overlay pattern:
  // observe moves, toggle passthrough off when the cursor enters an
  // interactive region.
  //
  // Implemented on macOS (NSEvent global monitor; mouse observation needs no
  // extra permission). Backends/platforms without global observation
  // (Windows, Linux, winit — see docs/window-management.md) currently ignore
  // the flag and report false from the getter. NULL on backends older than
  // API version 34; callers must null-check.
  void (*set_click_passthrough_forward)(void* backend_data, uint32_t window_id,
                                        bool forward);

  // Return whether forwarding is currently enabled for the window. Returns
  // false if the id is unknown or the backend doesn't support forwarding.
  // NULL on backends older than API version 34.
  bool (*is_click_passthrough_forward)(void* backend_data, uint32_t window_id);

  // --- Deep links / custom URL schemes (API >= 35) ---------------------------
  //
  // Register a callback invoked when the OS routes a custom URL scheme this
  // app has registered — `acme://open/document/42` — to the app. Registering
  // the scheme itself is NOT laufey's job: the embedder declares it in the
  // bundle it ships (macOS `CFBundleURLTypes`, Linux `.desktop`
  // `x-scheme-handler/<scheme>`, Windows `HKCU\Software\Classes\<scheme>`).
  // See docs/deep-links.md.
  //
  // macOS only, for the same reason as set_dock_reopen_handler: AppKit
  // delivers the URL to the *running* app as an Apple Event
  // (`application:openURLs:`), so one process handles every link. Windows and
  // Linux have no equivalent — the OS spawns a NEW process with the URL in
  // argv, and turning that into "focus the running app" needs a
  // single-instance lock, an app identity, and a policy on whether a second
  // instance is even wrong. All three belong to the embedder that owns the
  // packaging, so those backends leave this pointer NULL and the embedder
  // reads its own argv.
  //
  // Delivery contract:
  //   - URLs that arrive before a handler is registered are buffered and
  //     flushed, in order, as soon as one is. A runtime loaded on a worker
  //     thread is never ready when a launch URL lands, so without this every
  //     cold-start deep link would be dropped. At most
  //     LAUFEY_MAX_PENDING_OPEN_URLS are held; older ones are discarded.
  //   - The callback fires on the backend UI thread, synchronously, like
  //     every other event handler.
  //   - Passing a NULL `fn` clears the handler and re-arms buffering.
  //   - macOS hands over an array of URLs; the backend fans it out into one
  //     call per URL.
  //
  // NULL on backends older than API version 35; callers must null-check.
  void (*set_open_url_handler)(void* backend_data, laufey_open_url_fn fn,
                               void* user_data);

  // Test-only. Synthesizes a deep-link delivery of `url` through the same
  // dispatch path a real OS-routed URL takes — including the buffer, so a
  // call made before any handler is registered is replayed on registration
  // just like a cold-start URL. Returns true if the URL was delivered to a
  // registered handler, false if it was buffered instead. Lets automated e2e
  // tests cover the round-trip without OS-level scheme registration or Apple
  // Events. NULL on backends that don't implement it (API < 35, or any
  // non-macOS backend); callers should treat a NULL hook as unavailable,
  // like test_click_menu_item.
  bool (*test_trigger_open_url)(void* backend_data, const char* url);

  // --- Single instance (API >= 36) -------------------------------------------
  //
  // Register a callback invoked in the running instance when the app is
  // launched again. Single-instance mode is opt-in and decided before the
  // runtime loads: "singleInstance": true in laufey-launch.json, or
  // LAUFEY_SINGLE_INSTANCE=1, together with an app id (LAUFEY_APP_ID /
  // "appId"). The second launch forwards its arguments and working directory
  // to the running instance and exits 0 without starting a web engine or the
  // runtime; the running instance brings its window to the front and calls
  // this handler. See docs/deep-links.md.
  //
  // This is how a deep link or a file reaches an app that is already running
  // on Windows and Linux (the OS starts `app "<url>"`) and on macOS when the
  // binary is exec'd directly; on macOS LaunchServices uses
  // set_open_url_handler instead. laufey does not parse the arguments: the
  // embedder decides what is a URL, a file or a flag, as it does with its own
  // argv at a cold start.
  //
  // Delivery contract (as set_open_url_handler):
  //   - The callback fires on the backend UI thread.
  //   - Launches that arrive before a handler is registered are buffered (at
  //     most LAUFEY_MAX_PENDING_SECOND_INSTANCES, oldest dropped) and
  //     delivered, in order, on the registering thread when one is.
  //   - Passing a NULL `fn` clears the handler and re-arms buffering.
  //
  // Set on the CEF and WebView backends on every desktop OS (the handler
  // simply never fires unless single-instance mode is on). NULL on the Winit
  // backend, which has no single-instance lock, and on backends older than
  // API version 36; callers must null-check.
  void (*set_second_instance_handler)(void* backend_data,
                                      laufey_second_instance_fn fn,
                                      void* user_data);

  // --- Passkeys (API >= 37) --------------------------------------------------
  //
  // WebAuthn ceremonies through the OS platform authenticator, for apps whose
  // page origin can't satisfy the RP ID (a custom scheme or loopback origin).
  // The wire format is the one of @clerk/electron-passkeys, so its JS bridge
  // can sit on top unchanged. See docs/passkeys.md.
  //
  // Backends: macOS 12+ AuthenticationServices (ASAuthorizationController,
  // platform + security-key providers), Windows 10 1903+ webauthn.dll. Linux
  // (WebKitGTK, CEF) has no platform API: capabilities 0, requests answer
  // not_supported. NULL on the Winit backend and on backends older than API
  // version 37; callers must null-check.

  // LAUFEY_PASSKEY_* flags: what a request can use right now. Any thread.
  uint32_t (*passkey_capabilities)(void* backend_data);

  // Start a ceremony. `kind` is LAUFEY_PASSKEY_CREATE or _GET; `options_json`
  // is PublicKeyCredentialCreationOptions / RequestOptions as JSON with
  // base64url (unpadded) binary fields: {rp:{id,name}, user:{id,name,
  // displayName}, challenge, pubKeyCredParams, timeout,
  // authenticatorSelection, attestation, excludeCredentials} for create,
  // {challenge, rpId, timeout, userVerification, allowCredentials} for get.
  // The options are untrusted input: the backend parses them strictly
  // (<= LAUFEY_PASSKEY_MAX_OPTIONS_BYTES, UTF-8, required fields, base64url,
  // RP ID syntax) and passes the RP ID to the OS, which enforces that the app
  // may use it (macOS: the associated domain; Windows accepts any RP ID).
  //
  // `window_id` anchors the OS sheet / dialog; 0 means the focused window.
  // One ceremony runs at a time per app: a request made while another is in
  // progress is refused ("unknown", "a passkey request is already in
  // progress").
  //
  // `callback` is invoked EXACTLY ONCE with the envelope
  //   {"ok":true,"credential":{id, rawId, type, authenticatorAttachment,
  //     response:{clientDataJSON, attestationObject, transports} (create) |
  //     response:{clientDataJSON, authenticatorData, signature, userHandle}
  //     (get)}}
  //   {"ok":false,"error":{"code":"cancelled"|"invalid_rp"|"not_supported"|
  //     "timeout"|"unknown","message":"..."}}
  // and on ANY thread: synchronously on the calling thread, before
  // passkey_request returns, when the request is refused up front (invalid
  // options, busy, unsupported platform); on the main thread on macOS; on
  // the ceremony's worker thread on Windows; on an internal timer thread for
  // a timeout. Embedders must not assume a thread and must not block in it.
  // Any thread may call passkey_request. A NULL callback makes it a no-op.
  void (*passkey_request)(void* backend_data, uint32_t window_id, uint32_t kind,
                          const char* options_json,
                          laufey_passkey_result_fn callback, void* user_data);

  // --- Device pixel ratio (API >= 38) ----------------------------------------
  //
  // Physical pixels per density-independent pixel for this window, the same
  // ratio as the Web `window.devicePixelRatio`. Live: a window that moves to
  // another monitor reports the new scale on the next call. Returns 1.0 if
  // the id is unknown. NULL on backends older than API version 38; callers
  // must null-check and treat NULL as 1.0.
  double (*get_window_scale_factor)(void* backend_data, uint32_t window_id);

  // --- Content-view origin (API >= 38) ---------------------------------------
  //
  // Top-left of the content view in the same DIP, top-left-origin screen
  // space as get_window_position. Differs from get_window_position by the
  // title-bar / frame chrome, so `inner + clientX/Y` is MouseEvent.screenX/Y.
  // Writes 0,0 if the id is unknown. NULL on backends older than API 38;
  // callers must null-check and fall back to get_window_position.
  void (*get_window_inner_position)(void* backend_data, uint32_t window_id,
                                    int* x, int* y);

  // --- Outer window size (API >= 38) -----------------------------------------
  //
  // Chrome-inclusive size in the same DIP space as get_window_size
  // (`window.outerWidth` / `outerHeight`). A frameless window matches
  // get_window_size. Writes 0,0 if the id is unknown. NULL on backends
  // older than this field; callers must null-check and fall back to
  // get_window_size.
  void (*get_window_outer_size)(void* backend_data, uint32_t window_id,
                                int* width, int* height);

  // --- Test input injection (API >= 38) --------------------------------------
  //
  // Test-only. Posts a synthetic input event through the same dispatch a
  // real OS event uses for this backend (winit: WindowEvent handlers;
  // CEF / WebView: Dispatch* after native translation). Returns true if
  // the event was accepted (known kind; winit also requires a live
  // window). NULL on backends that do not implement it; callers should
  // treat NULL like the other test hooks.
  bool (*test_inject_input)(void* backend_data, uint32_t window_id,
                            const laufey_test_input_t* event);

  // --- Window state (API >= 38) ----------------------------------------------
  //
  // Apply a LAUFEY_WINDOW_ACTION_* to the window. Any thread; the backend
  // hops to its UI thread. Asynchronous on platforms that animate the change
  // (macOS fullscreen, minimize): the getter and the state handler report it
  // once it took effect. An action the window is already in is a no-op.
  // UNMAXIMIZE on a window that is not maximized, RESTORE on one that is not
  // minimized and LEAVE_FULLSCREEN outside fullscreen do nothing.
  void (*set_window_state)(void* backend_data, uint32_t window_id, int action);

  // LAUFEY_WINDOW_STATE_* bits for the window as the OS reports it now; 0 for
  // an unknown id or a normal window.
  uint32_t (*get_window_state)(void* backend_data, uint32_t window_id);

  // Register the (process-wide) window-state handler; NULL clears it.
  void (*set_window_state_handler)(void* backend_data,
                                   laufey_window_state_fn handler,
                                   void* user_data);

  // --- Size constraints (API >= 38) ------------------------------------------
  //
  // Minimum and maximum size of the window, in the same units as
  // set_window_size (content size in DIP on every backend; see
  // docs/window-management.md). 0 on an axis means "no constraint" for that
  // axis. The OS enforces them while the user resizes, and set_window_size
  // clamps to them; if the current size is outside the new range the window
  // is resized into it. A maximum smaller than the minimum on an axis is
  // raised to the minimum.
  void (*set_window_size_constraints)(void* backend_data, uint32_t window_id,
                                      int min_width, int min_height,
                                      int max_width, int max_height);
  // Writes the constraints last set (0 = none). Any pointer may be NULL.
  void (*get_window_size_constraints)(void* backend_data, uint32_t window_id,
                                      int* min_width, int* min_height,
                                      int* max_width, int* max_height);

  // --- Screens (API >= 38) ---------------------------------------------------
  //
  // Fill up to `capacity` entries of `out` with the connected displays, the
  // primary one first, and return how many displays there are (which may be
  // more than `capacity`: call again with a bigger buffer). `out` may be NULL
  // when `capacity` is 0. Any thread.
  size_t (*get_screens)(void* backend_data, laufey_screen_t* out,
                        size_t capacity);

  // The id of the display the window is on (the one it overlaps most), or 0
  // when unknown.
  int64_t (*get_window_screen)(void* backend_data, uint32_t window_id);

  // Register the display-changed handler; NULL clears it.
  void (*set_display_changed_handler)(void* backend_data,
                                      laufey_display_changed_fn handler,
                                      void* user_data);

  // --- Capabilities (API >= 38) ----------------------------------------------
  //
  // LAUFEY_WINDOW_CAP_* bits for this backend on this OS. Any thread.
  uint32_t (*window_capabilities)(void* backend_data);

  // --- Title bar and backdrop (API >= 38) ------------------------------------
  //
  // Set the title bar style (LAUFEY_TITLEBAR_*) at runtime. Returns false
  // (and changes nothing) when the backend can't do that style, which is
  // everything but macOS today. Frameless windows have no title bar to style.
  bool (*set_window_titlebar_style)(void* backend_data, uint32_t window_id,
                                    int style);

  // Move the macOS traffic lights so the close button's top-left sits at
  // (x, y) from the window's top-left corner, in points. Negative values put
  // them back where the system draws them. Kept across resizes, fullscreen
  // and title changes. Returns false when unsupported.
  bool (*set_window_traffic_light_position)(void* backend_data,
                                            uint32_t window_id, int x, int y);

  // Put a LAUFEY_BACKDROP_* behind the web content (`material` is a
  // LAUFEY_VIBRANCY_* for LAUFEY_BACKDROP_VIBRANCY and ignored otherwise).
  // Returns false (and changes nothing) when this backend / OS can't show
  // that backdrop; see window_capabilities.
  bool (*set_window_backdrop)(void* backend_data, uint32_t window_id,
                              int backdrop, int material);

  // --- Normal bounds (API >= 38) ---------------------------------------------
  //
  // The bounds the window returns to when it leaves the maximized, minimized
  // or fullscreen state (the current bounds for a normal window): the
  // position as get_window_position and the size as get_window_size, so
  // set_window_position + set_window_size restore it. This is what an app
  // persists to reopen its window where the user left it. Returns false for
  // an unknown id. Any pointer may be NULL.
  bool (*get_window_normal_bounds)(void* backend_data, uint32_t window_id,
                                   int* x, int* y, int* width, int* height);

  // --- App lifetime (API >= 38) ----------------------------------------------
  //
  // Whether the event loop ends when the last window closes (true, the
  // default). A tray / menu-bar app passes false so it keeps running with no
  // window; it then ends through quit(). On macOS an app whose activation
  // policy is Accessory (set_dock_visible(false)) also stays alive when its
  // last window closes, whatever this is set to. quit() always ends the loop.
  void (*set_quit_on_last_window_closed)(void* backend_data, bool quit);

  // --- Drag and drop (API >= 39) ---------------------------------------------
  //
  // Register the (process-wide) file-drop handler; NULL clears it. See
  // laufey_file_drop_fn and docs/drag-and-drop.md. The page keeps receiving
  // its own DOM drag events (with File objects, never paths); this handler is
  // where the native paths are. LAUFEY_WINDOW_CAP_FILE_DROP says whether it
  // fires. Fires on the backend UI thread.
  void (*set_file_drop_handler)(void* backend_data, laufey_file_drop_fn handler,
                                void* user_data);

  // Start dragging `count` files (absolute paths, UTF-8) out of the window to
  // another app or the desktop, as a copy, with `icon_png` (PNG bytes; NULL /
  // 0 for the platform's file icon) under the pointer. The OS drives the drag
  // modally from the pointer's current position, so call it while the left
  // mouse button is held: from the page's `dragstart` (cancel the page's own
  // drag with preventDefault()) or a `mousedown` + move. Any thread; the
  // backend hops to its UI thread.
  //
  // `callback` is invoked EXACTLY ONCE with a LAUFEY_DRAG_RESULT_*: on the
  // backend UI thread (Windows: its I/O thread) when the drag ends, or
  // synchronously on the calling thread with FAILED, before start_file_drag
  // returns, when the request is refused (no paths, more than
  // LAUFEY_MAX_DROP_PATHS, a path that is not an existing absolute path, or a
  // backend without drag-out). FAILED also comes from the UI thread when the
  // drag can't start there (no left button held, another drag in progress, an
  // unknown window). NULL callback: fire and forget.
  // LAUFEY_WINDOW_CAP_FILE_DRAG_OUT says whether the backend can do it at all.
  void (*start_file_drag)(void* backend_data, uint32_t window_id,
                          const char* const* paths, size_t count,
                          const uint8_t* icon_png, size_t icon_len,
                          laufey_drag_result_fn callback, void* user_data);

  // Test-only. Delivers a file-drag phase to the registered file-drop handler
  // through the same dispatch the OS path uses (paths are copied and capped
  // the same way). Returns true if a handler received it. NULL on backends
  // that do not implement it.
  bool (*test_trigger_file_drop)(void* backend_data, uint32_t window_id,
                                 int phase, double x, double y,
                                 const char* const* paths, size_t count);

  // --- Native file dialogs (API >= 39) ---------------------------------------
  //
  // Show an open / save / folder dialog: the OS's own (NSOpenPanel /
  // NSSavePanel, IFileOpenDialog / IFileSaveDialog, GtkFileChooserNative, the
  // last one portal-aware). Any thread: the backend shows it on its UI thread
  // and returns at once, so the runtime thread never blocks. `window_id` != 0
  // makes the dialog modal to that window (a sheet on macOS) where
  // LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL is reported; 0 shows an app-level
  // dialog. One file dialog is open at a time per app.
  //
  // Returns the dialog id (> 0) that cancel_file_dialog takes and the callback
  // carries, or 0 when the request was answered at once. `callback` fires
  // EXACTLY ONCE: on the backend UI thread (Windows: the backend's own I/O
  // thread, so the dialog's modal loop never stalls the engine's UI thread)
  // when the dialog closes, or
  // synchronously on the calling thread before this returns 0 (BUSY while
  // another dialog is open; FAILED for bad options, a NULL `options`, or a
  // backend without dialogs). A NULL `callback` returns 0 and shows nothing.
  // `options` is copied before this returns.
  uint32_t (*show_file_dialog)(void* backend_data, uint32_t window_id,
                               const laufey_file_dialog_options_t* options,
                               laufey_file_dialog_result_fn callback,
                               void* user_data);

  // Close an open file dialog as if the user cancelled it (its callback gets
  // LAUFEY_FILE_DIALOG_CANCELLED). Returns false when no dialog with that id
  // is open. Any thread.
  bool (*cancel_file_dialog)(void* backend_data, uint32_t dialog_id);

  // Test-only. Acts on the open file dialog as a user would: CANCEL closes
  // it; ACCEPT first puts `path` (absolute, UTF-8; NULL keeps the dialog's
  // own selection) into it where the platform lets a program do that, then
  // accepts, so the result comes back through the dialog's own completion
  // path. Returns false when no dialog is open or the platform can't accept
  // programmatically (e.g. a portal dialog). Any thread. NULL on backends
  // that do not implement it.
  bool (*test_file_dialog_respond)(void* backend_data, int action,
                                   const char* path);

  // --- Clipboard: HTML, images, formats, changes (API >= 39) -----------------
  //
  // From API 39 every clipboard call, read_clipboard_text and
  // write_clipboard_text included, may be made from any thread: the backend
  // runs it where the platform requires (the main / GTK thread on macOS and
  // Linux). Reads larger than LAUFEY_CLIPBOARD_MAX_READ_BYTES answer NULL.

  // LAUFEY_CLIPBOARD_CAP_* bits for this backend on this OS. Any thread.
  uint32_t (*clipboard_capabilities)(void* backend_data);

  // The clipboard's HTML (UTF-8, a fragment or a document as the source app
  // wrote it), freed with string_free, or NULL when it holds none. On Windows
  // the CF_HTML header is stripped and the fragment returned.
  char* (*read_clipboard_html)(void* backend_data);

  // Replace the clipboard with `html`, plus `text_or_null` as the plain-text
  // alternative for apps that don't take HTML (NULL: none). Returns false when
  // the write failed.
  bool (*write_clipboard_html)(void* backend_data, const char* html,
                               const char* text_or_null);

  // The clipboard's image as PNG bytes (`*len_out` set), freed with
  // buffer_free, or NULL when it holds none. Other image formats on the
  // clipboard (TIFF on macOS, a DIB on Windows, any pixbuf format on Linux)
  // are converted to PNG.
  uint8_t* (*read_clipboard_image)(void* backend_data, size_t* len_out);

  // Replace the clipboard with the PNG image `png` (also offered in the
  // platform's native image format so every app can paste it). Returns false
  // when `png` is not a decodable PNG or the write failed.
  bool (*write_clipboard_image)(void* backend_data, const uint8_t* png,
                                size_t len);

  // The kinds of content on the clipboard, as MIME types separated by '\n'
  // (NUL-terminated, freed with string_free): "text/plain", "text/html",
  // "image/png" (any image), "text/uri-list" (files), "text/rtf". An empty
  // string for an empty clipboard; NULL on failure.
  char* (*read_clipboard_formats)(void* backend_data);

  // Register the (process-wide) clipboard-change handler; NULL clears it.
  // macOS has no change notification, so the backend polls the pasteboard's
  // change count (twice a second) only while a handler is set; Windows uses
  // AddClipboardFormatListener; Linux the GTK clipboard's owner-change (X11
  // needs XFixes; on Wayland GTK only hears of changes while one of the app's
  // windows has focus). LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS says whether it
  // fires. Any thread; the handler fires on the backend UI thread (Windows:
  // the backend's I/O thread).
  void (*set_clipboard_change_handler)(void* backend_data,
                                       laufey_clipboard_change_fn handler,
                                       void* user_data);

  // Free a buffer returned by read_clipboard_image. Safe with NULL.
  void (*buffer_free)(void* backend_data, void* buffer);

  // --- Global shortcuts, launch at login, DevTools (API >= 40) ---------------
  //
  // LAUFEY_SYSTEM_CAP_* bits for this backend on this OS and session. Any
  // thread.
  uint32_t (*system_capabilities)(void* backend_data);

  // Register the (process-wide) global-shortcut handler; NULL clears it. See
  // laufey_shortcut_fn and docs/global-shortcuts.md. Any thread.
  void (*set_shortcut_handler)(void* backend_data, laufey_shortcut_fn handler,
                               void* user_data);

  // Bind `accelerator` ("CommandOrControl+Shift+K", the menu accelerator
  // syntax; see docs/global-shortcuts.md) system-wide: macOS Carbon
  // RegisterEventHotKey, Windows RegisterHotKey, X11 XGrabKey on the root
  // window, the XDG GlobalShortcuts portal on Wayland. Any thread; never
  // blocks. `callback` fires EXACTLY ONCE with a LAUFEY_SHORTCUT_* status:
  // synchronously on the calling thread for INVALID, ALREADY_REGISTERED,
  // NOT_SUPPORTED and a NULL accelerator, else from the backend once the OS
  // answered (on Wayland, after the user answered the portal's dialog). A
  // NULL callback: fire and forget.
  void (*register_shortcut)(void* backend_data, const char* accelerator,
                            laufey_shortcut_result_fn callback,
                            void* user_data);

  // Release a shortcut this app registered (any spelling of it). Returns
  // false if it wasn't registered. Its handler stops firing before this
  // returns; the OS binding is released right after. Any thread.
  bool (*unregister_shortcut)(void* backend_data, const char* accelerator);

  // Release every shortcut this app registered. Any thread.
  void (*unregister_all_shortcuts)(void* backend_data);

  // The canonical accelerators currently registered, separated by '\n'
  // (freed with string_free; "" when there are none). Any thread.
  char* (*list_shortcuts)(void* backend_data);

  // The canonical form of `accelerator` (what register_shortcut reports and
  // the handler receives; "Ctrl+Shift+K"), freed with string_free, or NULL
  // when it doesn't parse. Any thread.
  char* (*canonicalize_accelerator)(void* backend_data,
                                    const char* accelerator);

  // Test-only. Fires the shortcut handler for a REGISTERED `accelerator`
  // (any spelling) through the dispatch the OS press uses. Returns false
  // when it isn't registered or no handler is set. NULL on backends that do
  // not implement it.
  bool (*test_trigger_shortcut)(void* backend_data, const char* accelerator);

  // Whether the app starts when the user logs in: a LAUFEY_LOGIN_ITEM_*
  // state. macOS 13+: SMAppService.mainAppService; Windows: the
  // HKCU\Software\Microsoft\Windows\CurrentVersion\Run value; Linux: the
  // XDG autostart entry ~/.config/autostart/<app id>.desktop. The entry is
  // named after LAUFEY_APP_ID (or the launch file's "appId"), else the
  // executable's name. Any thread.
  int (*get_launch_at_login)(void* backend_data);

  // Turn launch at login on or off. Returns the state afterwards (ENABLED,
  // REQUIRES_APPROVAL, DISABLED), NOT_SUPPORTED, or FAILED with a message in
  // `*error_out` (if non-NULL; freed with string_free). Any thread.
  int (*set_launch_at_login)(void* backend_data, bool enabled,
                             char** error_out);

  // Close the window's DevTools (opened with open_devtools, or by the user).
  // Any thread.
  void (*close_devtools)(void* backend_data, uint32_t window_id);

  // Whether the window's DevTools are open. Any thread.
  bool (*is_devtools_open)(void* backend_data, uint32_t window_id);

  // Whether DevTools can be opened at all: false when the app launched with
  // LAUFEY_INSPECTABLE=0 (or "inspectable": false in laufey-launch.json),
  // which turns off the engine's inspector (WKWebView `inspectable`,
  // WebView2 AreDevToolsEnabled, WebKitGTK enable-developer-extras, CEF's
  // DevTools and remote debugging) so neither open_devtools nor the user
  // (a shortcut, the context menu) can open them. With a `window_id`, the
  // window's engine setting as read back from the engine; with 0, the
  // launch setting. Any thread.
  bool (*is_devtools_enabled)(void* backend_data, uint32_t window_id);

  // --- Menus: context-menu close, accelerators (API >= 41) ------------------
  //
  // An app menu item's "accelerator" (the global-shortcut syntax:
  // "CommandOrControl+Shift+K", see docs/menus.md) fires the item from the
  // keyboard while its window has the focus: NSMenu key equivalents on macOS,
  // an accelerator table on Windows (TranslateAccelerator in the message loop
  // and WebView2's AcceleratorKeyPressed), a GtkAccelGroup on the WebKitGTK
  // window, CEF Views accelerators on the CEF backend (Windows and Linux).
  // A context menu shows the accelerators without binding them.

  // LAUFEY_MENU_CAP_* bits for this backend on this OS. Any thread.
  uint32_t (*menu_capabilities)(void* backend_data);

  // show_context_menu, plus `on_closed` (may be NULL), which fires EXACTLY
  // ONCE when the menu closes: see laufey_menu_closed_fn. The backend takes
  // ownership of `menu_template` (calls value_free on it), as for
  // show_notification. Never blocks: the menu shows on the UI thread. A NULL
  // template, an unknown window or a backend that can't show the menu fires
  // `on_closed` (on the calling thread or the UI thread) without a click.
  void (*show_context_menu_ex)(void* backend_data, uint32_t window_id, int x,
                               int y, laufey_value_t* menu_template,
                               laufey_menu_click_fn on_click,
                               void* on_click_data,
                               laufey_menu_closed_fn on_closed,
                               void* on_closed_data);

  // Test-only. Closes the open context menu as Escape would (its on_closed
  // fires, with no click). Returns false when no context menu is open. Any
  // thread. NULL on backends that do not implement it.
  bool (*test_dismiss_context_menu)(void* backend_data);

  // Test-only. Presses `accelerator` (any spelling) in window `window_id`
  // through the backend's own accelerator dispatch (performKeyEquivalent:,
  // TranslateAccelerator, gtk_accel_groups_activate, the CEF window's
  // accelerator), as a key press reaching the window would. Returns true if
  // an app menu item fired. Any thread. NULL on backends that do not
  // implement it.
  bool (*test_trigger_menu_accelerator)(void* backend_data, uint32_t window_id,
                                        const char* accelerator);

  // --- Notifications: scheduling, actions, responses (API >= 41) -----------
  //
  // See docs/notifications.md for each platform's mechanism: macOS
  // UNUserNotificationCenter (calendar / time-interval triggers, categories),
  // Windows toast notifications (ScheduledToastNotification, a COM activator
  // registered per user for the app's AppUserModelID), Linux
  // org.freedesktop.Notifications over D-Bus (laufey's own scheduler).

  // LAUFEY_NOTIFICATION_CAP_* bits for this backend on this OS (and, on
  // Linux, whether a notification server is running). Any thread.
  uint32_t (*notification_capabilities)(void* backend_data);

  // Register the (process-wide) handler for notification responses no live
  // show_notification callback owns; NULL clears it. Responses that arrive
  // while none is registered are held (LAUFEY_MAX_PENDING_NOTIFICATION_
  // RESPONSES) and delivered, in order and with "launch": true, on the
  // registering thread when one is. Any thread.
  void (*set_notification_response_handler)(
      void* backend_data, laufey_notification_response_fn handler,
      void* user_data);

  // The notifications scheduled with "schedule_at" and not delivered yet.
  // `callback` fires EXACTLY ONCE, on a backend thread or synchronously on
  // the calling thread. Any thread.
  void (*list_scheduled_notifications)(void* backend_data,
                                       laufey_notification_list_fn callback,
                                       void* user_data);

  // Cancel the scheduled notification with `tag` and remove delivered ones
  // carrying it from the notification center. A live notification's
  // callback gets LAUFEY_NOTIFICATION_CLOSED. Any thread; the OS may finish
  // asynchronously, but a later list_scheduled_notifications no longer
  // includes it.
  void (*cancel_notification)(void* backend_data, const char* tag);

  // Test-only. Delivers a click on the notification `tag` (the body for a
  // NULL `action_id`, else that action button) through the dispatch the OS
  // response uses: the live notification's callback when this process shows
  // it, else the response handler (or the pending buffer). Returns true if a
  // callback or handler received it. Any thread. NULL on backends that do
  // not implement it.
  bool (*test_notification_respond)(void* backend_data, const char* tag,
                                    const char* action_id);

  // --- UI-thread tasks (API >= 42) -----------------------------------------
  //
  // post_ui_task with a delivery guarantee, for runtimes that hop onto the UI
  // thread (AppKit / Win32 / GTK objects, a native extension's code) and
  // wait for the result. `task(data, true)` runs on the UI thread, queued
  // behind the work already posted there (never inline, even when called on
  // the UI thread). If the backend's event loop has ended, or ends before the
  // task ran, `task(data, false)` is called instead: synchronously when the
  // loop has already ended, else on the thread ending it, before the backend
  // calls laufey_runtime_shutdown. So a runtime thread waiting for a task is
  // always released, and never blocks the backend's shutdown. Any thread. A
  // NULL `task` is a no-op. NULL on backends older than API version 42.
  void (*dispatch_ui_task)(void* backend_data, laufey_ui_task_fn task,
                           void* data);

  // True when the calling thread is the backend's UI thread (the thread that
  // runs dispatch_ui_task's tasks: the process main thread on macOS and for
  // the WebView backends, CEF's TID_UI, the Winit event loop's thread). Any
  // thread.
  bool (*is_ui_thread)(void* backend_data);

  // --- Auth session (API >= 42) --------------------------------------------
  //
  // A browser sign-in the OS runs for the app and ends at a callback URL
  // (RFC 8252 "native app" OAuth): macOS ASWebAuthenticationSession, a sheet
  // on the app's window that shares Safari's cookies (or none, with
  // LAUFEY_AUTH_SESSION_EPHEMERAL), with a real "cancelled" when the user
  // closes it. Windows and Linux have no OS equivalent: RFC 8252 says to
  // open the system browser and receive the redirect through a loopback
  // listener or a claimed URL scheme, which the embedder does itself; there
  // the capabilities are 0 and every request answers NOT_SUPPORTED. See
  // docs/auth-session.md.

  // LAUFEY_AUTH_SESSION_CAP_* bits for this backend on this OS. Any thread.
  uint32_t (*auth_session_capabilities)(void* backend_data);

  // Start a session at `url` (http or https, <=
  // LAUFEY_AUTH_SESSION_MAX_URL_BYTES). `callback` is the custom scheme the
  // sign-in ends at ("myapp", the URL "myapp:..." completes the session) or,
  // with LAUFEY_AUTH_SESSION_CAP_HTTPS_CALLBACK, an https URL
  // ("https://example.com/auth/done": a navigation to that host and path
  // completes it). `window_id` anchors the sheet; 0 means the key window (or
  // the app's first visible window). `flags` are LAUFEY_AUTH_SESSION_*
  // flags. One session runs at a time per app: another request meanwhile
  // answers BUSY.
  //
  // `on_result` is called EXACTLY ONCE, on ANY thread: synchronously before
  // auth_session_start returns for a refusal (NOT_SUPPORTED, INVALID, BUSY);
  // later from an OS thread or the UI thread otherwise. A session anchored
  // to a window that closes ends CANCELLED, and so does one still running
  // when the event loop ends. Embedders must not block in it. A NULL
  // `on_result` makes the call a no-op. Any thread.
  void (*auth_session_start)(void* backend_data, uint32_t window_id,
                             const char* url, const char* callback,
                             uint32_t flags,
                             laufey_auth_session_result_fn on_result,
                             void* user_data);

  // Test-only. Ends the running session as the user closing its sheet
  // would (its result is CANCELLED). Returns false when no session is
  // running. Any thread. NULL on backends that do not implement it.
  bool (*test_cancel_auth_session)(void* backend_data);

  // --- Auth session cancel (API >= 43) -------------------------------------

  // Cancel the running session: the app gave up on it (the page cancelled,
  // a timeout). Its sheet closes, and its `on_result` is called with
  // CANCELLED, still exactly once; the slot is then free for the next
  // session. Returns false, and does nothing, when no session is running,
  // which is always the case where sessions are not supported (Windows,
  // Linux, Winit). Any thread. NULL on backends older than API version 43.
  bool (*auth_session_cancel)(void* backend_data);
};

#ifdef __cplusplus
}
#endif

#endif  // LAUFEY_H
