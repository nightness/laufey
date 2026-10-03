# Window management

Every laufey application is built around one or more native windows. A `Window`
controls its title, size, position, resizable and always-on-top flags, opacity,
click passthrough, visibility, and focus. The type is a builder, so you can
configure a window fluently when you create it, and each property also has a
plain setter you can call later while the window is open.

```rust
use laufey::Window;

let win = Window::new(800, 600)
  .title("My App")
  .position(100, 100)
  .resizable(true)
  .opacity(0.95)
  .load("index.html"); // or .navigate("https://example.com")

win.set_size(1024, 768);
let (width, height) = win.get_size();
let scale = win.get_scale_factor(); // window.devicePixelRatio
win.focus();
win.hide();
```

A few properties can only be chosen when the operating system creates the window
and cannot be changed afterwards: whether the window is frameless (drawn without
operating-system chrome), whether it is a non-activating panel that does not
steal keyboard focus, and whether it has a transparent background. You set those
through `Window::new_with_options`. Everything else is a live setter. All
positions and sizes are expressed in density-independent pixels with the origin
at the top-left of the screen. `Window::get_scale_factor` is the physical-to-DIP
ratio for that window (`window.devicePixelRatio`); it updates when the window
moves to another display. `get_inner_position` is the content-view origin
(`get_position` plus title-bar chrome), so `inner + client` is
`MouseEvent.screenX` / `screenY`. The Winit backend can create and manage
windows, but because it has no web engine it cannot navigate to a URL or execute
JavaScript.

## Opacity and transparency

These are two distinct things:

- **Opacity** (`Window::opacity` / `set_opacity` / `get_opacity`) fades the
  _entire_ window — web content and native chrome alike — by a uniform factor in
  `0.0..=1.0`, where `1.0` is fully opaque (the default), like CSS `opacity` on
  the whole window. It is a live setter you can animate at runtime. The web
  backends implement it on every desktop platform (macOS `NSWindow.alphaValue`,
  Windows layered-window alpha, Linux `gtk_widget_set_opacity`). The Winit
  backend has no opacity API, so the call is a no-op there and `get_opacity`
  returns `1.0`.

  ```rust
  win.set_opacity(0.8); // 80% opaque
  ```

- **Transparency** (`WindowOptions::transparent`) gives the window a transparent
  _background_ so the web content's own alpha composites against whatever is
  behind the window. Any region the page leaves transparent (e.g. a
  `transparent` root background) shows the desktop through it. This must be
  chosen at creation time and is commonly paired with `frameless`.

  ```rust
  use laufey::{Window, WindowOptions};

  let win = Window::new_with_options(
    400,
    300,
    WindowOptions { frameless: true, transparent: true, ..Default::default() },
  )
  .load("index.html");
  ```

  Transparency is supported by the system-WebView backend on macOS and on Linux
  (WebKitGTK, on a compositing window manager), and by the Winit backend. It is
  not supported by the Windows WebView2 backend or the CEF backend, which paint
  an opaque window background; the flag is ignored there.

## Click passthrough

`Window::click_passthrough` / `set_click_passthrough` / `get_click_passthrough`
makes the window ignore _all_ mouse input — clicks, moves, and wheel events fall
through to whatever window is beneath it, like Electron's
`setIgnoreMouseEvents(true)`. Keyboard input is unaffected. It is a live setter
you can toggle at any time, intended for frameless/transparent overlay windows:
HUDs, notification toasts, screen annotations.

```rust
use laufey::{Window, WindowOptions};

let overlay = Window::new_with_options(
  400,
  300,
  WindowOptions { frameless: true, transparent: true, ..Default::default() },
)
.always_on_top(true)
.click_passthrough(true)
.load("overlay.html");

// Later, to start accepting input again:
overlay.set_click_passthrough(false);
```

Platform notes:

- **macOS** — `NSWindow.ignoresMouseEvents`; works with every backend.
- **Windows** — the top-level window gets the `WS_EX_TRANSPARENT` and
  `WS_EX_LAYERED` extended styles, which exclude it (children included) from
  mouse hit-testing. Composes with `set_opacity`, which shares the layered
  style.
- **Linux** — the window's X11/Wayland input shape region is cleared.
  Best-effort under a reparenting X11 window manager, where the WM's frame may
  still catch clicks — pair it with a frameless window (the intended overlay use
  case) for reliable behavior.
- The Winit backend uses winit's `set_cursor_hittest`, with the same platform
  behavior as above.

