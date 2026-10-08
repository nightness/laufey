# Global shortcuts

A global shortcut is a key combination that reaches the app whichever app has
the keyboard focus, like a "show the quick-entry window" hotkey. laufey binds
them with the operating system's own hot-key API (API ≥ 40):

| Platform       | Mechanism                                                                                          |
| -------------- | -------------------------------------------------------------------------------------------------- |
| macOS          | Carbon `RegisterEventHotKey` (no Accessibility permission needed), presses on the main thread      |
| Windows        | `RegisterHotKey` on a message-only window of the backend's UI thread (`MOD_NOREPEAT`)              |
| Linux, X11     | passive key grabs (`XGrabKey`) on the root window over laufey's own XCB connection and thread      |
| Linux, Wayland | the XDG GlobalShortcuts portal (`org.freedesktop.portal.GlobalShortcuts`), when the desktop has it |
| Winit          | not supported (`NOT_SUPPORTED`)                                                                    |

The WebView and CEF backends of one OS share the implementation
(`backend-common/src/shortcuts*.cc`), so they behave the same.

## Rust

```rust
laufey::on_shortcut(|accel| {
  // accel is the canonical form, e.g. "Ctrl+Shift+K"
  println!("pressed {accel}");
});

match laufey::register_shortcut("CommandOrControl+Shift+K").await {
  Ok(canonical) => println!("bound {canonical}"),
  Err(laufey::ShortcutError::Conflict) => println!("another app has it"),
  Err(e) => println!("not bound: {e}"),
}

laufey::shortcuts();                              // ["Ctrl+Shift+K"]
laufey::canonical_accelerator("shift+ctrl+k");    // Some("Ctrl+Shift+K")
laufey::unregister_shortcut("Ctrl+Shift+K");      // any spelling
laufey::unregister_all_shortcuts();
```

`system_capabilities().global_shortcuts()` says whether registration can succeed
in this session; `shortcuts_user_binds()` is set where the user approves each
shortcut (the Wayland portal).

## Accelerator syntax

The menu accelerator syntax: `+`-separated, case-insensitive, any modifiers
first and exactly one key last.

- **Modifiers:** `CommandOrControl` / `CmdOrCtrl` (Command on macOS, Control
  elsewhere), `Control` / `Ctrl`, `Alt` / `Option`, `Shift`, `Super` / `Meta`
  (Command on macOS, the Windows key, Super on Linux), and `Command` / `Cmd`
  (macOS only; elsewhere it is an error, so a shortcut can't silently mean
  different keys).
- **Keys:** `A`-`Z`, `0`-`9`, the punctuation keys `` - = [ ] \ ; ' , . / ` ``,
  `F1`-`F24`, `Space`, `Tab`, `Backspace`, `Delete`, `Insert`, `Enter` /
  `Return`, `Escape` / `Esc`, `Up` / `Down` / `Left` / `Right`, `Home`, `End`,
  `PageUp`, `PageDown`, `PrintScreen`, `Num0`-`Num9`, `NumDecimal`, `NumAdd`,
  `NumSubtract`, `NumMultiply`, `NumDivide`, `MediaPlayPause`, `MediaNextTrack`,
  `MediaPreviousTrack`, `MediaStop`, `VolumeUp`, `VolumeDown`, `VolumeMute`.

Keys name physical keys, so shifted symbols are written with Shift (`Shift+=`,
not `Plus`). Letters, digits and punctuation follow the current keyboard layout
on macOS and Windows (`Ctrl+Z` is the key labelled Z on an AZERTY keyboard) and
the keyboard map on X11.

A key used while typing or moving around in a document (a letter, digit,
punctuation, Space, any numpad key, Enter, Tab, Backspace, Delete, Insert,
Escape, the arrows, Home, End, PageUp or PageDown) needs a modifier other than
Shift, so a global shortcut can never swallow or observe ordinary typing and
navigation in other apps; `K`, `Shift+K`, `Num5`, `Enter`, `Escape`, `Up` or
`Shift+PageDown` alone is `INVALID`. There is no option to allow one: the C ABI
has none, by design. Function, media and volume keys and PrintScreen may stand
alone (`F13`, `MediaPlayPause`).

Every combination has one **canonical** spelling, `Ctrl+Alt+Shift+Super+<Key>`
with only the modifiers present (`Ctrl+Shift+K`, `Super+Space`, `Alt+F4`,
`Ctrl+Num5`). Registration reports it, the press handler receives it and
`list_shortcuts` lists it; unregistering takes any spelling.

## The C ABI

```c
uint32_t (*system_capabilities)(void* backend_data);
void (*set_shortcut_handler)(void*, laufey_shortcut_fn, void* user_data);
void (*register_shortcut)(void*, const char* accelerator,
                          laufey_shortcut_result_fn callback, void* user_data);
bool (*unregister_shortcut)(void*, const char* accelerator);
void (*unregister_all_shortcuts)(void*);
char* (*list_shortcuts)(void*);   // '\n'-separated, string_free
char* (*canonicalize_accelerator)(void*, const char* accelerator);  // or NULL
bool (*test_trigger_shortcut)(void*, const char* accelerator);  // test hook
```

`register_shortcut` never blocks: its callback fires exactly once, synchronously
for answers that need no OS call (`INVALID`, `ALREADY_REGISTERED`,
`NOT_SUPPORTED`) and otherwise once the OS has answered. Statuses:

