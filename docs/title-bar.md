# Title bars

People set up their title bars: which side the window buttons are on and in
which order, light or dark, an accent colour, the title bar font, and what a
double click on a title bar does. laufey never draws a title bar of its own, so
a window with a frame looks like every other window on the desktop. An app that
hides its title bar and draws its own reads the same settings through
`title_bar_preferences` (API 47), so its buttons and drag region behave like the
rest of the desktop.

## Windows with a frame

| Where                                                                                  | Who draws the title bar                                                                                                                                                                                                                                                       |
| -------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| macOS, Windows                                                                         | The OS.                                                                                                                                                                                                                                                                       |
| Linux, X11                                                                             | The window manager (server-side decorations).                                                                                                                                                                                                                                 |
| Linux, Wayland with server-side decorations (KWin, Sway and other wlroots compositors) | The compositor: GTK (WebView backend) asks for them with KDE's server-decoration protocol, Chromium (CEF backend) and winit with `xdg-decoration`. Every KWin setting (theme, buttons, colours) applies.                                                                      |
| Linux, Wayland without them (GNOME)                                                    | The toolkit, client side. WebView: GTK's own header bar, which follows `gtk-decoration-layout`, the theme and the double-click action. CEF: Chromium's GTK-themed frame. Winit: sctk-adwaita, which reads the portal's button layout and colour scheme when the window opens. |

Nothing branches on the desktop's name: each backend asks the compositor for
server-side decorations and draws (or has GTK draw) the frame only when the
compositor has none.

## `title_bar_preferences`

```rust
if let Some(json) = laufey::title_bar_preferences() {
  println!("{json}");
}
laufey::on_title_bar_preferences_changed(|| {
  // read laufey::title_bar_preferences() again
});
```

The C ABI entry point returns a JSON object the caller frees with `string_free`.
It may be called from any thread; on Linux the first call may wait a few seconds
for xdg-desktop-portal to start.

| Key           | Value                                                                                                                                                                                                                                          |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `buttons`     | `{ "left": [...], "right": [...] }`: the buttons on each side of the title, in order. Items are `"close"`, `"minimize"`, `"maximize"` and the app's own menu or icon (`"appmenu"`, `"menu"`, `"icon"`), as GTK's decoration layout names them. |
| `side`        | `"left"` or `"right"`: where the close button is (with none, the side with the other buttons).                                                                                                                                                 |
| `doubleClick` | What a double click on a title bar does: `"maximize"` (toggle), `"minimize"`, `"shade"` (roll up), `"lower"`, `"menu"` (the window menu) or `"none"`.                                                                                          |
| `colorScheme` | `"light"`, `"dark"` or `"no-preference"`.                                                                                                                                                                                                      |
| `accentColor` | `"#rrggbb"`, or `null` when the session has none.                                                                                                                                                                                              |
| `font`        | The title bar font (`"Cantarell Bold 11"`), or `null`.                                                                                                                                                                                         |
| `source`      | Where the button layout came from: `"portal"`, `"gsettings"`, `"default"` (GTK's defaults: nothing answered) or `"os"` (macOS, Windows).                                                                                                       |

### Linux

The sources, highest priority first, key by key:

1. **xdg-desktop-portal's Settings interface**: one `ReadAll` of
   `org.gnome.desktop.wm.preferences` (`button-layout`,
   `action-double-click-titlebar`, `titlebar-font` /
   `titlebar-uses-system-font`), `org.freedesktop.appearance` (`color-scheme`,
   `accent-color`) and `org.gnome.desktop.interface` (for a portal without the
   appearance keys). Every desktop's portal answers these: GNOME's from
   GSettings, Plasma's from KWin's decoration settings and its colour scheme.
2. **GSettings**, for a key the portal didn't answer (no portal running).
3. **GTK's defaults**: `menu:minimize,maximize,close`, a double click maximizes,
   no colour preference.

The portal wins over GSettings: on Ubuntu GNOME, GSettings' `button-layout` can
differ from the portal's (an Ubuntu override), and the portal's is what GTK 4
and sandboxed apps show.

The change handler follows the portal's `SettingChanged` (and GSettings'
`changed`) on a watcher thread of its own, waits for a burst of changes to
settle (150 ms), and fires when the answer differs from the last one reported. A
portal that starts after the handler was set is heard too.

### macOS

The buttons are on the left (`close`, `minimize`, `maximize`). `doubleClick` is
System Settings' "Double-click a window's title bar to"
(`AppleActionOnDoubleClick`: Zoom and Fill read `"maximize"`, Minimize
`"minimize"`, Do Nothing `"none"`; the older `AppleMiniaturizeOnDoubleClick`
where that is unset). The colour scheme and accent colour are the appearance's.
The handler fires on the main thread when the appearance or the accent colour
changes; the double-click setting is read on every call (macOS posts nothing
when it changes).

### Windows

The buttons are on the right (`minimize`, `maximize`, `close`) and a double
click maximizes. The colour scheme is the app colour mode
(`Personalize\AppsUseLightTheme`), the accent colour DWM's `AccentColor`. The
handler fires on a watcher thread when either changes.

### Winit

The Winit backend reports the same object on Linux (read with zbus and gio). On
macOS and Windows its entry points are `NULL`.

## Testing

- `laufey_title_bar_test` (ctest): the layout parser, the merge (the portal
  wins), the JSON, and the OS's answer on macOS and Windows.
- `laufey_title_bar_dbus_test` (ctest, Linux): a mock portal Settings interface
  on a private bus and GSettings' memory backend: the portal wins over
  GSettings, `SettingChanged` is followed live (a burst is one call; an
  unrelated key is none), a GSettings change of a key the portal doesn't answer.
- `scripts/native-e2e-run.sh <backend> --title-bar`: the backend's answer and
  its change handler. Under Xvfb the battery serves the private bus's portal
  Settings and moves the buttons; in a real session
  (`LAUFEY_E2E_HOST_SESSION=1`) `LAUFEY_E2E_TITLEBAR_SET_CMD` changes a setting
  and the handler must see it.