### Forwarding: passthrough, but still observing events

`Window::click_passthrough_forward` / `set_click_passthrough_forward` /
`get_click_passthrough_forward` keeps the window's mouse events flowing to your
registered `on_mouse_move` / `on_mouse_click` / `on_wheel` handlers _while_
passthrough is active — like Electron's
`setIgnoreMouseEvents(true, { forward: true })`. The OS still delivers every
event to the window beneath; forwarding is observation only, sourced from a
global input observer that hit-tests the overlay's frame. While passthrough is
disabled the flag has no effect, because normal per-window delivery already
fires the handlers.

You cannot selectively consume a forwarded event — hit-testing is decided by the
OS before your handler runs. The standard interactive-overlay pattern instead
toggles passthrough just-in-time: watch forwarded mouse moves and disable
passthrough when the cursor enters an interactive region, re-enable it on leave.

```rust
let overlay = Window::new_with_options(
  400,
  300,
  WindowOptions { frameless: true, transparent: true, ..Default::default() },
)
.always_on_top(true)
.click_passthrough(true)
.click_passthrough_forward(true)
.on_mouse_move(move |ev| {
  // e.g. flip set_click_passthrough(false) when (ev.x, ev.y) is over a button
})
.load("overlay.html");
```

Platform support: implemented on **macOS** (an `NSEvent` global monitor —
observing mouse events needs no extra permission) for the WebView and CEF
backends. **Windows** (a `WH_MOUSE_LL` hook), **Linux/X11** (XInput2 raw
events), and the Winit backend are not implemented yet and ignore the flag (the
getter reports `false`); **Linux/Wayland** cannot support it — the compositor
does not expose global input. One macOS caveat: global monitors never see events
delivered to your _own_ application, so if the event lands on another
(non-passthrough) window of the same app that overlaps the overlay, the
overlay's handlers do not fire for it.

## Maximize, minimize and fullscreen (API ≥ 38)

```rust
use laufey::{Window, WindowState};

let win = Window::new(800, 600)
  .on_state_change(|ev| {
    // ev.state / ev.previous: WindowState { maximized, minimized, fullscreen }
    if ev.state.fullscreen && !ev.previous.fullscreen {
      println!("entered fullscreen");
    }
  })
  .load("index.html");

win.maximize();
win.unmaximize();
win.minimize();
win.restore(); // un-minimize: back to the state before minimize
win.set_fullscreen(true);
let state: WindowState = win.get_state();
```

The setters are asynchronous wherever the OS animates the change (macOS zoom,
miniaturize and the fullscreen Space transition, every Linux window manager):
`get_state()` and the state handler report the change once the OS has applied
it, never before. The handler fires once per real change, whichever OS signal
noticed it, with the state before and after; resize, move, focus and blur keep
their own handlers. A window can be minimized while it is maximized (it restores
to maximized); `restore()` only un-minimizes, `unmaximize()` only un-maximizes.

