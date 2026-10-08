# End-to-end testing strategy

This document describes how laufey can automatically test its **native chrome
and windowing surface** — menus, tray icons, notifications, dialogs, clipboard,
window geometry/state, input events, and the JavaScript bridge — across **every
backend** (CEF, WebView, Winit) on **Linux, macOS, and Windows**.

It is a design/contributor document, not a user reference. It records the
verification techniques, the empirical findings that back them, a full coverage
matrix over the C ABI surface, and a phased rollout.

> Status: implemented for all three backends. `examples/native_e2e` (the Layer-0
> battery + menu/tray click round-trips via the `test_click_menu_item` hook) and
> `examples/native_e2e_driver` (the Linux D-Bus observer) run under **Winit,
> WebView, and CEF** via `scripts/native-e2e-run.sh`. The `native-e2e` CI job
> gates macOS for all three backends (both native-chrome codebases, end to end),
> plus winit/Windows and cef/Linux. Some backend × OS combos are **excluded** as
> known headless-CI backend limitations (follow-ups, not harness bugs):
> winit/Linux (the winit backend doesn't support Linux menus and panics building
> a muda menu with no GTK init), webview/Linux (WebKitGTK/Xlib isn't thread-safe
> under the worker-thread runtime; that leg runs only the custom-scheme
> request-body round trip, `native-e2e-run.sh webview --scheme-body`, and the
> storage e2e), and cef|webview/Windows (CEF dist extraction / WebView2 run
> flakiness). The `test_click_menu_item` hook (`§8`) is implemented for the
> Winit backend (Rust) and the C++ `backend-common` shared by CEF/WebView. The
> macOS self-accessibility approach (`§7.2`) is verified standalone but not yet
> embedded (it needs the backend's main thread — see `§8`). Implementing the
> hook exposed and fixed a pre-existing self-deadlock in the Winit menu-callback
> registration. The `test_trigger_close_requested` hook (API 31, `§8`) for
> `set_close_requested_handler`'s defer-until-close_window contract is
> implemented for all backends and verified green under Winit and WebView on
> macOS; it rides the same pre-existing CI exclusions above for CEF and the
> Windows/Linux webview combos, so those aren't gated by it yet. The
> `test_trigger_open_url` hook (API 35, `§8`) for deep-link delivery is
> implemented wherever deep links are — macOS, for all three backends — and
> verified green under Winit and WebView there; it reports `N/A` on every other
> platform, where the ABI pointer is NULL by design.

---

## 1. Goals and non-goals

**Goals**

- Automatically verify, in CI, that laufey's native surface actually works — not
  just that the C ABI accepts a call, but that the OS registered/rendered the
  widget and that user-driven callbacks round-trip back to app code.
- Cover **all backends**, because native chrome has two independent
  implementations (C++ `backend-common` for CEF/WebView; Rust
  `backend-winit-common` for Winit) and four web engines.
- Maximize the fraction of the surface that runs as a **blocking PR gate** on
  stock GitHub-hosted runners (no self-hosted infra, no manual permission
  setup).

**Non-goals**

- Pixel-perfect visual regression. Screenshot diffing is high-flake and
  low-diagnostic; it is not part of the gate. (A last-resort visual smoke on
  notifications is acceptable nightly.)
- Simulating real hardware user input where a cheaper deterministic path exists.
  We drive callbacks through the same dispatch code a real event would, not by
  injecting OS-level mouse/keyboard events, except where noted.

---

## 2. Why this is hard (and where it isn't)

The existing harness, `examples/cef_e2e`, exercises the CEF web bridge
(bindings, `execute_js`, navigation) but explicitly punts on everything that
lives **outside the webview**:

> Doesn't drive dialogs (alert/confirm/prompt) because those need either real OS
> input or a backend-side test stub — neither exists today.

Native chrome is OS-owned and has no return value to assert against — a menu or
a tray icon is "somewhere on the screen", owned by the window server, the shell,
or another process. That is the genuinely hard part.

The key insight of this document is that the difficulty is **wildly
asymmetric**, and that most of the surface is _not_ actually hard:

- A large fraction of the API has **direct readback** (`set_window_size` →
  `get_window_size`, `write_clipboard_text` → `read_clipboard_text`). This is
  the cheapest, most reliable category and needs none of the machinery below.
- The truly OS-owned chrome (tray/menu/notifications) turns out to be
  **introspectable without pixels** on every platform, via a different mechanism
  each: D-Bus on Linux, the Accessibility API on macOS, UI Automation on
  Windows.
- Only genuinely modal/outward-facing surfaces (dialogs, devtools, external
  browser launch) resist automation and are relegated to nightly.

---

## 3. Backend and surface landscape

### 3.1 Backends are not interchangeable

Runtimes are backend-agnostic cdylibs; a development backend loads one via
`--runtime <path>` or `LAUFEY_RUNTIME_PATH` (a packaged one only loads the
runtime next to its executable;
[Runtime library](launch-config.md#runtime-library)). That means **one test
runtime can be driven by every backend binary**. But the backends differ in what
they implement:

| Backend | Web engine                                 | Native-chrome impl                                    | OSes      |
| ------- | ------------------------------------------ | ----------------------------------------------------- | --------- |
| CEF     | Chromium (multiprocess)                    | C++ `backend-common`                                  | L/mac/win |
| WebView | WKWebView / WebView2 / WebKitGTK (per-OS!) | C++ `backend-common`                                  | L/mac/win |
| Winit   | **none**                                   | **Rust `backend-winit-common`** (`tray-icon`, `muda`) | L/mac/win |
| Servo   | Servo (branch)                             | —                                                     | deferred  |
| iOS     | WKWebView (iOS)                            | subset                                                | deferred  |

Consequences:

- There are **two native-chrome codebases**. Running the tray/menu/clipboard
  suite under _both_ a `backend-common` backend and Winit validates two separate
  implementations and catches divergence. This is the real payoff of "test all
  backends" — not redundancy.
- WebView is **three different web engines** by OS, so its web-layer tests
  genuinely differ per platform.
- Winit has **no web engine**: `navigate`, `execute_js`,
  `set_page_load_handler`, and `register_scheme_handler` are `None`. Winit
  leaves ~68 API fields unimplemented in total. Capability probing (`§6`) is
  therefore mandatory.

### 3.2 The full C ABI surface

Every function pointer in `laufey_backend_api_t` is a capability to cover. They
group as:

- **Windowing / state**: create, size, position, resizable, always-on-top,
  visibility (show/hide), opacity, window flags (frameless, transparent,
  transparent-titlebar, hidden, no-activate), handles.
- **Window & input events**: resize, move, focus, close-requested, mouse
  click/move, wheel, cursor enter/leave, keyboard.
- **Web bridge** (CEF/WebView only): navigate, execute_js, JS bindings /
  namespace / callbacks, page-load, custom scheme handlers, devtools.
- **Native chrome**: application menu, context menu, tray (icon/tooltip/menu/
  click/double-click/dark-icon/bounds), dock/taskbar (badge/bounce/menu/
  visibility/reopen), notifications, dialogs (alert/confirm/prompt/file).
- **System integration**: clipboard read/write, permissions (query/request).

---

## 4. Verification techniques

Ordered cheapest → hardest. Each capability maps to one (or a combination).

### A. Direct state readback — _cheapest, most reliable, all-platform gate_

Set a property, read it back from the real OS object via the backend's own
getter. No display-server introspection needed.

- Window: `set_window_size`/`get_window_size`,
  `set_window_position`/`get_window_position`, `set_resizable`/`is_resizable`,
  `set_always_on_top`/`is_always_on_top`, `show`/`hide`/`is_visible`,
  `set_window_opacity`/`get_window_opacity`, `get_window_scale_factor`.
- Clipboard: `write_clipboard_text` → `read_clipboard_text` (round-trips through
  the real OS clipboard).
- Handles: `get_window_handle` / `get_display_handle` / `get_window_handle_type`
  (assert non-null and correct type enum per platform).
- Permissions: `query_permission`.
- Tray geometry: `get_tray_icon_bounds`.

### B. Event injection → callback round-trip

Drive a callback by calling the corresponding setter and asserting the handler
fires with the right arguments. No OS input required.

- `set_resize_handler` ← `set_window_size`; `set_move_handler` ←
  `set_window_position`; `set_focused_handler` ← `focus`/`show`;
  `set_close_requested_handler` ← programmatic close.
- Menu / tray clicks via the platform's non-modal invoke primitive (`§5`).
- `set_page_load_handler` ← `navigate` (CEF/WebView).

### C. Custom scheme / IPC

Register a custom scheme _before the first window_, serve a page over it, and
assert the registered handler served it **as a real origin**. `native_e2e`
registers `laufey-e2e`, loads `laufey-e2e://app/`, and the page reports back
through a binding: `location.origin == "laufey-e2e://app"`, `isSecureContext`, a
working `crypto.subtle.digest`, `localStorage` set/get, a same-origin `fetch`
whose streamed body arrives intact, and a cross-origin `fetch` to a loopback
echo server that proves the `Origin: laufey-e2e://app` header. A second window
loads `app://e2e/` to prove the built-in scheme still works next to the
registered one. Two negative checks follow: a `fetch` to a scheme nobody
registered must fail without reaching the handler, and a scheme registered only
after the window exists must not be served in it (reported, not asserted, on
WebKitGTK, whose shared web context applies late registrations to existing
views). `N/A` on engine-less backends. The CEF host must be told the scheme up
front (`LAUFEY_CUSTOM_SCHEMES=laufey-e2e`, set by `scripts/native-e2e-run.sh`,
which also exports `LAUFEY_E2E_BACKEND`) because Chromium registers custom
schemes before the runtime loads.

Local Network Access (`lna_checks.rs`, also on its own with `--lna`) runs with
Chromium's checks on: the custom-scheme page's cross-origin fetch and its
WebSocket to the loopback echo server must get through (laufey grants local
network access to the embedder's declared schemes, see
[custom-schemes.md](custom-schemes.md)), on every engine. On CEF a page on any
other origin must still be refused at once: the script declares three loopback
ports public (`--ip-address-space-overrides=127.0.0.1:<port>=public,…`, the
ports in `LAUFEY_E2E_PUBLIC_PORT`, comma-separated), the battery serves a page
on the first one it can bind, and its fetch to the echo server must fail within
five seconds instead of waiting for a prompt. The ports come from 20000-32767,
below every OS's ephemeral port range, so a port the OS gave an outgoing
connection or a server bound to port 0 is never one of them.

`--network-quiet` (`network_quiet_checks.rs`) checks that the CEF host makes no
request of its own (see [Backends](backends.md#no-network-requests-of-its-own)):
the script runs the host with `--log-net-log`, the battery loads the
custom-scheme page (its fetch to the loopback echo server is the one request the
log must show), idles for `LAUFEY_E2E_QUIET_SECS` seconds (default 10; CI uses
75, past the component updater's first check at 60 s) and quits through `quit()`
so Chromium finishes the log. `scripts/netlog-hosts.py` then lists every host
the log shows a request, preconnect, DNS lookup or socket connect for, and the
run fails on any but loopback (and `wpad` on Windows, the system's proxy
auto-discovery). `LAUFEY_E2E_NETLOG_OUT` keeps a copy of the log; CI uploads it
when the step fails. `N/A` on the other backends.

The bridge (API 44) is held to the calling document. Every call of the
request-body page must carry its origin (`call.origin == "app://e2e-body"`); a
same-origin sub-frame that posts to the engine's message handler directly (an
object and a JSON string, with no, an empty and a guessed WebKitGTK token) must
never reach the binding; and the main frame posting malformed messages (wrong
types, NaN / negative / huge ids, a lone surrogate, a `Date` argument,
unparseable JSON) must neither crash the app nor stop the bridge.
`--bridge-origin` (`bridge_origin_checks.rs`) writes a `laufey-launch.json` next
to the backend pinning `"bridgeOrigins": ["app://e2e-bridge-ok"]`: a window on
that origin must get the bridge with its origin on every call, and a window on
`app://e2e-bridge-no` must get no `Laufey` namespace and see a call it posts to
the engine's channel itself refused. The script removes the file when the run
ends.

Request bodies travel the other way: a page at `app://e2e-body/` sends POST, PUT
and PATCH requests (UTF-8 text, binary bytes including NUL and 0x80–0xFF, a body
over 1 MB, an empty body, bodies of 256 KiB − 1, 256 KiB, 256 KiB + 1 and 512
KiB around the chunk WebKitGTK reads them in, and eight bodies in flight at
once) to the scheme handler, which reads each one with `read_body` and echoes
it. The battery checks that the handler received exactly the bytes sent and that
the page got an identical echo (`examples/native_e2e/src/body_echo.rs`). The
same page then checks that a response's `Content-Type` reaches the engine as its
MIME type and charset: a `text/plain; charset=iso-8859-1` response keeps that
header and its `XMLHttpRequest` text decodes as Latin-1 (a quoted charset too),
`application/json` keeps its type, and documents loaded in a frame are
`text/html` in UTF-8 or windows-1252 as declared, or `text/plain` shown as text.
WebKitGTK once sent no MIME type (a navigation became a download) and CEF once
passed `text/html; charset=utf-8` whole as the MIME type (an empty document).

Responses that never end are read incrementally: a page at `app://e2e-stream/`
reads a never-ending response with `fetch` (its first chunks, status and
headers, then `reader.cancel()`), aborts one with an `AbortController`, reads
Server-Sent Events with `EventSource` (a default and a named event, the event
id, then `close()`) and a never-ending `XMLHttpRequest` to `LOADING`, and reads
a 6 MiB binary body (over the WebView2 credit window) byte for byte. Each
never-ending route keeps writing heartbeats until a write fails, so the battery
also asserts that every cancellation reached the handler
(`examples/native_e2e/src/stream_checks.rs`; see
[Streaming responses](custom-schemes.md#streaming-responses)). Two scenarios
check that a write never blocks the writer (API 42): `slow` writes a 4 MiB body
from a single "event loop" thread while the UI thread is held busy for 1.5 s
(through `spawn_on_ui_thread`), and must finish writing in well under that, then
serve a `ping` request in the middle of the page's slow read of the body; `cap`
writes 80 MiB to a page that doesn't read and expects a write to fail once 64
MiB are held, and the page's `fetch` or read to reject (N/A on WKWebView, which
hands every write to WebKit). The old WebKitGTK pipe failed both: its writer
blocked for the whole 1.5 s and the 80 MiB went through.
`LAUFEY_E2E_ONLY=scheme-body` (`native-e2e-run.sh <backend> --scheme-body`) runs
only these two checks.

### D. OS-observer introspection — _for chrome with no getter_

The three platform mechanisms (`§7`): Linux D-Bus, macOS self-Accessibility,
Windows UI Automation. Covers tray, menu structure as the OS sees it,
notifications, window title, decorations, dock.

### E. Raw input events

Mouse/keyboard/wheel/cursor handlers. Best driven by a **backend test-inject
hook** that posts a synthetic event down the same path (deterministic), rather
than OS-level input injection (flaky). Part of the Layer-0 hook (`§8`).

### F. Modal / outward-facing — _nightly_

`show_dialog` (alert/confirm/prompt/file), `request_permission`,
`open_devtools`, `bounce_dock`, external-browser open. Modal dialogs block the
main thread (see the macOS modal trap in `§7.2`), so they need either a backend
auto-answer stub or an external AX/UIA driver on a separate thread.

---

## 5. Per-platform non-modal click primitives

For Layer-0 click round-trips we invoke an item through the same dispatch a real
click uses, **without** entering a modal tracking loop:

| Platform | Primitive                                    | Fires                                                     |
| -------- | -------------------------------------------- | --------------------------------------------------------- |
| macOS    | `[NSMenu performActionForItem:idx]`          | `LaufeyCommonMenuTarget menuItemClicked:` (`menu_mac.mm`) |
| Linux    | `gtk_menu_item_activate(item)`               | `OnGtkMenuItemActivate` (`menu_linux.cc`)                 |
| Windows  | post `WM_COMMAND` with the item's command id | `WM_COMMAND` handler (`tray_win.cc` / menu)               |

macOS note: `AXPress` on a status item _opens the menu modally on the main
thread_ and deadlocks in-process driving — do **not** use AX to invoke;
`performActionForItem` is the correct primitive (verified, `§7.2`).

---

## 6. Capability probing (mandatory)

Because backends implement different subsets, a test must distinguish
"unsupported here" (→ `N/A`) from "supported but broken" (→ `FAIL`). Probe via
documented signals:

- Tray: `create_tray_icon()` returns `0` when unsupported.
- Any capability whose backend fn pointer is `None`: the capi Rust wrapper
  returns `Option::None` / no-ops — the runtime treats that as `N/A`.
- Web capabilities are absent on Winit (`navigate`/`execute_js`/... are `None`)
  → web/JS/scheme/devtools assertions are skipped on Winit.

Each assertion is tagged with the capability it requires; the harness emits
`[e2e] PASS <name>`, `[e2e] FAIL <name>`, or `[e2e] N/A <name>` and exits
non-zero only on `FAIL`. This lets **one runtime binary** be valid across all
backends.

---

## 7. The observers

### 7.1 Linux — D-Bus watcher/observer (runs in CI on cef/linux)

On Linux, both native-chrome implementations expose the tray over the
freedesktop **StatusNotifierItem** spec and the menu over
**`com.canonical.dbusmenu`**:

- CEF/WebView: appindicator, dlopened at runtime — Ayatana or legacy
  (`backend-common/src/tray_linux.cc`).
- Winit: the `tray-icon` crate's internal StatusNotifier logic.

So a **single D-Bus driver validates both**. The driver _is_ the desktop shell:
it owns `org.kde.StatusNotifierWatcher` (with
`IsStatusNotifierHostRegistered = true`, without which libappindicator silently
falls back to legacy GtkStatusIcon/XEmbed and never touches D-Bus), owns a stub
`org.freedesktop.Notifications` to capture `Notify` payloads, spawns the backend

- runtime, and then introspects.

Run line (works for **any** backend binary):

```
xvfb-run -a dbus-run-session -- sni-driver <backend-bin> --runtime libnative_e2e.so
```

In CI: `scripts/native-e2e-run.sh cef --layer1` on every cef/linux leg (x64 and
arm64), with the Ayatana runtime library installed and
`LAUFEY_E2E_REQUIRE_TRAY=1`. The battery runs under the driver with
`LAUFEY_E2E_HOLD` set, waits for the driver's dbusmenu `Event` to reach the
tray's `on_click` (a FAIL if it doesn't), and the driver fails unless the
battery's own verdict is PASS.

Key facts baked into the driver:

- libayatana passes the item's **object path** to `RegisterStatusNotifierItem`;
  the bus name is the **message sender** (`#[zbus(header)] hdr → hdr.sender()`),
  not the argument. (KDE-style apps pass a bus name instead — handle both.)
- `com.canonical.dbusmenu.GetLayout(0, -1, [])` returns
  `(u32 revision, (i32 id, a{sv} props, av children))`; children are variants
  wrapping the recursive struct — walk manually.
- A menu click is `AboutToShow(id)` then
  `Event(id, "clicked", <variant "">, <timestamp u32>)`, which fires the app's
  `laufey_menu_click_fn`.
- Icon set via a `/tmp` PNG path surfaces as `IconThemePath`, **not**
  `IconPixmap`; Linux tray tooltip and left-click are no-ops — don't assert
  them.

The driver lives at `examples/native_e2e/driver` (Rust, `zbus` v5 with the
`tokio` feature). Skeleton of the watcher interface:

```rust
#[interface(name = "org.kde.StatusNotifierWatcher")]
impl Watcher {
    async fn register_status_notifier_item(
        &self, service: &str, #[zbus(header)] hdr: Header<'_>,
    ) {
        let sender = hdr.sender().map(|s| s.to_string()).unwrap_or_default();
        let (bus_name, path) = if service.starts_with('/') {
            (sender, service.to_string())            // ayatana / Winit tray-icon
        } else {
            (service.to_string(), "/StatusNotifierItem".to_string()) // KDE-style
        };
        self.tx.send((bus_name, path)).ok();
    }
    #[zbus(property)] async fn is_status_notifier_host_registered(&self) -> bool { true }
    #[zbus(property)] async fn registered_status_notifier_items(&self) -> Vec<String> { /* ... */ }
    #[zbus(property)] async fn protocol_version(&self) -> i32 { 0 }
}
```

### 7.2 macOS — self-Accessibility (empirically verified, no permission)

macOS has no protocol boundary; the chrome is live AppKit objects. The
Accessibility API reaches them, and — verified on macOS 15.5 with
`AXIsProcessTrusted() == false` (i.e. **no TCC permission granted**) — a process
can read its **own** tree:

```
AXUIElementCreateApplication(getpid())
  AXMenuBar        -> AXError 0, full app menu
  AXExtrasMenuBar  -> AXError 0, the app's own NSStatusItem + its menu items
```

So macOS **menu and tray structure are verifiable on hosted CI with zero
permission setup** — no XCUITest, no self-hosted runner, no `TCC.db` surgery
(which is SIP-protected and unavailable on hosted runners anyway). The click
half uses `NSMenu.performActionForItem(at:)` in-process (verified to fire the
target-action, no permission, no modal loop).

This runs **in-process inside the test runtime** (a small AX verifier invoked
after the runtime builds its menus), so it is backend- and engine-agnostic.

Limitation: true _external_ user-input simulation still needs TCC/XCUITest, but
structure + callback wiring — which is what we care about — does not.

### 7.3 Windows — UI Automation

UIA has no permission gate; any process can inspect any UI on the session.

- Menus: `ControlType.Menu`/`MenuItem` via **FlaUI** (.NET/UIA3 — the maintained
  choice; WinAppDriver has had no release since 2020). Enumerate and `Invoke`.
- Tray: icons live in Explorer (`Shell_TrayWnd` → notification-area toolbar +
  `NotifyIconOverflowWindow`). Enumerate by name (= tooltip). Win11 moved most
  icons into the overflow flyout and changed the shell toolbar model, so
  enumeration is brittle → prefer the Layer-0 callback per-PR and treat tray
  _scraping_ as nightly.
- Toasts: UIA over the toast / Action Center — nightly.

---

## 8. Layer 0 — the in-process test hook

A small, test-only extension to `laufey_backend_api_t` that proves laufey's own
plumbing (template parse → native build → callback dispatch → id routing) on
**all** backends cheaply and deterministically — essentially what Electron's own
spec suite does. Appending to the end of the struct is ABI-safe because every
backend `memset`s its api table (unimplemented hooks stay NULL → the capi
wrapper returns `false`/`None` → the runtime reports `N/A`); the API version is
bumped alongside (29 → 30).

**Implemented (API 30):**

```c
// Synthesizes a click on the menu/tray item with id `item_id` by invoking the
// same on_click dispatch a real click uses (looks the handler up by id in the
// backend's shared click store and calls it). Returns true if an item with
// that id was registered and its handler ran. Runs on the caller's thread — no
// main-thread UI access needed — so it works from the worker-thread runtime.
bool (*test_click_menu_item)(void* backend_data, const char* item_id);
```

Implemented for **both** native-chrome codebases, so every backend has it:

- **Winit** (`backend-winit-common`): `dispatch_menu_click_by_id` reuses the
  exact path `poll_menu_events` uses for a real muda `MenuEvent`.
- **CEF + WebView** (C++ `backend-common`): a shared click registry
  (`test_hooks.cc`: `RegisterMenuClick` / `TestClickMenuItem`) that the menu
  builders (`menu_mac.mm`, `menu_linux.cc`, `tray_win.cc`) populate; each
  backend's api table points `test_click_menu_item` at it.

The capi exposes `laufey::test_click_menu_item(item_id)`. All three backends run
the same runtime green (both app-menu and tray-menu click round-trips PASS).
Doing this surfaced and fixed a pre-existing self-deadlock in Winit:
`register_menu_callbacks` held the click-store mutex while recursing into
submenus (std `Mutex` is not reentrant), freezing the main thread on any menu
containing a submenu.

**Implemented (API 31):**

```c
// Synthesizes a close-requested event on window_id through the same
// dispatch code a real OS close click runs. Returns true if a registered
// set_close_requested_handler deferred the close (the window is still
// open), false if the close proceeded (no handler was registered).
// "Proceeded" means initiated: winit and CEF queue the actual close, so
// poll window state instead of asserting right after a false return. The
// handler runs synchronously on the calling thread, not the backend UI
// thread a real close click would use.
bool (*test_trigger_close_requested)(void* backend_data, uint32_t window_id);
```

Implemented for both native-chrome codebases, reusing each backend's existing
close dispatch rather than a new registry (unlike the click hook, there's
nothing to "look up by id" — a window has at most one pending close):

- **Winit** (`backend-winit-common`): dispatches via the same
  `dispatch_close_requested_event`, then proceeds through `backend_close_window`
  — the real close entry point.
- **CEF + WebView** (each backend's `RuntimeLoader`): dispatches via
  `DispatchCloseRequestedEvent`, then proceeds through `Backend_CloseWindow`.

The capi exposes `laufey::test_trigger_close_requested(window_id)`. The
`native_e2e` runtime registers an `on_close_requested` handler that _stashes_
the window_id instead of resolving inline, calls the hook to confirm the window
stays open, then closes it from outside the handler and confirms it actually
closes — proving resolution genuinely works from outside the handler (not just
that a bool threads through) round-trips. Verified green under Winit and WebView
on macOS; CEF and the Windows/Linux webview backends implement the same plumbing
but ride the pre-existing `native-e2e` CI exclusions for those combos (`§`
status note above) — not gated by this change, but not covered by CI for it
either.

**Implemented (API 35):**

```c
// Synthesizes a deep-link delivery of `url` through the same dispatch path
// a real OS-routed URL takes, buffer included: called before any handler is
// registered, the URL is replayed on registration exactly like a cold-start
// launch link. Returns true if a handler consumed it, false if it was
// buffered. NULL on every non-macOS backend (see docs/deep-links.md).
bool (*test_trigger_open_url)(void* backend_data, const char* url);
```

Deep links can't be exercised for real without registering a URL scheme with the
OS and driving it from outside the process — exactly the kind of setup this
strategy avoids. The hook routes through `FireOpenUrlMac` (CEF + WebView) /
`open_url::fire` (Winit), the same functions each backend's
`application:openURLs:` delegate method calls, so the buffer-and-flush behavior
under test is the shipping one.

The capi exposes `laufey::test_trigger_open_url(url)`. The `native_e2e` runtime
triggers one URL _before_ registering a handler — the position every cold-start
launch URL is in — then registers, and asserts the buffered URL was replayed,
that a subsequent URL is delivered live, and that neither was duplicated or
lost. Verified green under WebView and Winit on macOS; `N/A` everywhere else,
where the pointer is NULL by design rather than by omission.

**Implemented (API 38):**

```c
// Post a synthetic input event through the same dispatch a real OS event
// uses. Wheel deltas are DOM-signed (positive Y is scroll down).
bool (*test_inject_input)(void* backend_data, uint32_t window_id,
                          const laufey_test_input_t* event);
```

Winit runs the `WindowEvent` path (`modifier_key_edges`, `next_click_count`,
`winit_scroll_to_dom`, enter-waits-for-move). CEF / WebView call `Dispatch*`
with already-DOM values (click_count 1). The capi exposes
`laufey::test_inject_input`. `native_e2e` capability-probes the hook.

**Not yet added** (future hooks, same append-and-`N/A` pattern):

```c
// Serialize the menu the backend ACTUALLY built, for template->native readback.
laufey_value_t* (*test_dump_menu)(void* backend_data, int surface, uint32_t id);
```

Note the contrast with macOS self-AX (`§7.2`): reading the OS's view of a widget
needs the backend's **main thread**, which the worker-thread runtime can't reach
(a `dispatch_sync` to the main queue deadlocks against the backend event loop) —
so structure checks must live behind a backend hook too, whereas the click hook
above only touches an in-process mutex and works from any thread.

---

## 9. Full coverage matrix

Rows are capability groups; each cell is per-backend. Every ✅ is additionally
per-OS (`Linux/macOS/Windows`); WebView's web-layer cells differ by engine.

| Capability                             | Technique   | CEF                         | WebView                 | Winit            | Gate                         |
| -------------------------------------- | ----------- | --------------------------- | ----------------------- | ---------------- | ---------------------------- |
| Window geometry/state/opacity readback | A           | ✅                          | ✅                      | ✅ (Rust)        | ✅                           |
| Window lifecycle events                | B           | ✅                          | ✅                      | ✅               | ✅                           |
| Close handler (defer-until-close)      | B (`§8`)    | ✅ (Linux/Win nightly-only) | ✅ (Linux nightly-only) | ✅               | ✅ (macOS; see `§8`)         |
| Clipboard round-trip                   | A           | ✅                          | ✅                      | ✅               | ✅                           |
| Window handles / types                 | A           | ✅                          | ✅                      | ✅               | ✅                           |
| Application / context menu             | D + B       | ✅                          | ✅                      | ✅ (`muda`)      | ✅                           |
| Tray icon / menu / click               | D + B       | ✅                          | ✅                      | ✅ (`tray-icon`) | ✅ (win tray nightly)        |
| Notifications payload                  | D           | ✅                          | ✅                      | probe            | Linux ✅, else nightly       |
| Dock / taskbar                         | A/D/F       | ✅                          | ✅                      | probe            | partial                      |
| Raw mouse/keyboard/wheel events        | E           | ✅                          | ✅                      | ✅               | ✅ (with hook)               |
| Web: bindings/execute_js/navigate/load | B/C/E       | ✅                          | ✅                      | **N/A**          | ✅ (CEF/WebView)             |
| Custom scheme handlers                 | C           | ✅                          | ✅                      | **N/A**          | ✅ (CEF/WebView)             |
| DevTools open / close / toggle / off   | A (`§18`)   | ✅                          | ✅                      | N/A              | ✅ (`--system`)              |
| Global shortcuts (incl. conflict)      | A/B (`§18`) | ✅                          | ✅                      | N/A              | ✅ (`--system`)              |
| Launch at login                        | A (`§18`)   | ✅                          | ✅                      | N/A              | ✅ (`--system`, CI)          |
| Menu accelerators / context-menu close | B (`§19`)   | ✅                          | ✅                      | N/A              | ✅ (`--menus-notifications`) |
| Notification responses / scheduling    | A/B (`§19`) | ✅                          | ✅                      | N/A              | ✅ (`--menus-notifications`) |
| Dialogs (alert/confirm/prompt/file)    | F           | ⚠️                          | ⚠️                      | ⚠️               | nightly                      |

Net: ~90% of the C ABI is a hosted-CI PR gate across all backends; only modal /
outward-facing surfaces are nightly.

---

## 10. CI architecture

`.github/workflows/ci.yml` runs on every pull request to `main`, every push to
`main`, `denext/integration` or a `v*` tag, nightly (so flakes and runner-image
drift show up between merges) and on demand:

- **`lint`** — `cargo fmt --check`, `cargo clippy --workspace -D warnings`,
  `clang-format` 22.1.5 over `capi`, `cef/src` and `webview/src`,
  `deno fmt --check`, `deno lint`.
- **`test`** — the `laufey` crate's unit and doc tests (`cargo test -p laufey`),
  and backend-common's plain C++ tests (data directory, launch config, single
  instance).
- **`test-winit-common`** (Linux, macOS, Windows) — the Winit backend crate's
  unit tests, among them the check that its hand-written API table has the exact
  layout of `laufey_backend_api_t`.
- **`tsan`** — the synchronous UI-hop rendezvous (`laufey_sync_call.h`), the
  passkey ceremony and the single-instance lock under ThreadSanitizer.
- **`build-winit` / `build-webview` / `build-cef`** — every backend for every
  release target (`build-webview` also runs backend-common's `ctest` suite);
  their packages are the release artifacts.
- **`native-e2e`** — the `native_e2e` battery (`§8`) under every backend:
  `{winit, webview, cef}` on macOS 14 and Windows, `cef` and `webview` on Ubuntu
  22.04, `webview` on Windows on Arm and `cef` on Linux arm64. Each step runs
  `scripts/native-e2e-run.sh <backend> [mode]`: Layer 0, the D-Bus observer
  (`--layer1`, cef/linux), and the mode batteries of `§14`–`§20`, with storage,
  single-instance and (Windows) the toast cold start on their own. Linux runs
  headless under Xvfb with a private session bus; the window-API, HiDPI, I/O,
  system and menu modes add a window manager (openbox) and real X input
  (xdotool). The Layer-0 battery itself is excluded on webview/linux (WebKitGTK
  under the worker-thread runtime), where the request-body and Local Network
  Access checks run on their own instead.
- **`e2e-cef-linux`** — the older `cef_e2e` binding harness under Xvfb.
- **`release`** — on a `v*` tag, after **all** of the jobs above pass: checks
  the tag against the `laufey` crate version, publishes the crate and creates
  the GitHub release with the build artifacts and `SHA256SUMS`.

The nightly run is the same workflow, not a wider one: what the hosted runners
cannot drive (modal dialogs, outward-facing notification UI, the macOS / Windows
Layer-1 observers of `§7.2`–`§7.3`) is not covered in CI.

When a run goes wrong in CI it leaves evidence: `native-e2e-run.sh` streams the
backend's output (so a hang shows how far the battery got) and prints its exit
status, and a watchdog (`LAUFEY_E2E_WATCHDOG_SECS`, default 300) prints every
thread's stack (macOS `sample`, Linux `gdb` when installed) of a run that hasn't
exited and kills it. Every e2e step has its own `timeout-minutes` besides. On
macOS the `Crash reports` step waits for ReportCrash and prints the exception
and the crashed thread of each report (`scripts/print-crash-report.py`).

---

## 11. Status

Landed: the capability-probing Layer-0 battery (`native_e2e`), the
backend-launch matrix above, the Linux Layer-1 D-Bus observer
(`native_e2e_driver`, run by `--layer1`), and CEF / WebView build jobs. Not
landed: the macOS self-Accessibility and Windows UI Automation observers
(`§7.2`, `§7.3`) — macOS and Windows chrome is checked in-process only — and a
job for dialogs and other modal / outward-facing surfaces (the nightly run
repeats the PR checks).

---

## 12. Open questions / spikes

- Whether the CI libayatana version registers the item at `/StatusNotifierItem`
  vs `/org/ayatana/NotificationItem/*` — the sender-based capture handles both,
  but assertions on the path should not hard-code it.
- laufey's real macOS `NSStatusItem` uses a button _image_, not a title, so its
  AX bar item may have no `AXTitle` — assert on the _menu items_ (which do have
  titles) rather than the bar item.
- Winit notification / dock capability coverage (probe at runtime; several
  fields are `None`).
- Dialog auto-answer: backend stub vs external AX/UIA driver on a side thread.

---

## 13. Verified artifacts

- Linux D-Bus driver: written, `cargo check` passes against `zbus` 5.16
  (`§7.1`).
- macOS self-AX read of `AXMenuBar` / `AXExtrasMenuBar` and
  `NSMenu.performActionForItem(at:)`: empirically confirmed on macOS 15.5 with
  no accessibility permission granted (`§7.2`).

---

## 14. Per-app web storage (`storage_e2e`)

[App data & web storage](app-data.md) needs several launches of the backend, so
it has its own runtime, `examples/storage_e2e`, and driver,
`scripts/storage-e2e-run.sh <webview|cef>`. Each launch serves a page on a fixed
loopback port (one origin across launches), writes or reads `localStorage` plus
a persistent cookie, and quits through the normal path so the engine flushes its
profile. The driver asserts:

- persistence: a value written under one `LAUFEY_APP_ID` (and under one
  `LAUFEY_DATA_DIR`) is read back by the next launch;
- isolation: another app id on the same origin doesn't see it, nor does an
  unconfigured launch;
- unchanged defaults: an unconfigured CEF launch still gets a throwaway profile;
- on-disk layout: `<dir>/CEF`, `<dir>/WebView2` or `<dir>/WebKitGTK` (with
  `data/cookies.sqlite`), created `0700` on Unix; no directory for WKWebView;
- CEF only: a second instance of a running app exits during startup and the
  first is unaffected;
- [launch file](launch-config.md): with only a `laufey-launch.json` next to the
  executable (no `LAUFEY_*` in the environment), the file's `appId` gives a
  persistent per-app store and profile directory. On CEF the page is served over
  a custom scheme that only the file's `customSchemes` declares, and it must be
  a secure origin. The file's `appId` wins over `LAUFEY_APP_ID` in the
  environment, a `LAUFEY_DATA_DIR` in the environment is reported and ignored
  (the pinned app id keeps its store), and a malformed file is reported and
  ignored. While the file is in place the driver copies the runtime next to the
  backend executable, since a packaged app loads only that one.

The `native-e2e` CI job runs it after Layer 0 on every webview/cef leg, and on
the webview/Linux leg, where the Layer-0 battery is excluded (see the status
note above) and only the request-body round trip runs before it.

How an app ends has its own driver over the same runtime,
`scripts/exit-e2e-run.sh <webview|cef> [iterations] [paths...]`
(`LAUFEY_E2E_STORAGE_EXIT`): each launch writes a fresh value to `localStorage`
and a cookie and ends at once, with no pause for the engine, by one path —
`quit` (`quit()`), `close` (the last window closes) or `exit` (`exit_app` with
code 7, the runtime's thread then blocked for good, as `Deno.exit()` does). The
driver asserts that the process ends within `LAUFEY_E2E_EXIT_TIMEOUT` seconds
(60), with that path's exit code, and that the next launch of the same app reads
the value back. It stops at the first hang: on Windows such a process may not be
killable and keeps its profile locked. `process-exit`, run only when named, ends
with `std::process::exit` from the runtime's thread under a running engine (how
a runtime ended before API 46) and reports whether the value survived. See
[backends.md](backends.md#how-an-app-ends). The `native-e2e` CI job runs it
after the storage step on every webview/cef leg.

---

## 15. Single instance and opened files (`single_instance_e2e`)

The [single-instance lock](deep-links.md#single-instance) and the arguments a
runtime sees need real processes, so they have their own runtime,
`examples/single_instance_e2e`, and driver,
`scripts/single-instance-e2e-run.sh <webview|cef>`. The driver asserts:

- cold start: the runtime reads the backend's command-line arguments (a URL
  among them) with `std::env::args()`;
- with `"singleInstance": true` in the launch file, a second launch from another
  working directory (with arguments containing spaces, a URL and non-ASCII text)
  exits 0 within seconds without loading the runtime (on Linux it runs with no
  display at all), and the first instance receives `second_instance` with
  exactly those arguments and that directory;
- with the app id pinned by the file, a second launch with
  `LAUFEY_SINGLE_INSTANCE=0` and its own `LAUFEY_DATA_DIR` in its environment
  still forwards (the variable is reported as ignored); without an app id in the
  file, `LAUFEY_SINGLE_INSTANCE=0` overrides its `singleInstance`, and
  single-instance mode without an app id warns and runs unlocked;
- without single-instance mode two instances run side by side (on CEF with
  different data directories; one CEF profile admits one process, which
  `storage-e2e-run.sh` covers); the first one stays up until the driver releases
  it through `LAUFEY_E2E_SI_HOLD_FILE`, however long the second takes to start;
- macOS: files opened with the bundle through LaunchServices (`open -a`) reach
  `on_open_url` as `file://` URLs, both when they start the app and while it
  runs, and a file on the command line of a directly exec'd binary reaches
  `argv` only;
- Windows and Linux: a test URL scheme registered with the OS as an installer
  would (`HKCU\Software\Classes\laufey-si-test` with a `shell\open\command` of
  `"<backend>" -- "%1"`, the runtime copied next to the backend since the launch
  file makes it a packaged app, which loads no other; a `.desktop` file with
  `x-scheme-handler/laufey-si-test` made the default with `xdg-mime`, in a
  private XDG home), and links opened through the OS (`Start-Process`,
  `xdg-open`): at a cold start the runtime sees the URL in its arguments, and
  while an instance runs the OS-started second launch forwards it to
  `second_instance`. (On Windows and Linux there is no `on_open_url`: the OS
  starts a process, so `test_trigger_open_url` stays macOS-only.)

The buffered `open_url` round trip (`test_trigger_open_url`) stays in Layer 0
(`native-e2e-run.sh`). The lock's framing, limits and naming are unit-tested in
`backend-common/tests/single_instance_test.cc` (ctest). The Winit backend has no
single-instance lock, so the driver doesn't run there.

## 16. Window state, constraints, screens and lifetime (API 38)

`native_e2e`'s `window_api_checks` (part of the Layer-0 battery, or alone with
`scripts/native-e2e-run.sh <backend> --window-api`) is capability-probed against
`window_capabilities()`: a backend that reports a capability must honour it, and
one that doesn't must refuse (setters return `false`).

- **Screens**: non-empty, exactly one primary listed first, nonzero distinct
  JS-safe ids, positive bounds, work area inside the bounds, sane scale, and the
  window's screen is one of them.
- **Size constraints**: round trip, `set_size` clamped up to the minimum and
  down to the maximum, a tighter maximum resizes the window into it, a maximum
  below the minimum is raised, constraints clear.
- **State**: maximize → `is_maximized` → event (previous not maximized) → normal
  bounds equal the pre-maximize bounds → unmaximize → event; minimize / restore;
  fullscreen enter / leave; events carry the window id and never repeat their
  previous state. Where nothing applied the change (Linux without a window
  manager) the check is N/A, never a pass, and it still fails if a wrong state
  is reported.
- **Title bar / backdrops**: each setter succeeds exactly when the capability is
  reported; a hidden title bar puts the content origin at the frame origin and
  the default one moves it back.
- **A window the user closes** (`close_checks.rs`): a second window is closed
  through the window system, not `close_window` — Alt+F4 through the window
  manager on X11 (N/A without one), `WM_CLOSE` posted to it on Windows,
  `-[NSWindow performClose:]` on macOS — with a JS call from its page still
  pending. It must then read as closed (size 0x0, position (0, 0)), and the
  getters, setters, `execute_js`, `navigate` and the answer to the pending call
  that name it must all be no-ops: the backend's own destroy path drops the
  window's state, so none of them reaches the destroyed native window.

`--lifetime` (its own run, since it ends the process): with keep-alive on, the
last window closes and the loop survives (no runtime shutdown), a new window
opens, then `quit()` with a window open, called at once from the UI thread and
from a runtime thread, must end the loop, observed as the backend calling the
runtime's shutdown, exactly once. The battery then returns from
`laufey_runtime_start` instead of exiting, so the backend has to end the process
itself; on Unix an `atexit` guard fails the run (exit 1) unless the runtime's
start function had returned and its shutdown was called once by then.

The Layer-0 battery runs it on Linux without a window manager, where nothing
applies maximize / minimize / fullscreen (N/A). CI also runs `--window-api` on
its own on every leg, and on Linux under a window manager (openbox, started by
`native-e2e-run.sh` inside the Xvfb session; `LAUFEY_E2E_WM` picks another,
`none` turns it off): there the battery sets `LAUFEY_E2E_WM_RUNNING` and the
state changes must apply, so an N/A becomes a FAIL.

`--hidpi` runs the same battery at a device scale factor of 2: CEF on Linux with
`--force-device-scale-factor=2`, WebKitGTK with `GDK_SCALE=2` (on a 2560x1600
Xvfb screen), and on Windows at the display's own scale, which
`scripts/windows-display-scale.ps1` raises first (the largest display mode, then
the per-user scale the Settings app sets; `LAUFEY_E2E_EXPECT_SCALE` is what it
reached). It checks the window's scale factor, the primary screen's, the page's
`devicePixelRatio`, that `get_size` / `get_position` stay in DIPs and the page
is that many CSS pixels, and that the window system (xdotool on X11,
`GetClientRect` / `ClientToScreen` on Windows) has the content at `scale` times
its DIP size and origin. macOS runners have one 1x display, so it doesn't run
there.

## 17. Drag and drop, file dialogs and the clipboard (API 39)

`scripts/native-e2e-run.sh <backend> --io` runs `native_e2e`'s `io_checks`
alone, on every backend (webview/linux included: every call it makes hops to the
GTK thread). Capability-probed like the rest:

- **Clipboard**: text from a worker thread, HTML with its plain-text
  alternative, a PNG byte-for-byte, a non-PNG write refused, the formats list
  after each write, and a change event after a write (the macOS change-count
  poll, Windows' clipboard listener, GTK's owner-change), none once the handler
  is cleared.
- **File drops**: ENTER, OVER, LEAVE, DROP and an empty DROP through
  `test_trigger_file_drop`, the dispatch the OS path uses, checked for window
  id, position and paths; no delivery after the handler is cleared.
- **File dialog aborts**: dialogs of every kind (open with filters and
  multi-select, save, folder; modal and app-level) cancelled through
  `cancel_file_dialog` at delays from "at once" to two seconds after they open
  (`LAUFEY_E2E_ABORT_ROUNDS` rounds, 12 by default). Each one settles cancelled
  exactly once, its slot is free right after, and on Windows its window is gone.
- **File dialogs**: real OS dialogs. One closed by `test_file_dialog_respond`
  (cancel), one by `cancel_file_dialog`, a second refused as busy while one is
  open, and three accepted with a path the hook types in (a save target, an
  existing file, a directory), each result coming back through the dialog's own
  completion path. A dialog that won't take a typed path is N/A, not a pass: on
  the macOS CI runners the panels run out of process (`ok:` is not implemented
  and a posted Return doesn't reach them), so there the accepts are N/A and only
  opening, cancelling and busy are checked.
- **A real drop (X11)**: `laufey_xdnd_source` (a small GTK program built with
  the backend's tests) offers a file as `text/uri-list` over XDND, as a file
  manager does; xdotool presses on it, drags into the window and releases. The
  backend's own XDND handling must report ENTER, OVER at the pointer, and a DROP
  at the release point with the file's path. Needs `LAUFEY_E2E_XDOTOOL` and
  `LAUFEY_E2E_XDND_SOURCE`, which `native-e2e-run.sh --io` sets on Linux. CEF
  calls `CefDragHandler::OnDragEnter` only for Alloy-style browsers, and
  laufey's are Chrome style, so on X11 the CEF backend reads the paths from the
  XDND source itself (`cef/src/drag_paths_linux.cc`, the source's
  `text/uri-list` on `XdndSelection`) when the page first reports a drag with
  files. The CEF runs pass `--password-store=basic`: otherwise Chromium asks the
  session bus's keyring to unlock, and gnome-keyring's `gcr-prompter` grabs the
  pointer and keyboard for the rest of the run, which swallows every xdotool
  event.
- **A real drop (Windows)**: `laufey_ole_drag_source` (built with the backend's
  tests) drags a file out of its window with `DoDragDrop` and a `CF_HDROP` data
  object, as Explorer does; SendInput presses on it, moves into the laufey
  window and releases. ENTER, OVER at the pointer and a DROP with the file's
  path must reach `on_file_drop`, on WebView2 and on CEF (whose drop target
  laufey wraps to read the paths). `native-e2e-run.sh --io` sets
  `LAUFEY_E2E_OLE_SOURCE`. The source is started without a console window, which
  would otherwise open on top of the drop target.
- **macOS**: a real drag needs posting pointer events, which the CI runners'
  processes may not (no Accessibility grant), so the CEF backend's drag
  pasteboard reader is unit-tested instead
  (`backend-common/tests/io_mac_test.mm`).
- **Drag out**: a relative path and a drag with no mouse button held both fail.
  A real drag out needs a person or OS-level input injection on the drag source
  side and is not part of the battery.

The portable pieces (the drop dispatch, the dialog slot's exactly-once
completion, option copying, `file://` parsing, CF_HTML) are unit-tested in
`backend-common/tests/io_test.cc` (ctest), the capi wrappers in
`capi/src/io.rs`, and the Winit drop batching in
`backend-winit-common/src/file_drop.rs`.

## 18. Global shortcuts, launch at login and DevTools (API 40)

`scripts/native-e2e-run.sh <backend> --system` runs `native_e2e`'s
`system_checks` alone, on every backend (Winit reports each part N/A and checks
that it says so). `--devtools-off` runs the DevTools part again under
`LAUFEY_INSPECTABLE=0`.

- **Global shortcuts** (see [global-shortcuts.md](global-shortcuts.md)): a real
  OS registration answered with the canonical form, listed, another spelling
  refused as `ALREADY_REGISTERED`, `INVALID` for a modifier-less printable key,
  a navigation key alone or with Shift (`Escape`, `Shift+Up`) and an unknown
  key, a press through `test_trigger_shortcut` reaching the handler with the
  canonical form, and a real key press: on Windows injected with `SendInput` and
  arriving as `WM_HOTKEY`, on X11 injected with xdotool (XTEST) and matched
  against the shortcut's key grab. Then the **conflict**: the battery starts a
  second copy of the backend (`LAUFEY_E2E_ONLY=shortcut-holder`, its own data
  directory) that registers a shortcut and reports through a file; this process
  must get `CONFLICT` for the same shortcut from the OS (Carbon's exclusive hot
  keys, `RegisterHotKey`, an X11 grab), and `OK` once the holder has released it
  and exited. Unregister, the list, the test hook after unregistering, and
  re-registering finish it. A Wayland session with the portal is N/A (nothing
  can answer the portal's dialog); the portal client is covered by
  `shortcuts_portal_test` (ctest) against a mock portal.
- **Launch at login** (see [launch-at-login.md](launch-at-login.md)): on, read
  back, the OS artefact checked (the `Run` value naming this executable on
  Windows, the autostart file with this executable's `Exec` on Linux; macOS
  keeps `SMAppService` state in its own database), off, read back, artefact
  gone, original state restored. Only with `CI` set or
  `LAUFEY_E2E_LOGIN_ITEM=1`; the run script names the entry
  `dev.laufey.e2e.system` (`LAUFEY_APP_ID`).
- **DevTools** (see [devtools.md](devtools.md)): the launch setting and the
  engine's own setting read back, then open, close and two toggles, each waited
  for through `is_devtools_open`. Under `--devtools-off`: both settings off,
  `open_devtools` and `toggle_devtools` open nothing, and on CEF `print_to_pdf`
  (which drives the DevTools protocol) still returns a PDF.

The portable pieces are unit-tested in `backend-common/tests/system_test.cc`
(parsing, the registry over a fake OS side, the autostart format) and the capi
wrappers in `capi/src/system.rs`.

## 19. Menus and notifications (API 41)

`scripts/native-e2e-run.sh <backend> --menus-notifications` runs `native_e2e`'s
`menu_notification_checks` alone, on every backend (Winit reports N/A and checks
that it says so). See [menus.md](menus.md) and
[notifications.md](notifications.md).

- **Accelerators**: an app menu with an accelerated item and a disabled one;
  `test_trigger_menu_accelerator` fires the first through the backend's own
  dispatch (`-[NSMenu performKeyEquivalent:]`, `TranslateAccelerator` with the
  modifiers in the thread's keyboard state, `gtk_accel_groups_activate`, the CEF
  window accelerator) and not the disabled one or an unbound combination. A real
  `Ctrl+Shift+F9` fires it too: on Windows injected with `SendInput` while the
  window has the focus (N/A when the window can't take the focus), on X11 with
  xdotool after the window manager has activated the window.
- **Context-menu close**: an empty menu reports its close at once; a real menu
  is dismissed with `test_dismiss_context_menu` and its close callback fires
  exactly once, with no click (a menu the OS refused to show closes by itself,
  which the check accepts).
- **The app runs while a context menu is open**: on a window whose page calls a
  binding every 100 ms, a menu is opened (on Windows its `#32768` popup must be
  visible); while it is open the page's timer keeps reaching the runtime (at
  least 3 calls in 1.5 s), and a synchronous window getter and
  `run_on_ui_thread` return within 5 s. Every leg. (CEF on Windows used to stop
  running its tasks inside `TrackPopupMenu`'s modal loop; WKWebView showed the
  menu from a main-queue block, which held the main queue.)
- **Windows: a context menu driven from the keyboard**: before each menu the
  window is brought to the front with an Alt press around `SetForegroundWindow`
  (what an automation script does, which leaves the system menu in keyboard menu
  mode); then Escape dismisses one menu and Down + Return chooses the first item
  of the next, injected with `SendInput`. N/A where the window can't take the
  foreground.
- **Windows: the app runs while a tray menu is open** (WebView2 / CEF, their
  shared `tray_win.cc`): the tray menu is opened by posting the right-click to
  the tray's hidden top-level window as `Shell_NotifyIcon` delivers it, and its
  `#32768` popup must be visible; while it is open the same three checks as for
  the context menu hold; `WM_CANCELMODE` to the tray window closes it with no
  click.
- **Responses**: a click before any response handler is buffered, then delivered
  with `launch: true` when the handler registers; later ones arrive directly
  with `launch: false`.
- **Live notifications** (where one can be posted: on macOS after the quiet,
  "provisional" authorization macOS grants without a prompt): action and body
  clicks through `test_notification_respond` reach the live callback, `close`
  reports `Closed`. On Linux the step runs a stand-in notification server
  (`laufey_mock_notification_server`, built with the backend) on the run's
  private session bus, which answers a body containing `[[invoke:reply]]` with a
  real `ActionInvoked`.
- **Scheduling**: a notification two minutes out is listed with its time, title,
  data and actions, cancelled, and gone; one two seconds out leaves the list
  once delivered.
- **Windows activation**: the COM activator the app registered answers a
  `CoCreateInstance` + `INotificationActivationCallback::Activate` (what Windows
  does for a click on a toast) and the response arrives.
- **Windows cold start** (`scripts/notification-coldstart-e2e.ps1 <backend>`,
  after the step above registered the app): with no copy of the app running, the
  script points the activator's `LocalServer32` at a copy of the backend (with
  the runtime colocated and a `laufey-launch.json` naming the same app id) and
  clicks through COM, so Windows starts that copy with
  `-ToastActivated -Embedding`; the runtime's cold-start mode writes the
  response it received, and the script checks its tag, action, data and
  `launch: true`.
- **Linux cold start** (`scripts/notification-coldstart-e2e.sh <backend>`): the
  script copies the backend with the runtime colocated and a launch file naming
  `dev.laufey.e2e.coldstart`, installs that app's `.desktop` entry and D-Bus
  service file (`--laufey-dbus-activated`) under `$XDG_DATA_HOME`, runs the app
  once to post a notification (through the portal where the session has one) and
  lets it quit, then sends what the shell sends for a click,
  `org.freedesktop.Application.ActivateAction` on the app's name: D-Bus starts
  the copy and its cold-start mode (`LAUFEY_E2E_COLDSTART`, set by the service
  file's `Exec`) writes the response, checked as on Windows, that the runtime's
  arguments (`laufey::args_os()`) leave `--laufey-dbus-activated` out, and that
  the runtime library's `.init_array` function found the process's `argv` intact
  (no NULL below `argc`, which crashed Deno's runtime). With `--schedule` the
  app schedules the notification ten seconds ahead and quits; the script checks
  the app's systemd user timer, that the timer's launch posted and claimed it,
  then clicks. `LAUFEY_E2E_CLICK_CMD` (a desktop's own click hook) or
  `LAUFEY_E2E_CLICK=manual` clicks the real notification instead.

The portable pieces are unit-tested in
`backend-common/tests/menu_notifications_test.cc` (template parsing and
accelerators, the context-menu session, the notification core over a fake
platform, the schedule file, toast arguments) and the Linux D-Bus client in
`notifications_dbus_test.cc` against a mock server (Notify arguments, signals,
the scheduler and its persisted re-arm), `notifications_portal_dbus_test.cc`
(the portal transport, `ActivateAction` on the app's name, the cold-start
launch, scheduled launches and systemd timers against mock services),
`notifications_caps_dbus_test.cc` (a server without actions, with body markup)
and `launcher_entry_dbus_test.cc` (the launcher badge); the capi parsing in
`capi/src/menus_notifications.rs`.

Under `--window-api` (and the main battery), the fullscreen check also reads
`get_window_normal_bounds` while fullscreen: it must be the pre-fullscreen
bounds (macOS used to report the fullscreen frame once its transition had
settled).

## 20. UI-thread tasks and auth sessions (API 42)

`scripts/native-e2e-run.sh <backend> --auth-thread` runs `native_e2e`'s
`auth_thread_checks` alone, on every backend, and ends with `quit()`. See
[c-abi.md](c-abi.md#ui-thread-tasks-api--42) and
[auth-session.md](auth-session.md).

- **UI-thread tasks**: the runtime's thread is not the UI thread;
  `spawn_on_ui_thread`, `try_run_on_ui_thread` (from a plain thread) and
  `run_on_ui_thread` (littledivy/laufey#79) run their task on the UI thread, by
  laufey's account (`is_ui_thread`) **and the OS's**: `pthread_main_np()` on
  macOS, `gettid() == getpid()` on Linux, and on Windows the thread that owns
  the test window's HWND (`GetWindowThreadProcessId`). A call made on the UI
  thread runs inline; 20 tasks run in dispatch order; 200 blocking tasks from 4
  threads each run once. After `quit()` (the runtime sees `should_shutdown`), a
  task that raced the quit has been answered, and a new one is refused at once
  with `UiThreadError::Shutdown` instead of hanging.
- **Auth sessions**: the capabilities match the OS (macOS: supported +
  ephemeral; elsewhere and on Winit: none). Off macOS a session answers
  `not_supported` with the RFC 8252 hint. On macOS: argument refusals; a real
  `ASWebAuthenticationSession` round trip, ephemeral (no consent prompt),
  through a loopback "identity provider" that redirects to
  `laufey-e2e-auth://cb?code=…&state=s1`, resolving with exactly that URL; a
  second session meanwhile is `busy`; `test_cancel_auth_session` ends a session
  `cancelled` (as the user closing the sheet); `auth_session_cancel` (API 43)
  does too, a second call answers `false`, and the next session completes a
  round trip; closing the anchor window ends it as well; and, in CI only (it
  puts a prompt on the screen), a cancel while the consent prompt of a
  non-ephemeral session is up, which the OS itself never reports, still ends it
  `cancelled`.

The portable cores are unit-tested in
`backend-common/tests/auth_main_thread_test.cc` (the dispatcher over a fake
loop, including tasks pending when the loop ends and a refused post; auth
argument validation, the one-session slot, exactly-once results, races),
`backend-winit-common/src/ui_tasks.rs`, and `capi/src/ui_thread.rs` /
`capi/src/auth_session.rs` with fake vtables. The WebKitGTK response body has
its own test, `backend-common/tests/scheme_body_stream_test.cc` (writes never
block, an asynchronous pollable reader on a GMainContext, the cap, a reader that
went away, a synchronous reader).

## 21. The first window at launch

`scripts/native-e2e-run.sh <backend> --launch-visibility` runs `native_e2e`'s
`launch_checks` alone, on every backend. On macOS another process first covers
every screen with an ordinary window (`scripts/launch-occluder.swift`), the way
an editor or a terminal is in front when an app is started from it. See
[window-management.md](window-management.md#the-first-window-at-launch).

- **The launch window**, created hidden and shown once its page has loaded (the
  reveal Deno Desktop uses): `get_visible` holds, the page reads
  `document.visibilityState` "visible" and runs a `requestAnimationFrame`
  callback within 2 s (measured from the runtime, script round trips included),
  and its `outerWidth` / `outerHeight` are the window's frame, not 0.
  `document.hasFocus()` is logged, not checked: whether the system grants the
  activation is its own decision.
- **An unfocused window**: a second ordinary window, mostly beside the launch
  window, left unfocused when the launch window is focused again: its page is
  visible and draws a frame within 2 s. An engine does not throttle a visible
  window for lacking focus (a fully covered one is occluded and rightly stops,
  as in a browser).
- **A non-activating panel** (`no_activate`): its page is visible and draws a
  frame within 2 s. Known limitation, reported N/A on the WebKit engines:
  WKWebView runs no animation frames in a non-activating `NSPanel` that has
  never been key, though the page reads as visible, and WebKitGTK sometimes
  doesn't either; both draw once the panel is shown with `show()` / `focus()`.
  The same WKWebView panel in a standalone AppKit program draws, so the trigger
  is in laufey's panel setup and is still open.
- **A window created hidden** stays hidden through both reveals (the WebView
  backends; CEF and Winit have no hidden-on-create and report N/A).

Winit has no page and reports N/A for the page checks.
