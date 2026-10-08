# Dock / taskbar

laufey can badge the application icon, request the user's attention, and — on
macOS — drive the dock menu, the icon's visibility, and a reopen callback. These
are free functions rather than window methods, because the dock is
application-scoped on macOS, while on Windows and Linux the equivalent
operations act on the currently focused window's taskbar button.

```rust
use laufey::DockBounceType;

laufey::set_dock_badge(Some("3"));      // pass None to clear the badge
laufey::bounce_dock(DockBounceType::Critical);

laufey::on_dock_reopen(|has_visible_windows| {
  // On macOS, the user clicked the dock icon while no windows were open.
});
```

On macOS the badge is a native red overlay drawn on the dock tile. On Windows it
is a `"(N) "` prefix on the window titles, the convention used by applications
such as Slack, Discord, and Telegram; taskbars and window-manager overviews
surface that title. On Linux, under the CEF and WebKitGTK backends, a badge of
digits is the count on the app's launcher where a dock reads the
`com.canonical.Unity.LauncherEntry` API: one owns `com.canonical.Unity`
(Ubuntu's dock, Dash to Dock) or `org.kde.plasmashell` runs (Plasma's task
manager), and `<app id>.desktop` is installed for the dock to match
`application://<app id>.desktop` against. Otherwise (no such dock, no installed
desktop entry, a badge that isn't a number, or the Winit backend, which sends no
launcher badge) it is the same title prefix. `platform_features` reports which
one applies (`badge`, `badgeReason`). Requesting attention bounces the dock icon
on macOS, flashes the taskbar button on Windows, and sets the window's urgency
hint on Linux. The dock menu, the ability to hide the dock icon, and the reopen
callback exist only on macOS.
