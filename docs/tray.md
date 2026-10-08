# Tray / status bar

A tray icon is a persistent icon in the operating system's status area: the menu
bar on macOS, the system tray on Windows, and the AppIndicator area on Linux.
Each icon has an image, a tooltip, a right-click menu, and click handlers.
`TrayIcon` is a builder; you must keep the returned value alive for the icon to
remain visible.

```rust
use laufey::{MenuItem, TrayIcon};

let tray = TrayIcon::new()
  .icon(include_bytes!("icon.png"))
  .icon_dark(include_bytes!("icon-dark.png")) // optional dark-mode variant
  .tooltip("My App")
  .menu(&[MenuItem::Role { role: "quit".into() }], |id| println!("{id}"))
  .on_click(|| println!("clicked"))
  .on_double_click(|| println!("double clicked"));

// The icon's bounds let you anchor a popover panel beneath it.
let bounds = tray.get_bounds(); // Option<(x, y, width, height)>
```

When you provide both a light and a dark icon, the backend watches the system
appearance and swaps between them live: it observes
`AppleInterfaceThemeChangedNotification` on macOS, the `WM_SETTINGCHANGE`
message together with the `AppsUseLightTheme` setting on Windows, and polls once
per event-loop tick on Winit. On Linux, AppIndicator renders the icon through
the desktop theme and does not deliver click or double-click events, so click
handlers and dark-mode swapping have no effect there (`platform_features()`
reports `"trayClicks": false`). The tooltip is set as the indicator's title,
which StatusNotifier hosts (Plasma, the GNOME AppIndicator extension) show on
hover. The CEF backend also uses AppIndicator on Linux, so a tray icon does not
require a browser window.

On Linux, the CEF and WebView backends load the appindicator library at runtime
— `libayatana-appindicator3.so.1` first, falling back to the legacy
`libappindicator3.so.1`. If neither is installed, tray creation returns id `0`
and all tray calls are no-ops; the rest of the application is unaffected. If you
package an application that uses a tray icon as a `.deb`/`.rpm`, declare a
dependency on the distro's Ayatana runtime package (Debian/Ubuntu:
`libayatana-appindicator3-1`; Fedora: `libayatana-appindicator-gtk3`).

The session must also have a tray host for the icon to show: a StatusNotifier
watcher (`org.kde.StatusNotifierWatcher` on the session bus; Plasma, Cinnamon,
Ubuntu's GNOME and most others run one, stock GNOME only with the AppIndicator
extension) or, on X11, an XEmbed system tray. Without one, `create_tray_icon`
returns `0` instead of an icon no one could see, logs the reason once, and
`platform_features()` reports `"trayHost": false` with that reason in
`"trayReason"` (see [Platform features](platform-features.md)). A tray-only app
should show a window instead. The probe follows the watcher's
`NameOwnerChanged`, so a host that starts later (an extension enabled, the shell
restarted) counts at once, and the platform-features change handler
(`laufey::on_platform_features_changed`, the C ABI's
`set_platform_features_changed_handler`, API 45) fires: create the tray again
when `"trayHost"` becomes `true`. `laufey::tray_unavailable_reason()` (the C
ABI's `tray_unavailable_reason`) answers the tray part of the probe alone, so it
never waits for xdg-desktop-portal.
