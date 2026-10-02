# Launch at login

An app can ask to be started when the user logs in (API ≥ 40). laufey uses each
OS's per-user mechanism; none needs administrator rights.

| Platform | Mechanism                                                                                            |
| -------- | ---------------------------------------------------------------------------------------------------- |
| macOS    | `SMAppService.mainAppService` (macOS 13+): the app bundle itself becomes a login item                |
| Windows  | a value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` with the executable's quoted path |
| Linux    | an XDG autostart entry, `$XDG_CONFIG_HOME/autostart/<name>.desktop` (`~/.config/autostart`)          |
| Winit    | not supported                                                                                        |

The entry is named after the app id: `LAUFEY_APP_ID`, or `"appId"` in
[`laufey-launch.json`](launch-config.md); without one, the executable's file
name. Give a packaged app an id so the entry is stable and two apps never share
one.

## Rust

```rust
use laufey::LoginItemState;

match laufey::set_launch_at_login(true) {
  Ok(LoginItemState::Enabled) => {}
  Ok(LoginItemState::RequiresApproval) => {
    // Registered; the user must allow it in the system settings.
  }
  Ok(LoginItemState::NotSupported) => {}
  Ok(LoginItemState::Disabled) => {}
  Err(message) => eprintln!("{message}"),
}
let state = laufey::launch_at_login();
```

## The C ABI

```c
int (*get_launch_at_login)(void* backend_data);
int (*set_launch_at_login)(void* backend_data, bool enabled, char** error_out);
```

Both may be called from any thread and return a `LAUFEY_LOGIN_ITEM_*` state:

| State               | Meaning                                                                          |
| ------------------- | -------------------------------------------------------------------------------- |
| `DISABLED`          | the app does not start at login                                                  |
| `ENABLED`           | it does                                                                          |
| `REQUIRES_APPROVAL` | registered, but the user has to allow it in the system settings first            |
| `NOT_SUPPORTED`     | not on this backend or OS version                                                |
| `FAILED`            | `set_launch_at_login` only; a message in `*error_out` (freed with `string_free`) |

`LAUFEY_SYSTEM_CAP_LAUNCH_AT_LOGIN` in `system_capabilities` says whether it
works here.

## Per platform

**macOS.** `SMAppService` registers the running app bundle; macOS shows the user
a "Login Item Added" notification and lists the app in System Settings > General

> Login Items. If the user has turned the app off there (or macOS wants them to
> confirm it), the state is `REQUIRES_APPROVAL`, and only the user can change
> that; calling `set_launch_at_login(true)` again does not. It needs macOS 13
> and an app bundle (an executable run outside a `.app`, with no bundle
> identifier, is `NOT_SUPPORTED`). Before macOS 13 the only APIs left are a
> helper login item or the deprecated shared file lists, so it is
> `NOT_SUPPORTED` there. The bundle should be signed, as for any distributed
> app; macOS ties the login item to the bundle's code signature.

**Windows.** The `Run` value holds `"<path of the executable>"`. A value with
the same name that starts a different executable (another install) is reported
as `DISABLED` and replaced when the app enables launch at login; disabling
leaves such a value alone. The user can turn startup apps off in Task Manager or
Settings > Apps > Startup without deleting the value; Explorer records that
under `...\Explorer\StartupApproved\Run`. laufey reports such an entry as
`REQUIRES_APPROVAL` and does not override the user's choice.

**Linux.** The entry follows the XDG Autostart specification, which GNOME, KDE
Plasma, Xfce, Cinnamon, MATE and LXQt honor:

```ini
[Desktop Entry]
Type=Application
Version=1.0
Name=<LAUFEY_APP_NAME or the app id>
Exec="<path of the executable>"
Terminal=false
X-GNOME-Autostart-enabled=true
```

`Exec` is quoted and escaped per the Desktop Entry specification. For an
AppImage it is `$APPIMAGE` (the mounted executable disappears when the AppImage
exits). The file is written through a temporary file and `rename(2)`. An entry
turned off with `Hidden=true` or `X-GNOME-Autostart-enabled=false` reads as
`DISABLED`; enabling rewrites it. Desktops without XDG autostart (a bare window
manager) ignore the file; laufey cannot detect that.

The executable that starts at login is the backend host. A packaged app that
relies on environment variables from a launcher should put its settings in
[`laufey-launch.json`](launch-config.md) so a direct start works.

## Testing

`native_e2e --system` turns launch at login on and off and reads the state back,
checks the OS artefact (the `Run` value on Windows, the autostart file on
Linux), and restores the original state. It only runs with `CI` set or
`LAUFEY_E2E_LOGIN_ITEM=1`, so a developer's machine is left alone. The autostart
entry format and `Exec` quoting are unit-tested in
`backend-common/tests/system_test.cc`.