- **macOS** (WKWebView and CEF): `zoom:`, `miniaturize:` / `deminiaturize:` and
  `toggleFullScreen:` (a fullscreen Space, Electron's default). A borderless
  window has no zoom button, so "maximized" is "fills the visible frame" there.
- **Windows** (WebView2 and CEF):
  `ShowWindow(SW_MAXIMIZE / SW_MINIMIZE /
  SW_RESTORE)`; fullscreen is
  borderless and covers the whole monitor, taskbar included, and leaving it
  restores the previous style and placement.
- **Linux** (WebKitGTK and CEF): `_NET_WM_STATE` requests through GTK /
  Chromium's X11 window, applied by the window manager. Under a bare X server
  (Xvfb without a window manager) nothing applies them and the state stays
  normal; the native e2e reports those checks as N/A there and runs them under
  xfwm4 for real coverage.
- **Winit**: `set_maximized`, `set_minimized`, `Fullscreen::Borderless`.

## Size constraints (API ≥ 38)

```rust
use laufey::SizeConstraints;

win.set_min_size(400, 300);
win.set_max_size(1600, 1200);
win.set_size_constraints(SizeConstraints { min_width: 400, min_height: 300, max_width: 0, max_height: 0 });
```

Constraints are content sizes in the same units as `set_size` (see below); 0 on
an axis means no limit there, and a maximum below the minimum is raised to it.
The OS enforces them while the user resizes (`contentMinSize` / `contentMaxSize`
on macOS, `WM_GETMINMAXINFO` on Windows, GTK geometry hints, the CEF window
delegate's `GetMinimumSize` / `GetMaximumSize`, winit's min / max inner size),
`set_size` clamps to them on every backend (some OS calls, such as
`-setContentSize:` and `SetWindowPos`, ignore the limits), and a window outside
a new range is resized into it.

## Screens (API ≥ 38)

```rust
for screen in laufey::screens() {
  println!("{} {:?} work {:?} @{}x primary={}", screen.id, screen.bounds,
    screen.work_area, screen.scale_factor, screen.is_primary);
}
let mine = win.get_screen(); // Option<Screen>
laufey::on_display_changed(|| { /* re-read laufey::screens() */ });
```

`screens()` lists the connected displays, primary first. Every rectangle is in
the same space as `get_position` / `set_position` on the backend, so a saved
window position can be checked against (and clamped to) the screens directly.
`work_area` is the bounds minus the menu bar, Dock, taskbar and panels. `id`
identifies a display while it stays connected (CGDirectDisplayID on macOS, a
hash of the device name on Windows and of the panel on GTK, CEF's display id)
and fits in a JS number. `on_display_changed` fires when a display is added or
removed, moved, or changes work area or scale (macOS
`NSApplicationDidChangeScreenParametersNotification`, Windows `WM_DISPLAYCHANGE`
/ `WM_SETTINGCHANGE(SPI_SETWORKAREA)` on a hidden top-level watcher window, GDK
monitor signals; CEF on Linux has no such signal and compares the layout every 2
s instead). Winit reports a screen's work area as its bounds (winit has no
work-area API) and has no display-changed event.

## Restoring a window where the user left it (API ≥ 38)

`get_normal_bounds()` returns the bounds a window returns to when it leaves the
maximized, minimized or fullscreen state (its current bounds when it is normal),
as a position (`get_position`) and size (`get_size`). Save that, plus whether
the window was maximized, when the app quits; on the next launch check the saved
rectangle against `screens()` (a monitor may be gone), then `set_position` +
`set_size` and `maximize()` if it was. Persisting it is the app's job, laufey
keeps nothing across runs.

Windows asks the OS (`GetWindowPlacement`, converted from workspace to screen
coordinates). Elsewhere the backend records the geometry it sees while the
window is normal and keeps it once it has been unchanged for 300 ms, so the
intermediate frames of a zoom animation (which macOS reports while the window
still looks normal) never replace the frame the window had before.

## Title bar styles (API ≥ 38, macOS)

```rust
use laufey::TitlebarStyle;

win.set_titlebar_style(TitlebarStyle::HiddenInset); // false where unsupported
win.set_traffic_light_position(Some((16, 18)));      // None puts them back
```

`Hidden` is the runtime form of `WindowOptions::transparent_titlebar` (the
content extends under a transparent title bar with the system buttons overlaid);
`HiddenInset` adds the taller title bar of Electron's `hiddenInset` (an empty
unified toolbar). The traffic-light position is kept across resizes, fullscreen
and title changes. Both work on WKWebView and on CEF's NSWindow; the other
platforms have no title bar API of this kind and return `false`.

## Backdrops: Mica, Acrylic, vibrancy (API ≥ 38)

```rust
use laufey::{Backdrop, VibrancyMaterial};

win.set_backdrop(Backdrop::Mica);                                 // Windows 11
win.set_backdrop(Backdrop::Vibrancy(VibrancyMaterial::Sidebar));  // macOS
win.set_backdrop(Backdrop::None);
```

A backdrop shows only where the page leaves its background transparent (e.g.
`html, body { background: transparent }`); the backend makes the web view's own
background transparent while one is set.

- **Windows 11, WebView2**: `DWMWA_SYSTEMBACKDROP_TYPE` (Mica, Acrylic, tabbed
  Mica) on build 22621 and later; on 22000–22620 only Mica, through the older
  `DWMWA_MICA_EFFECT`; nothing on Windows 10. The frame is extended into the
  client area and the WebView2 default background made transparent.
- **macOS, WKWebView**: an `NSVisualEffectView` (behind-window blending) is put
  behind the web view, which stops drawing its background.
- **CEF, every OS**: not supported — the CEF browser view paints an opaque
  background in windowed mode (the same reason `WindowOptions::transparent` is
  ignored there), so nothing could show through.
- **WebKitGTK / Winit**: no backdrop API.

`set_backdrop` returns `false` and changes nothing when the backend / OS can't
show that backdrop.

## Capabilities (API ≥ 38)

