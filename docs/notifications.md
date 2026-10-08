# Notifications

laufey can post system notifications. The options mirror a subset of the Web
Notifications API: a title and body, an icon, a tag that replaces an earlier
notification carrying the same tag, a silent flag, a require-interaction flag,
and action buttons. From API 41 a notification can also be scheduled, carry
opaque data, be cancelled by tag, and a click on it reaches the app even when
the notification outlived the process that posted it. Notifications are
application-scoped.

```rust
use laufey::Notification;

let handle = Notification::new("Build finished")
  .body("3 warnings")
  .icon(include_bytes!("icon.png").to_vec())
  .tag("build")
  .data(r#"{"build":42}"#)
  .action("rebuild", "Rebuild")
  .on_event(|event| println!("{event:?}")) // shown, clicked, closed, or action
  ;

handle.close();

// Later, or in another run of the app:
laufey::set_notification_response_handler(|response| {
  // A click no live `on_event` callback owns: response.tag, response.action
  // (None for the body), response.data, and response.launch (true for the
  // click that launched the app).
});
```

## Scheduling

`Notification::schedule_at(time)` delivers the notification at that time instead
of now (a time in the past shows it now). A scheduled notification is identified
by its tag: `list_scheduled_notifications` lists the pending ones (tag, title,
body, time, data, actions; soonest first) and `cancel_notification(tag)` cancels
one (and removes a delivered one with that tag from the notification center).
Give a scheduled notification a tag; without one the crate makes one up.

