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

| Platform | Scheduler                                                                                                                                                  | Delivered while the app is not running |
| -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------- |
| macOS    | `UNTimeIntervalNotificationTrigger` (under a minute away) or `UNCalendarNotificationTrigger` (the wall-clock date, so a sleep in between doesn't shift it) | yes (the system holds it)              |
| Windows  | `ScheduledToastNotification`                                                                                                                               | yes (the system holds it)              |
| Linux    | laufey's own timer: the freedesktop protocol has no scheduler                                                                                              | no — see below                         |

On Linux the schedule is persisted in the app data directory
(`laufey-notifications.json`, see [app-data.md](app-data.md)) and re-armed at
the next launch, where a notification whose time passed while the app wasn't
running is delivered at once. Without an app data directory (no `LAUFEY_APP_ID`
or `LAUFEY_DATA_DIR`) the schedule lives only as long as the process. Several
instances share the file under a lock and a due notification is claimed from it
before it is posted, so it fires once. With no notification server on the
session bus (yet), a due notification waits and is retried every 30 seconds.

`notification_capabilities()` reports `schedule()` and `schedule_persists()`.

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

| Platform | Mechanism                                                                                                              | Cold-start click |
| -------- | ---------------------------------------------------------------------------------------------------------------------- | ---------------- |
| macOS    | `UNUserNotificationCenter` delegate, installed before AppKit finishes launching; actions are `UNNotificationCategory`s | yes              |
| Windows  | toast activation through the app's COM activator (`INotificationActivationCallback`), registered at launch             | yes              |
| Linux    | `org.freedesktop.Notifications` `ActionInvoked` (the body click is the `default` action)                               | no — see below   |

On Linux the server sends `ActionInvoked` to the connection that posted the
notification, so a click after that process exited can't reach the app. The
notification carries the `desktop-entry` hint (`LAUFEY_APP_ID`), which lets a
shell such as GNOME launch the app for it, but the click itself is not
delivered.

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

The freedesktop.org Desktop Notifications protocol over D-Bus
(`org.freedesktop.Notifications`): `Notify` with the actions plus `default` (the
body click), the `desktop-entry`, `urgency` (critical for `require_interaction`,
which also never expires), `suppress-sound` and `image-data` (the icon) hints,
and the server's id of a notification with the same tag as `replaces_id`.
`ActionInvoked` and `NotificationClosed` become the events. Everything runs on a
thread of laufey's own (its own GLib main context), under WebKitGTK and CEF
alike.

Permission: granted when a notification server owns the name (or D-Bus can start
one), else unsupported; there is no prompt.

### Winit

The Winit backend uses `notify-rust` and reports show, close, and a synthetic
shown event; it has no scheduling, responses or capabilities (API 41 entries are
`NULL`).

## Test hook

`test_notification_respond(tag, action)` (API 41) delivers a click through the
dispatch the OS response uses: the live callback when this process shows the
notification, else the response handler (or its buffer). See
[e2e-testing.md](e2e-testing.md).
