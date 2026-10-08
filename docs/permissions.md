# Permissions

laufey lets you query or request the operating system's authorization for a
capability. The only capability today is notifications. The status set mirrors
the Web Permissions API: granted, denied, prompt, and unsupported.

```rust
use laufey::{PermissionKind, PermissionStatus};

laufey::request_permission(PermissionKind::Notifications, |status| {
  if status == PermissionStatus::Granted {
    // The capability is authorized.
  }
});
```

`query_permission` reads the current status without prompting the user.
`request_permission` shows the system prompt only when the status is prompt;
once the user has decided, the operating system returns the cached decision
rather than prompting again. Both callbacks run on the user-interface thread.

On macOS all backends route through `UNUserNotificationCenter`. A process that
is not bundled — one with no `CFBundleIdentifier`, or a binary that does not
live inside an `.app` — reports unsupported rather than denied, so that an
embedder can distinguish "the user declined" from "this environment cannot be
authorized at all." An application that packages its own `.app` sets its own
bundle identifier and entitlements; laufey hard-codes none of its own.
`PermissionKind::NotificationsProvisional` (API 41; request only) asks for
quiet, "provisional" authorization, which macOS grants without a prompt: the
notifications go to the Notification Center without a banner or sound until the
user keeps them. Elsewhere it is the same as `Notifications`.

Windows and Linux have no prompt (API 41). Windows reports the user's setting
for the app's toasts (`ToastNotifier.Setting`): granted when enabled, denied
otherwise. Linux reports granted when a notification server owns
`org.freedesktop.Notifications` on the session bus (or D-Bus can start one), and
unsupported otherwise. `request_permission` answers the same as
`query_permission` there. See [notifications.md](notifications.md).