`laufey::window_capabilities()` says what this backend can do on this OS (and
build), so an app can surface "unsupported" instead of pretending:

| Capability                | WKWebView | CEF macOS | WebView2       | CEF Windows | WebKitGTK | CEF Linux       | Winit                  |
| ------------------------- | --------- | --------- | -------------- | ----------- | --------- | --------------- | ---------------------- |
| state + state events      | ✓         | ✓         | ✓              | ✓           | ✓ (WM)    | ✓ (WM)          | ✓                      |
| size constraints          | ✓         | ✓         | ✓              | ✓           | ✓         | ✓               | ✓                      |
| screens                   | ✓         | ✓         | ✓              | ✓           | ✓         | ✓               | ✓ (work area = bounds) |
| display-changed events    | ✓         | ✓         | ✓              | ✓           | ✓         | ✓ (polled, 2 s) | –                      |
| title bar hidden / inset  | ✓         | ✓         | –              | –           | –         | –               | –                      |
| traffic-light position    | ✓         | ✓         | –              | –           | –         | –               | –                      |
| Mica / Acrylic / tabbed   | –         | –         | ✓ (Windows 11) | –           | –         | –               | –                      |
| vibrancy                  | ✓         | –         | –              | –           | –         | –               | –                      |
| normal bounds             | ✓         | ✓         | ✓              | ✓           | ✓         | ✓               | ✓                      |
| keep-alive with no window | ✓         | ✓         | ✓              | ✓           | ✓         | ✓               | ✓                      |
| set position              | ✓         | ✓         | ✓              | ✓           | X11 only  | X11 only        | not on Wayland         |

"(WM)": applied by the window manager; nothing changes under a bare X server.

**Units.** Sizes, constraints, positions and screen rectangles are in DIP
(density-independent pixels: points on macOS, CSS pixels at zoom 1) on every
backend. `get_size` / `set_size` and the size constraints are the window's
**content** area, the page (`window.innerWidth` / `innerHeight`), and
`get_outer_size` is the whole window, frame included; `get_position` is the
frame's top-left corner and `get_inner_position` the content's. On Windows the
process is per-monitor DPI aware: a window's geometry is converted with the
scale of the monitor it is on, a screen's with its own, so DIP coordinates are
continuous within one monitor (as in winit) but not across monitors with
different scales. WebView2 reported physical pixels and the outer window
rectangle for `get_size` / `set_size` in earlier versions, and CEF on Windows
and macOS the whole window; both now size the content like the other backends.
The page's `window.outerWidth` / `outerHeight` and `screenX` / `screenY` are the
same frame as `get_outer_size` / `get_position`; WKWebView answered 0 for all
four before laufey answered WebKit's request for the window frame.

## The first window at launch

An app is often started by something that stays frontmost: a terminal, an IDE's
task runner, a CI agent. On macOS 14 and later an app only takes activation
cooperatively, so such a launch can leave it inactive, and a window that was
merely made key and ordered front would open behind the active app's windows.
WebKit and Chromium read a fully covered window as occluded: the page reports
`document.visibilityState` "hidden" and `requestAnimationFrame` stops until the
window is uncovered. So the first window an app shows (at creation, or with
`show()` for one created `hidden`) activates the app, becomes key and is ordered
in front of other apps' windows even when the activation is declined. A window
created `hidden` is never revealed by this, and a non-activating panel
(`no_activate`) never activates the app. Later windows are made key and ordered
front within the app, as before. A window that is covered, minimized or on a
locked screen is still throttled by the engine, as in a browser.

## Keeping a tray app alive; quitting (API ≥ 38)

By default the event loop ends when the last window closes. A tray / menu-bar
app calls `laufey::set_quit_on_last_window_closed(false)` and keeps running with
no window (it can open one again later); `laufey::quit()` ends it. On macOS an
app whose activation policy is Accessory (`set_dock_visible(false)`) also stays
alive when its last window closes, as it did before.

`laufey::quit()` ends the loop the way closing the last window does: the backend
returns from its run loop and calls the runtime's `laufey_runtime_shutdown`.
Windows still open are closed without a close-requested event (quitting is
app-level; see [window-events.md](window-events.md)). CEF closes its browsers
first and ends the loop when the last one is gone (macOS stops `[NSApp run]`,
which `CefQuitMessageLoop` did not); WebView2 posts the quit to its UI thread
(it used to post it to the calling thread's queue); Winit now calls the
runtime's shutdown once its loop has ended, like the other backends.