| Platform | Scheduler                                                                                                                                                  | Delivered while the app is not running  |
| -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| macOS    | `UNTimeIntervalNotificationTrigger` (under a minute away) or `UNCalendarNotificationTrigger` (the wall-clock date, so a sleep in between doesn't shift it) | yes (the system holds it)               |
| Windows  | `ScheduledToastNotification`                                                                                                                               | yes (the system holds it)               |
| Linux    | laufey's own timer (the freedesktop protocol has no scheduler), plus a systemd user timer per notification where a systemd user manager runs               | with a systemd user manager — see below |

On Linux the schedule is persisted in the app data directory
(`laufey-notifications.json`, see [app-data.md](app-data.md)) and re-armed at
the next launch, where a notification whose time passed while the app wasn't
running is delivered at once. Without an app data directory (no `LAUFEY_APP_ID`
or `LAUFEY_DATA_DIR`) the schedule lives only as long as the process. Several
instances share the file under a lock and a due notification is claimed from it
before it is posted, so it fires once. With no notification server on the
session bus (yet), a due notification waits and is retried every 30 seconds.

Where a systemd user manager answers on the session bus
(`org.freedesktop.systemd1`), each scheduled notification also gets a transient
timer, `laufey-<app id>-<tag id>.timer` (the tag id is 16 hex digits of the
tag's FNV-1a hash), created with `StartTransientUnit` the way
`systemd-run --user
--on-calendar=` creates one. At the notification's time
(`OnCalendar=`, UTC, to the second) it runs `<exe> --laufey-notify <tag id>`
with `LAUFEY_APP_ID` (and `LAUFEY_DATA_DIR` when set) in its environment. That
launch posts the stored notification and exits, before any web engine or the
runtime loads. When the app runs (another process owns the app's D-Bus name) it
first asks it to post the notification itself, with
`ActivateAction("laufey-schedule-due", [<tag id>])` on that name: the app
re-reads the schedule, posts what is due (an entry it never armed too, such as
one another instance scheduled) and then answers. The launch posts the entry
itself only if it is still in the file after that, or after two seconds without
an answer, so a process that took the app's name without being the app can't
make it vanish; either way the entry is claimed from the file under its lock, so
it is posted once. The app also re-reads the schedule each time one of its own
timers fires. A notification less than two seconds away gets no timer.
Cancelling one stops its timer. Transient timers last until the user manager
stops (a logout without lingering, a reboot); each launch of the app re-creates
them for the pending notifications. An AppImage's timer runs `$APPIMAGE`,
trusted only while the running executable is inside the AppImage's mount
(`$APPDIR`). The unit name keeps at most 200 characters of the app id (a longer
one is cut, with 8 hex digits of its hash), so it stays within systemd's limit.
Without a systemd user manager the app's own timer is the only scheduler.

`notification_capabilities()` reports `schedule()` and `schedule_persists()` (on
Linux: the systemd timers are available).

## Clicks, actions and responses

A click on a notification shown by this process goes to its `on_event` callback:
`Clicked` for the body, `Action(id)` for an action button. A click on any other
notification of the app — one an earlier run posted, a scheduled one delivered
after a restart, the one whose click launched the app — goes to the response
handler (`set_notification_response_handler`, C ABI
`set_notification_response_handler`) as a `NotificationResponse` with its tag,
action, data and a `launch` flag. Responses that arrive before a handler is
registered are held (at most `LAUFEY_MAX_PENDING_NOTIFICATION_RESPONSES`, the
oldest dropped first) and delivered, with `launch: true`, when one registers:
that is how the click that launched the app reaches it, like the cold-start
deep-link buffer ([deep-links.md](deep-links.md)).

| Platform | Mechanism                                                                                                              | Cold-start click                                       |
| -------- | ---------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------ |
| macOS    | `UNUserNotificationCenter` delegate, installed before AppKit finishes launching; actions are `UNNotificationCategory`s | yes                                                    |
| Windows  | toast activation through the app's COM activator (`INotificationActivationCallback`), registered at launch             | yes                                                    |
| Linux    | the xdg-desktop-portal Notification interface: `org.freedesktop.Application.ActivateAction` on the app's D-Bus name    | with the portal and the D-Bus service file — see below |

On Linux a click reaches a quit app only through the portal path (below): the
desktop calls `ActivateAction` on the app's D-Bus name, and D-Bus starts the app
from the service file a `.deb` or `.rpm` installs. Through
`org.freedesktop.Notifications` itself, the server sends `ActionInvoked` to the
connection that posted the notification, so a click after that process exited
can't reach the app. `notification_capabilities()` reports `cold_start()` when
the click would start it.

### Click authenticity

On Windows and on the Linux portal path a click arrives as a call that any
process of the user can make: `INotificationActivationCallback::Activate` on the
app's COM class, `ActivateAction` on its D-Bus name. Both carry the tag, action
and data in their arguments (`laufey=1&tag=…&action=…&data=…`), so laufey signs
what it posts: the arguments end in `&mac=` and an HMAC-SHA256 of the rest under
a key of this install's own, and a click whose MAC doesn't verify is dropped
without a trace (set `LAUFEY_NOTIFICATION_DEBUG=1` for a line on stderr).
Arguments over 16 KiB, a tag over `LAUFEY_NOTIFICATION_MAX_TAG_BYTES`, data over
`LAUFEY_NOTIFICATION_MAX_DATA_BYTES` (4 KiB) or an action id over 1 KiB are
dropped too. A dropped click neither reaches a handler nor counts as the click
that launched the app.

The key is `laufey-notification-key` (32 random bytes, hex) in the app data
directory ([app-data.md](app-data.md)), created on first use: owner-only
(`0600`) on Unix, replaced if it is damaged, a link, or readable by others;
under `%LOCALAPPDATA%`'s per-user ACL on Windows. Every process of the install
reads the same key, so a cold-start click, a click on a scheduled notification
(including one the Linux systemd timer's `--laufey-notify` launch posted) and a
click after a restart all verify. Without an app data directory (no app id) the
key is this process's alone: only clicks on what this process posted arrive.

A MAC proves only that the arguments were posted by this install. It doesn't
make the data trustworthy: a notification's `data` is whatever the app put there
(often from a web page or a server), and the response handler must treat it as
untrusted input. Clicks through macOS's `UNUserNotificationCenter` and the Linux
`org.freedesktop.Notifications` path (the server sends `ActionInvoked` to the
posting connection, and laufey keeps the tag and data itself) carry nothing that
needs a MAC.

## Platforms

### macOS

`UNUserNotificationCenter`. The tag is the request identifier (a later request
with the same tag replaces it); the request's `userInfo` carries the tag, the
data and the actions, so a click and the pending list have them after a
relaunch. Each action set is a `UNNotificationCategory` named after a hash of
the set; the categories earlier runs registered are kept, since a pending
request still needs its own. A banner shows even while the app is frontmost. The
icon is not shown (the app's icon is the bundle's). UN requires the process to
run inside a bundled `.app` with a `CFBundleIdentifier`; unbundled, every
capability is off and showing fails.

Authorization: see [permissions.md](permissions.md). `request_permission` with
`PermissionKind::NotificationsProvisional` asks for quiet ("provisional")
authorization, which macOS grants without a prompt.

### Windows

WinRT toast notifications (`ToastNotificationManager`) with action buttons (at
most five), the icon as the toast's app logo, `silent` as a silent audio element
and `require_interaction` as a long duration. An unpackaged app has no package
identity, so laufey registers one per user, the way the Windows App SDK and the
Windows Community Toolkit do:

```
HKCU\Software\Classes\AppUserModelId\<AUMID>
    DisplayName      the app's name (the exe's FileDescription / ProductName, else its name)
    IconUri          a PNG of the executable's icon (in %TEMP%\laufey-notifications)
    CustomActivator  {CLSID}
HKCU\Software\Classes\CLSID\{CLSID}\LocalServer32
    (default)        "<exe>" -ToastActivated
```

The AppUserModelID is `LAUFEY_APP_ID` (or the launch file's `appId`), else the
executable's name; the CLSID is derived from it, so it is stable across runs and
differs between apps. No Start menu shortcut is needed, and the process's own
AppUserModelID (taskbar grouping) is left alone. The registration is written the
first time the app uses notifications, and rewritten only when a value changed
(the executable moved).

At launch every laufey process registers the activator's class object
(`CoRegisterClassObject`). A click on a toast while the app runs reaches it in
that process; while the app isn't running, Windows starts the executable from
`LocalServer32`, so its command line carries `-ToastActivated -Embedding`, and
the click is delivered to the response handler with `launch: true` (even if the
handler was registered before COM handed the click over).
`scripts/notification-coldstart-e2e.ps1` tests that round trip.

A toast that times out moves to the notification center; laufey reports it
closed, and a later click there arrives as a response. Uninstallers should
remove the two registry keys.

Permission: Windows has no prompt; the status is the user's setting for the app
(`ToastNotifier.Setting`): granted when enabled, else denied.

### Linux

Two transports, chosen once per process.

**The portal** (`org.freedesktop.portal.Notification`, version 1) when all of
these hold: xdg-desktop-portal registers the app's id for this host app
(`org.freedesktop.host.portal.Registry.Register`, xdg-desktop-portal 1.19 or
later; it is the first portal call on laufey's own connection), the portal has
the Notification interface, and `<app id>.desktop` is installed in an XDG data
directory (the shell names the app by it). `AddNotification` takes the tag as
its id, the title, the body, the priority (`urgent` for `require_interaction`,
else `normal`), the icon (a serialized `GBytesIcon`), and every click as the
GApplication action `app.laufey-notification` with a target string holding the
tag, the action and the data (the toast-argument encoding). Version 1 has no
sound setting (`silent` is ignored) and reports no closes. Closing removes the
notification (`RemoveNotification`).

While it runs, the app owns its D-Bus name (the app id, never queued: a second
instance doesn't take it) and exports `org.freedesktop.Application` at the app
id's path (`/` and the id with `.` as `/`, `-` as `_`). A click becomes
`ActivateAction("laufey-notification", [target])` there: gnome-shell sends it
for xdg-desktop-portal-gnome, KDE's portal for Plasma. When the app isn't
running, D-Bus starts it from `<app id>.service` in `dbus-1/services` (the
`.deb` and `.rpm` install it; its `Exec` line passes `--laufey-dbus-activated`),
and the click is delivered to the response handler with `launch: true`, as on
Windows. The host leaves `--laufey-dbus-activated` out of the arguments it hands
the web engine and GTK, but never changes the process's own `argv`: the C
runtime passes it to the `.init_array` functions of the runtime library as it
loads. A runtime reads its arguments with `laufey::args_os()`, which leaves the
argument out (the first one before any `--`, Linux only). Where a portal also
sends `ActionInvoked` for the click, the first of the two wins for five seconds.
`Open(uris)` on the running app is a second launch with those links (files as
paths) as its arguments, and `Activate` a second launch with none: what a
desktop sends when it launches an app whose desktop entry says
`DBusActivatable=true`. Any process of the same user on the session bus can call
these methods, as with the Windows COM activator.

Tested on Ubuntu 26.04 with GNOME 50 (xdg-desktop-portal 1.21,
xdg-desktop-portal-gnome 50) and Plasma 6.6 (xdg-desktop-portal-kde 6.6), with
the WebKitGTK and CEF backends: a click after the app quit starts it (the body
on both, a button on Plasma), and so does a click on a notification a systemd
timer posted while the app was closed (`scripts/notification-coldstart-e2e.sh`).
Neither desktop needs `DBusActivatable=true` in the desktop entry for that; the
service file is enough. gnome-shell refuses notifications for a desktop entry it
hasn't loaded yet (a few seconds after it is installed) while the portal still
reports success.

**`org.freedesktop.Notifications`** otherwise (an AppImage or a tarball, which
install no desktop entry; an older portal; no portal): `Notify` with the actions
plus `default` (the body click), the `desktop-entry`, `urgency` (critical for
`require_interaction`, which also never expires), `suppress-sound` and
`image-data` (the icon) hints, and the server's id of a notification with the
same tag as `replaces_id`. `ActionInvoked` and `NotificationClosed` become the
events.

Both follow the server's `GetCapabilities`, read once per server: without
`actions` no button (and no `default`) is sent, and `clicks()`, `actions()` and
`cold_start()` are false. The body is escaped, since it is text, unless the
server is known not to read markup (its capabilities were read and lack
`body-markup`). `platform_features` reports the transport, the cold start and
the scheduled-launch timers with the reason for each "no", and the server's
capabilities ([platform-features.md](platform-features.md)).

Everything runs on a thread of laufey's own (its own GLib main context and its
own session-bus connection), under WebKitGTK and CEF alike.

Permission: granted when a notification server owns the name (or D-Bus can start
one), else unsupported; there is no prompt.

Packagers: install `<app id>.desktop` in `/usr/share/applications` and
`/usr/share/dbus-1/services/<app id>.service`:

```ini
[D-BUS Service]
Name=<app id>
Exec=/usr/bin/env LAUFEY_APP_ID=<app id> /usr/bin/<app> --laufey-dbus-activated
```

and on removal stop the users' scheduled-notification timers, matching exactly
16 hex digits after the app id so that another app whose id starts with this
one's (`<app id>-extra`) keeps its timers:

```sh
timeout 10 systemctl --user --machine="$user"@ --no-block \
  stop 'laufey-<app id>-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f].timer' || :
```

for each user `loginctl list-users` knows (`NotificationTimerGlob` builds the
pattern; an app id over 200 characters is cut as in the unit names).
`--no-block` and the timeout keep a user manager that doesn't answer from
stalling the package manager.

### Winit

The Winit backend uses `notify-rust` and reports show, close, and a synthetic
shown event; it has no scheduling, responses or capabilities (API 41 entries are
`NULL`).

## Test hook

`test_notification_respond(tag, action)` (API 41) delivers a click through the
dispatch the OS response uses: the live callback when this process shows the
notification, else the response handler (or its buffer). See
[e2e-testing.md](e2e-testing.md).
