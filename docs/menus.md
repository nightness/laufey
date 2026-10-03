# Menus

laufey supports three kinds of menus: an application menu bar, per-window
context menus, and the developer tools. A menu is described by a slice of
`MenuItem` values, which can be regular items, submenus, separators, or standard
roles such as quit, copy, and paste. Items may carry a keyboard accelerator.
When the user clicks an item that has an identifier, your callback is invoked
with that identifier.

Regular items also support a few visual properties, mirroring Electron's
`MenuItem`: `checked` (a checkmark, all platforms), `icon` (PNG-encoded image
bytes, matching the tray and notification icon APIs), and `tooltip` (hover
text). See [Per-platform support](#per-platform-support) for where each is
drawn.

```rust
use laufey::MenuItem;

let menu = [MenuItem::Submenu {
  label: "File".into(),
  items: vec![
    MenuItem::Item {
      label: "Open".into(),
      id: Some("open".into()),
      accelerator: Some("CmdOrCtrl+O".into()),
      enabled: true,
      checked: false,
      icon: Some(include_bytes!("icons/open.png").to_vec()), // PNG bytes
      tooltip: Some("Open a file".into()),
    },
    MenuItem::Separator,
    MenuItem::Role { role: "quit".into() },
  ],
}];

win.set_menu(&menu, |id| println!("menu: {id}"));
win.show_context_menu_with_close(
  x,
  y,
  &menu,
  |id| println!("context: {id}"),
  || println!("context menu closed"),
);
win.open_devtools();
```

On macOS the application menu is the global menu bar at the top of the screen,
and laufey swaps it as windows take focus. On Windows and Linux the menu is
attached to the individual window. A context menu is a pop-up shown at a point
you specify, in window coordinates (top-left origin, the page's coordinates).

## Accelerators

An item's `accelerator` uses the same syntax as global shortcuts (see
[global-shortcuts.md](global-shortcuts.md)): `+`-separated, case-insensitive,
modifiers first and exactly one key last, such as `CommandOrControl+Shift+K`,
`Alt+F4` or `Ctrl+Num5`. `CommandOrControl` is Command on macOS and Control
elsewhere; `Command` alone is macOS-only. An accelerator that does not parse is
ignored with a warning on stderr; the item still works, without a key.

In the application menu an accelerator fires its item from the keyboard while
the window has the focus, the page included:

| Backend          | Mechanism                                                                                                                                                                                |
| ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| macOS (both)     | `NSMenuItem` key equivalents on the main menu                                                                                                                                            |
| Windows WebView2 | an accelerator table (`CreateAcceleratorTableW`), run through `TranslateAccelerator` in the message loop, and matched in WebView2's `AcceleratorKeyPressed` while the page has the focus |
| Linux WebKitGTK  | a `GtkAccelGroup` on the window (GtkWindow runs accelerators before the web view sees the key)                                                                                           |
| CEF (Win, Linux) | CEF window accelerators (`CefWindow::SetAccelerator`, high priority, so they win over the page)                                                                                          |

A disabled item's accelerator does nothing. In context and tray menus the
accelerator is shown next to the label but not bound. Windows accelerator tables
and CEF accelerators have no Windows-key (`Super`) modifier, so such an
accelerator is shown but not bound there. On Windows the label shows the
accelerator in the canonical form (`Ctrl+Shift+N`).

## The context menu's close callback

`Window::show_context_menu_with_close` (C ABI: `show_context_menu_ex`, API 41)
calls `on_closed` exactly once when the menu closes: after `on_click` when an
item was chosen, or alone when it was dismissed (Escape, a click outside,
another window taking the focus). It never blocks the caller. A menu that could
not be shown (an unknown window, an empty template, no interactive desktop)
reports its close at once. `menu_capabilities().context_closed()` says whether
the backend reports the close.

## Per-platform support

| Backend          | App menu                    | Accelerators | Context menu                       | Icons             | Tooltips |
| ---------------- | --------------------------- | ------------ | ---------------------------------- | ----------------- | -------- |
| macOS WKWebView  | global menu bar (`NSMenu`)  | yes          | `NSMenu` pop-up                    | yes (template)    | yes      |
| macOS CEF        | global menu bar (`NSMenu`)  | yes          | `NSMenu` pop-up                    | yes (template)    | yes      |
| Windows WebView2 | Win32 menu bar (`SetMenu`)  | yes          | `TrackPopupMenu`                   | yes               | no       |
| Windows CEF      | Views menu bar              | yes          | `TrackPopupMenu`                   | context menu only | no       |
| Linux WebKitGTK  | `GtkMenuBar` above the page | yes          | `GtkMenu` at the given point       | yes               | yes      |
| Linux CEF        | Views menu bar              | yes          | Views menu (`CefWindow::ShowMenu`) | no                | no       |
| Winit            | per backend                 | no           | per backend                        | —                 | —        |

A CEF window is a Chromium Views window that owns its whole client area (and on
Linux there is no GtkWindow at all), so neither a Win32 menu bar nor a
`GtkMenuBar` can be attached to it. Its application menu is a Views menu bar
instead: a row of menu buttons above the page, each dropping a menu, the
approach of CEF's own sample client. Views menus draw no item icons or tooltips.
On macOS a monochrome black+alpha icon is treated as a template and tints to
white on selection; Windows and Linux render icons as-is (Linux uses a
`GtkImageMenuItem`, which the AppIndicator tray also exports). A checked item
shows its checkmark instead of an icon on Linux.

On Windows (both backends) a context menu is a `TrackPopupMenu` modal loop on
the UI thread:

- Only one menu can be active on a thread. When another one is (the window's
  menu bar or system menu in keyboard menu mode, which a lone press and release
  of Alt starts, or a context menu that is still open), `TrackPopupMenu` would
  fail at once and nothing would show; laufey ends that menu first and shows the
  new one as soon as its loop has unwound (within about a second at most).
- The owner window is made the foreground window before the menu opens, so the
  menu gets keyboard input (arrows, Return, Escape) and closes on a click
  elsewhere (KB135788).
- The UI thread keeps doing its work while the menu is open. Chromium runs no
  tasks inside a native modal loop unless told to, so on CEF everything the
  runtime sends through CEF's task queue (a page's binding calls, synchronous
  window calls, `dispatch_ui_task`) used to wait for the menu, and a runtime
  blocked on one stopped altogether; the CEF backend allows nestable tasks for
  the length of the loop (`CefSetNestableTasksAllowed`). WebView2 delivers its
  work as window messages, which the loop dispatches anyway.

## Test hooks

- `test_click_menu_item(id)` runs a menu or tray item's click handler (every
  backend, Windows app menus included from API 41).
- `test_trigger_menu_accelerator(window, accelerator)` (API 41) presses an
  accelerator through the backend's own dispatch:
  `-[NSMenu
  performKeyEquivalent:]`, `TranslateAccelerator` (with the thread's
  keyboard state holding the modifiers), `gtk_accel_groups_activate`, the CEF
  window's accelerator.
- `test_dismiss_context_menu()` (API 41) closes the open context menu as Escape
  would.

See [e2e-testing.md](e2e-testing.md).