| Status                               | Meaning                                                                 |
| ------------------------------------ | ----------------------------------------------------------------------- |
| `LAUFEY_SHORTCUT_OK`                 | bound; the canonical accelerator is passed along                        |
| `LAUFEY_SHORTCUT_INVALID`            | doesn't parse, or a printable key without a real modifier               |
| `LAUFEY_SHORTCUT_CONFLICT`           | the OS refused it: another app (or code outside this API) holds it      |
| `LAUFEY_SHORTCUT_ALREADY_REGISTERED` | this app registered it already (any spelling)                           |
| `LAUFEY_SHORTCUT_NOT_SUPPORTED`      | no global shortcuts here (Winit; Wayland without the portal; see below) |
| `LAUFEY_SHORTCUT_DENIED`             | the user declined it in the portal's dialog                             |
| `LAUFEY_SHORTCUT_FAILED`             | any other OS failure, or more than `LAUFEY_MAX_SHORTCUTS` (256) at once |

The press handler fires on the backend UI thread. Unregistering stops the
handler at once; the OS binding is released right after. Every registration ends
with the process.

## How a conflict is detected

- **macOS:** hot keys are registered with `kEventHotKeyExclusive`; a combination
  another app registered comes back as `eventHotKeyExistsErr`. System shortcuts
  handled before the hot-key layer (Spotlight's Command-Space, Mission Control,
  and so on) are not reported as conflicts: macOS accepts the registration, and
  the system keeps the key.
- **Windows:** `RegisterHotKey` fails with `ERROR_HOTKEY_ALREADY_REGISTERED`
  when any other window or app holds the combination. Some keys the shell
  reserves (`Win+L`, `Ctrl+Alt+Delete`) fail as well.
- **X11:** a grab another client holds fails with `BadAccess`. laufey grabs
  every combination with and without Caps Lock and Num Lock, so either lock
  state works; if any of those grabs is refused, the others are released and the
  shortcut is a conflict. A key auto-repeat fires once, not per repeat.
- **Wayland portal:** the desktop resolves conflicts in its own dialog.

## Wayland

Wayland gives no client a way to grab keys, so laufey asks the XDG desktop
portal. On a Wayland display (`WAYLAND_DISPLAY` names a socket that exists;
`XDG_SESSION_TYPE` is not read, see [Backends](backends.md)) the backend probes
the portal's GlobalShortcuts interface once; when it answers,
`system_capabilities` reports both `GLOBAL_SHORTCUTS` and
`SHORTCUTS_USER_BINDS`.

- Each shortcut gets its own portal session; `BindShortcuts` passes the
  canonical accelerator as the shortcut's id and description and the XDG trigger
  form (`CTRL+SHIFT+k`) as the preferred trigger.
- The desktop may show its own dialog (GNOME does, the first time). The user may
  accept, pick a different trigger, or decline (`DENIED`). The press handler
  still receives the canonical accelerator that was registered, whatever trigger
  the user chose. A dialog nobody answers within 30 s
  (`LAUFEY_SHORTCUT_PROMPT_TIMEOUT_MS`) is closed and the registration answers
  `DENIED`, so a caller never waits on an empty desk. `CreateSession` gets 10 s
  (`LAUFEY_SHORTCUT_SESSION_TIMEOUT_MS`); past that the registration answers
  `DENIED` as well, and laufey closes the session by its predictable handle
  (`…/session/<sender>/<token>`) and any session a late `Response` still names,
  so none is left behind.
- With `LAUFEY_APP_ID` set, laufey registers the app with the portal's host
  registry (xdg-desktop-portal 1.19+) so the desktop can remember the user's
  choices per app. The portal ties an app id to a D-Bus connection the first
  time that connection talks to it, and GTK reads the Settings portal over the
  process's shared connection at startup, so the shortcut client uses a
  connection of its own: on the shared one the registration fails and
  xdg-desktop-portal 1.19+ refuses `CreateSession` ("An app id is required").
- **The OS limit:** without the portal, or with a portal backend that doesn't
  implement GlobalShortcuts (it needs xdg-desktop-portal 1.17+ with
  xdg-desktop-portal-kde, or GNOME 48+), registrations answer `NOT_SUPPORTED`.
  There is no fallback: an X11 grab through XWayland only sees keys while an
  XWayland window has the focus.

`LAUFEY_GLOBAL_SHORTCUTS=x11|portal|off` overrides the choice (for example `x11`
on a desktop whose XWayland allows global grabs).

## Testing

- `backend-common/tests/system_test.cc` (ctest): parsing, canonical forms, the
  registry's exactly-once results, `ALREADY_REGISTERED`, unregistering while the
  OS hasn't answered, dispatch and the test hook, over a fake OS side.
- `backend-common/tests/shortcuts_portal_test.cc` (ctest, Linux): the portal
  client against a mock portal on a private D-Bus bus: the probe, the host
  registration, `CreateSession` / `BindShortcuts` through Request objects,
  `Activated` reaching the handler, a declined binding, an unanswered dialog, a
  `CreateSession` that times out (`DENIED`, the session closed by its handle) or
  answers after the deadline (the late session closed), and `Session.Close` on
  unregister.
- `native_e2e --system` (every backend; see [e2e-testing.md](e2e-testing.md)):
  the real OS registration, a `CONFLICT` while a second process holds the same
  shortcut and `OK` once it let go, the test hook, and on Windows a real key
  press injected with `SendInput`.
