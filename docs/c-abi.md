# C ABI

laufey is built around a single C header,
[`capi/include/laufey.h`](https://github.com/littledivy/laufey/blob/main/capi/include/laufey.h).
It defines the boundary between a **backend** (a native executable embedding a
browser engine) and a **runtime** (a shared library holding the application
logic). The backend implements the ABI; the runtime consumes it.

`LAUFEY_API_VERSION` (currently `43`) versions the contract. The `version` field
on the API table lets a runtime detect the backend's vintage and avoid calling
function pointers a backend predates (older backends leave new pointers `NULL`).

## Runtime entry points

A runtime is a `.dylib`/`.so`/`.dll` that exports three symbols:

| Symbol (`*_SYMBOL` macro) | Signature                              | Role                                                                           |
| ------------------------- | -------------------------------------- | ------------------------------------------------------------------------------ |
| `laufey_runtime_init`     | `int(const laufey_backend_api_t* api)` | Backend hands the runtime the API table. Stash it; return 0 on success.        |
| `laufey_runtime_start`    | `int(void)`                            | Run application setup (create windows, register handlers). Returns when ready. |
| `laufey_runtime_shutdown` | `void(void)`                           | Tear down before the process exits.                                            |

The backend `dlopen`s the runtime, resolves these symbols, calls `init` then
`start`, and drives the OS event loop. Control flows backend → runtime through
the API table, and runtime → backend through the registered callbacks.

## The API table

`laufey_backend_api_t` is a struct of function pointers plus two data fields:

```c
struct laufey_backend_api {
  uint32_t version;     // == LAUFEY_API_VERSION the backend was built against
  void*    backend_data; // opaque; pass back as the first arg of every call
  /* ... function pointers ... */
};
```

Every function takes `backend_data` as its first argument, so the table is a
hand-rolled vtable with no global state. Windows are referenced by an opaque
`uint32_t window_id` returned from `create_window`.

The pointers group into:

- **Window lifecycle** — `create_window`, `create_window_ex` (style flags, see
  `LAUFEY_WINDOW_FLAG_*`, including `LAUFEY_WINDOW_FLAG_TRANSPARENT` for a
  transparent background), `close_window`, `navigate`, `set_title`,
  size/position get+set, `set_resizable`/`is_resizable`,
  `set_always_on_top`/`is_always_on_top`,
  `set_window_opacity`/`get_window_opacity` (whole-window alpha, API ≥ 28),
  `get_window_scale_factor` (`window.devicePixelRatio`, API ≥ 38),
  `get_window_inner_position` (content-view origin, API ≥ 38),
  `get_window_outer_size` (`window.outerWidth` / `outerHeight`, API ≥ 38),
  `test_inject_input` (synthetic pointer / key / wheel, API ≥ 38),
  `show`/`hide`/`is_visible`, `focus`, `quit`, `post_ui_task`, and from API 42
  `dispatch_ui_task` / `is_ui_thread` (see
  [UI-thread tasks](#ui-thread-tasks-api--42) below).
- **Value marshalling** — the `value_*` family (below).
- **JavaScript interop** — `set_js_call_handler`, `js_call_respond`,
  `invoke_js_callback`, `release_js_callback`, `execute_js`, `set_js_namespace`,
  `poll_js_calls`, `set_js_call_notify`.
- **Event handlers** — `set_keyboard_event_handler`, `set_mouse_click_handler`,
  `set_mouse_move_handler`, `set_wheel_handler`,
  `set_cursor_enter_leave_handler`, `set_focused_handler`, `set_resize_handler`,
  `set_move_handler`, `set_close_requested_handler` (defer-until-close_window
  contract as of API ≥ 31 — see below).
- **Window handles** — `get_window_handle`, `get_display_handle`,
  `get_window_handle_type` (for GPU surface creation).
- **Menus** — `set_application_menu`, `show_context_menu`, and from API 41
  `show_context_menu_ex` (a close callback fired exactly once; the backend takes
  the template), `menu_capabilities`, and the `test_trigger_menu_accelerator` /
  `test_dismiss_context_menu` hooks. App-menu accelerators fire their items on
  every desktop backend from API 41. See [menus.md](menus.md).
- **DevTools** — `open_devtools` and, from API 40, `close_devtools`,
  `is_devtools_open` and `is_devtools_enabled` (read back from the engine;
  `LAUFEY_INSPECTABLE=0` / `"inspectable": false` turns DevTools off for the
  process). See [devtools.md](devtools.md).
- **Dialogs** — `show_dialog`, `string_free`.
- **Dock / taskbar** — `set_dock_badge`, `bounce_dock`, `set_dock_menu`,
  `set_dock_visible`, `set_dock_reopen_handler`.
- **Deep links** (API ≥ 35) — `set_open_url_handler`, macOS-only and buffered
  until a handler registers (see [deep-links.md](deep-links.md)).
- **Single instance** (API ≥ 36) — `set_second_instance_handler`: a later launch
  of the app forwarded by the opt-in single-instance lock (arguments and working
  directory), buffered until a handler registers. CEF and WebView on every
  desktop OS; `NULL` on Winit (see
  [deep-links.md](deep-links.md#single-instance)).
- **Window state, screens and chrome** (API ≥ 38) — `set_window_state` /
  `get_window_state` / `set_window_state_handler` (maximize, minimize, restore,
  fullscreen with change events), `set_window_size_constraints` /
  `get_window_size_constraints`, `get_screens` / `get_window_screen` /
  `set_display_changed_handler`, `window_capabilities` (what this backend / OS
  can do), `set_window_titlebar_style` / `set_window_traffic_light_position`
  (macOS), `set_window_backdrop` (Mica / Acrylic on Windows 11, vibrancy on
  macOS), `get_window_normal_bounds`, `set_quit_on_last_window_closed`, and the
  `quit` contract (ends the loop like the last window closing). See
  [window-management.md](window-management.md). API 39 adds the
  `LAUFEY_WINDOW_CAP_FILE_*` bits for drag and drop and file dialogs.
- **Drag and drop** (API ≥ 39) — `set_file_drop_handler` (files dragged over and
  dropped on a window, with native paths; the page keeps its DOM drag events),
  `start_file_drag` (drag files out to other apps), and the
  `test_trigger_file_drop` hook. See [drag-and-drop.md](drag-and-drop.md).
- **File dialogs** (API ≥ 39) — `show_file_dialog` (the OS's open / save /
  folder dialog, from any thread, resolved through a callback; never blocks the
  caller), `cancel_file_dialog`, and the `test_file_dialog_respond` hook. See
  [file-dialogs.md](file-dialogs.md).
- **Clipboard** — `read_clipboard_text` / `write_clipboard_text` (API ≥ 27) and,
  from API 39, `clipboard_capabilities`, HTML and PNG reads and writes,
  `read_clipboard_formats`, `set_clipboard_change_handler` and `buffer_free`;
  every clipboard call may then be made from any thread. See
  [clipboard.md](clipboard.md).
- **Passkeys** (API ≥ 37) — `passkey_capabilities`, `passkey_request`: WebAuthn
  ceremonies through the OS platform authenticator, one at a time, with the
  result (a JSON envelope) delivered exactly once on any thread. macOS and
  Windows; not supported on Linux; `NULL` on Winit (see
  [passkeys.md](passkeys.md)).
- **Global shortcuts** (API ≥ 40) — `system_capabilities`,
  `set_shortcut_handler`, `register_shortcut` (any thread, never blocks, the
  result delivered exactly once: OK with the canonical accelerator, or INVALID /
  CONFLICT / ALREADY_REGISTERED / NOT_SUPPORTED / DENIED / FAILED),
  `unregister_shortcut`, `unregister_all_shortcuts`, `list_shortcuts`,
  `canonicalize_accelerator` and the `test_trigger_shortcut` hook. macOS,
  Windows, X11 and the Wayland portal; `NULL` on Winit (see
  [global-shortcuts.md](global-shortcuts.md)).
- **Launch at login** (API ≥ 40) — `get_launch_at_login`, `set_launch_at_login`
  (`SMAppService`, the HKCU `Run` key, XDG autostart; `NULL` on Winit; see
  [launch-at-login.md](launch-at-login.md)).
- **Tray** — `create_tray_icon`, `destroy_tray_icon`, `set_tray_icon`(`_dark`),
  `set_tray_tooltip`, `set_tray_menu`, click handlers, `get_tray_icon_bounds`.
- **Notifications** — `show_notification`, `close_notification`, and from API 41
  the `"schedule_at"` and `"data"` options, `notification_capabilities`,
  `set_notification_response_handler` (clicks no live callback owns, buffered
  until a handler registers: the cold-start click),
  `list_scheduled_notifications`, `cancel_notification` and the
  `test_notification_respond` hook. See [notifications.md](notifications.md).
- **Permissions** — `query_permission`, `request_permission`
  (`LAUFEY_PERMISSION_NOTIFICATIONS_PROVISIONAL` from API 41).
- **Auth session** (API ≥ 42) — `auth_session_capabilities`,
  `auth_session_start` (an OS-run browser sign-in that ends at a callback URL,
  one at a time, the result delivered exactly once on any thread), from API 43
  `auth_session_cancel` (the app ends the running session: `CANCELLED`, once),
  and the `test_cancel_auth_session` hook. macOS `ASWebAuthenticationSession`
  (WKWebView and CEF); `NOT_SUPPORTED` on Windows and Linux, where RFC 8252 says
  to use the system browser, and `auth_session_cancel` answers `false` there;
  `NULL` on Winit, except `auth_session_cancel`, which answers `false` (see
  [auth-session.md](auth-session.md)).
- **Custom URL scheme handler** (API ≥ 26) — `register_scheme_handler`,
  `scheme_request_read_body`, `scheme_response_begin`, `scheme_response_write`,
  `scheme_response_finish`.

See [the feature pages](window-management.md) for behavior and per-platform
differences.

## Values (`laufey_value_t`)

`laufey_value_t` is an opaque, dynamically-typed value used for everything
crossing the JS ↔ native boundary (call arguments, results, menu templates,
notification options). It models the JSON types plus binary blobs and
JS-callback handles:

- **Inspect:** `value_is_null` / `_bool` / `_int` / `_double` / `_string` /
  `_list` / `_dict` / `_binary` / `_callback`.
- **Read:** `value_get_bool` / `_int` / `_double`; `value_get_string` (returns a
  heap buffer freed with `value_free_string`); list access (`value_list_size`,
  `value_list_get`); dict access (`value_dict_get`, `value_dict_has`,
  `value_dict_size`, `value_dict_keys` + `value_free_keys`); `value_get_binary`;
  `value_get_callback_id`.
- **Build:** `value_null` / `_bool` / `_int` / `_double` / `_string` / `_list` /
  `_dict` / `_binary` (constructors take `backend_data`), then
  `value_list_append` / `_set`, `value_dict_set`.
- **Free:** `value_free`.

**Ownership.** Constructors return a value the caller owns and must `value_free`
(unless handed off). Functions that accept a template — `set_application_menu`,
`show_context_menu`, `set_tray_menu`, `set_dock_menu`, `show_notification` —
take ownership of the passed value and free it themselves.

A `_callback` value wraps a JS function passed as an argument: read its
`value_get_callback_id`, then call it later with `invoke_js_callback(id, args)`
and free it with `release_js_callback(id)`.

## JavaScript call flow

1. The runtime exposes a namespace in the page (`set_js_namespace`, default
   `"Laufey"`) and registers `set_js_call_handler`.
2. Page JS calls `Laufey.someMethod(args…)`; the backend invokes the handler
   with a `call_id`, the method name, and the arguments as a `laufey_value_t`
   list.
3. The runtime does its work and replies with
   `js_call_respond(call_id, result,
   error)` — resolving or rejecting the
   JS-side promise.

`execute_js` runs a script in a window and delivers its result/error through a
`laufey_js_result_fn`. When the runtime services calls off the UI thread, the
backend signals readiness via `set_js_call_notify` and the runtime drains the
queue with `poll_js_calls`.

## Custom URL scheme handler (API ≥ 26)

A custom scheme handler lets the runtime service webview requests for a
registered URL scheme (e.g. `app://`) entirely in-process — no network socket,
port, or `localhost` exposure. This is how the Deno desktop runtime serves an
embedded browser over an in-memory byte channel instead of a TCP loopback.

1. The runtime calls
   `register_scheme_handler(scheme, handler, on_cancel,
   user_data)` with the
   scheme name (e.g. `"app"`, no `://`). The backend registers it as a standard,
   secure, fetch/CORS-enabled scheme and installs a handler factory. Call it
   once per scheme: every web-engine backend serves the built-in `app`, and an
   embedder may add its own (`"myapp"`). One handler serves every registered
   scheme (a later call replaces the handler and adds the scheme), so dispatch
   on the request URL.
2. When the webview requests `<scheme>://…`, the backend invokes `handler` with
   request metadata (method, URL, headers) and an opaque
   `laufey_scheme_exchange_t*`. Headers use a flat `name\0value\0…\0` encoding
   (`headers_len` counts every terminating NUL).
3. The runtime pulls the request body (if any) with `scheme_request_read_body`
   (returns >0 bytes, 0 at EOF, <0 on error), then streams the response:
   `scheme_response_begin(status, headers)` once, `scheme_response_write(bytes)`
   any number of times, and `scheme_response_finish` to release the exchange.
   Every web-engine backend buffers the whole request body before it invokes
   `handler`, so `scheme_request_read_body` returns immediately. On Linux this
   needs WebKitGTK 2.40 or newer (`webkit_uri_scheme_request_get_http_body`); a
   WebView backend built against an older WebKitGTK forwards every request with
   an empty body.

If the webview cancels (navigation away, window closed) before the response
finishes, `scheme_response_write` / `scheme_request_read_body` return negative;
the runtime should stop and call `scheme_response_finish`. Backends predating
API version 26 leave these pointers `NULL`; the runtime must null-check and fall
back to a socket transport.

### Registered schemes are real origins

A page served over a registered scheme behaves like an `https` origin on every
web-engine backend: `location.origin` is `<scheme>://<host>`, `isSecureContext`
is true (so `crypto.subtle` and other secure-only APIs work), same-origin
`fetch` streams the response as the runtime writes it, cross-origin requests
carry `Origin: <scheme>://<host>` and honor CORS, and `localStorage` / IndexedDB
are scoped to that origin. On the WebView backends that storage persists across
launches; the CEF host keeps its profile in a per-process temporary directory,
so there it lasts for one run, as for any other origin. Scheme names follow the
RFC 3986 grammar (a letter, then letters, digits, `+`, `-`, `.`), are
case-insensitive and stored lowercase; an invalid name is logged and ignored.

**Register every scheme before creating the first window.** The engines read
their scheme tables when a web view is created — the `WKWebViewConfiguration` on
macOS, the default `WebKitWebContext` on Linux, the WebView2 environment options
on Windows — so a scheme registered after a window exists is not served by that
window. WebView2 goes further: all environments in a process share one user data
folder and must carry identical custom-scheme registrations, so the backend
freezes the set at the first window and later registrations are logged and
ignored. WebKitGTK is the lenient one: its schemes live on the shared web
context, so a late registration reaches existing windows too — portable code
must not rely on it. The WebView backends warn on stderr when the contract is
broken.

The CEF backend needs one more step. Chromium learns custom schemes in
`CefApp::OnRegisterCustomSchemes`, which runs during `CefInitialize` in every
process — before the runtime library is loaded and can call
`register_scheme_handler`. The embedder therefore declares its schemes when it
launches the CEF host, either as `--laufey-custom-schemes=myapp,other` or as
`LAUFEY_CUSTOM_SCHEMES=myapp,other` (comma-separated; `app` is implicit). The
browser process forwards the switch to its renderer and utility processes. On
CEF, registration timing does not matter for a declared scheme: its handler
factory is global, so every window serves it from the moment it is registered. A
scheme that is registered at runtime but was not declared is still served, but
as a non-standard scheme — opaque origin, insecure context — and the backend
logs a warning.

## Close-requested handler defers the close (API ≥ 31)

As of API 31, registering `set_close_requested_handler` changes the backend's
behavior: the window no longer closes on its own when the user clicks the close
button. Resolving is just a call to the existing `close_window` — no new handle
type, no synchronous return value. This isn't only a veto — it's a general hold
point for whatever needs to happen before the window actually goes away: flush
writes, close file handles, wait for a save to finish, ask the user, or just
always let it through once cleanup is done.

1. The runtime calls `set_close_requested_handler(handler, user_data)`.
2. When the user requests a close, the backend invokes `handler` and then does
   nothing further — the window stays open.
3. The runtime does whatever work it needs, then decides — synchronously inside
   `handler` or later from any thread: call `close_window(window_id)` to
   actually close it, or do nothing to leave it open. There's no separate
   "resolve" call and no opaque handle to release — `close_window` already
   bypasses each backend's close-confirm gate (`[NSWindow close]` not
   `performClose:`, `DestroyWindow` not `WM_CLOSE`, `gtk_widget_destroy` not
   `gtk_window_close`, CEF marks the window close-allowed so `CanClose` skips
   the negotiation before force-closing with `CloseBrowser(true)`), so it's safe
   to call from inside the handler without re-entering it.

With no handler registered, behavior is unchanged from backends predating API
31: the window closes immediately on click.

**The defer is process-wide, not per-window.** Once registered, the handler
receives — and holds open — _every_ window's close click, including windows the
embedder never meant to intercept. A handler that only cares about some windows
must call `close_window(window_id)` itself for the rest, or those windows become
unclosable. Higher-level per-window APIs (e.g. the Rust capi's
`Window::on_close_requested`) implement this exact pattern: their process-wide C
handler completes the close for any window without a registered per-window
handler.

**Scope: only the window's own close control.** `handler` fires for the window's
native close affordance — the title bar close button, `Alt+F4`, a window
manager's "close" action — and nothing else. It does **not** fire for `Cmd+Q`,
an app menu or tray "Quit" item, or any other app-level termination path, on any
backend. This is deliberate, not an oversight: a window that hides itself
instead of closing (e.g. a tray-icon app — see
[window-events.md](window-events.md)) needs an app-level quit that it _can't_
intercept, or "Quit" would just re-hide the window instead of exiting.
Concretely:

- macOS: CEF's `terminate:` override — which is what both the `"quit"` menu role
  and `Cmd+Q`'s default handling route through — marks every window
  close-allowed (so `CanClose` skips the close-requested negotiation) and
  force-closes with `CloseBrowser(true)`, which skips the beforeunload prompt
  (it does _not_ bypass `CanClose`/`DoClose`; the close-allowed marks are what
  skip the negotiation) — see `cef/src/main_mac.mm` and `CloseAllBrowsers` in
  `cef/src/app.cc`. WebView never wires `applicationShouldTerminate:` to
  `windowShouldClose:` at all, so it's already outside the handler's reach.
- Windows and Linux: the `"quit"` menu role has no automatic OS-level binding on
  either platform — clicking it just invokes the runtime's own registered
  menu-click callback (`menu_linux.cc`; unhandled on Windows, where `"quit"`
  isn't special-cased at all), so it never touches the close path unless the
  runtime's own callback explicitly calls `close_window`.

If a runtime wants "are you sure you want to quit?" behavior, it needs an
app-level quit hook — which doesn't exist yet in this ABI — not
`set_close_requested_handler`.

`handler` fires synchronously on the backend's native UI thread (e.g. AppKit's
`windowShouldClose:`) — the same thread every other event handler
(`set_resize_handler`, `set_mouse_click_handler`, etc.) already fires on. A
_synchronous_ confirm dialog can be shown directly in the handler (the backend's
`show_dialog` pumps OS events while blocking, so this doesn't deadlock).
Runtimes that want to resolve asynchronously instead need their own way back
into their async runtime from that thread; e.g. the Rust `laufey` crate's
`Window::on_close_requested` docs recommend capturing a `tokio::runtime::Handle`
ahead of time rather than relying on a bare `tokio::spawn`, which requires an
ambient runtime context this thread doesn't have.

## UI-thread tasks (API ≥ 42)

```c
typedef void (*laufey_ui_task_fn)(void* data, bool ran);
void (*dispatch_ui_task)(void* backend_data, laufey_ui_task_fn task, void* data);
bool (*is_ui_thread)(void* backend_data);
```

`post_ui_task` hands a task to the platform's queue (a GCD block, a window
message, a GLib idle source, a CEF task, a Winit user event) and forgets it.
Once the event loop has ended nothing drains that queue, so a runtime thread
waiting for such a task waits forever, and the backend's shutdown, which waits
for the runtime thread, waits with it. `dispatch_ui_task` adds the guarantee:
`task` is called **exactly once**, with `ran == true` on the UI thread (queued
behind the work already posted there, never inline), or with `ran == false` when
the loop has ended or ends before the task ran. The backend answers every
pending task that way on the thread that ends the loop, **before** it calls
`laufey_runtime_shutdown`, and answers later dispatches synchronously. A
headless worker (no loop) answers every dispatch with `ran == false`.

`is_ui_thread` is true on the thread that runs those tasks: the process main
thread on macOS (every backend) and for the WebView backends, CEF's `TID_UI`
(the main thread), the Winit event loop's thread. Both are callable from any
thread; the shared implementation is `backend-common/include/laufey_ui_tasks.h`
(`backend-winit-common/src/ui_tasks.rs` for Winit).

In the `laufey` crate: `is_ui_thread()`, `try_run_on_ui_thread(f)` (blocks;
inline on the UI thread; `Err(UiThreadError::Shutdown)` once the loop ended),
`spawn_on_ui_thread(f)` (a future; never blocks), and `run_on_ui_thread(f)`
(littledivy/laufey#79, which now hops through `dispatch_ui_task` on every
platform and panics instead of hanging when the loop has ended). A panic in `f`
is resumed on the caller. Blocking a thread the UI thread is itself waiting for
deadlocks: prefer `spawn_on_ui_thread` from an async runtime.

## Threading

All API calls must happen on the UI thread the backend's event loop runs on,
unless their documentation says otherwise. `post_ui_task` hops onto it from
another thread, and `dispatch_ui_task` (API ≥ 42) does so with an answer
guaranteed even when the app quits. `show_dialog` blocks on the UI thread but
pumps OS events so other windows stay responsive. The API 38 window calls, the
API 39 file dialogs, drag-out and every clipboard call may be made from any
thread; the backend hops itself, and a file dialog resolves through its callback
instead of blocking the caller.
