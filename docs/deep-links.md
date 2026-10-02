# Deep links, opened files and single instance

A deep link is a custom URL scheme the OS routes to your app — clicking
`acme://open/document/42` in a browser, a mail client, or a terminal hands that
URL to the app that claims the `acme` scheme. Opening a file with the app
(Finder "Open With", a double click on a file type the app claims, a drop on the
Dock icon) works the same way. laufey delivers these to your runtime; it does
not register the scheme or the file types.

```rust
laufey::on_open_url(|url| {
  // "acme://open/document/42" or "file:///Users/me/notes.txt" (macOS)
  window.navigate(&route_for(url));
});

laufey::on_second_instance(|args, cwd| {
  // The app was started again while running (single-instance mode):
  // args = ["acme://open/document/42"], cwd = where it was started.
  handle_launch(args, cwd);
});
```

## Registering the scheme is the embedder's job

laufey [does not build application bundles](distribution.md), and scheme
registration lives entirely inside the bundle metadata, so it belongs to
whatever packages the app:

| Platform | Where the scheme is declared                                              |
| -------- | ------------------------------------------------------------------------- |
| macOS    | `CFBundleURLTypes` / `CFBundleURLSchemes` in the bundle `Info.plist`      |
| Linux    | `MimeType=x-scheme-handler/acme;` plus `Exec=… %u` in the `.desktop`      |
| Windows  | `HKCU\Software\Classes\acme` with `URL Protocol` and `shell\open\command` |

File types are declared in the same places: `CFBundleDocumentTypes` on macOS,
the file's MIME type in the `.desktop` `MimeType=` (with `%f`/`%U` in `Exec=`)
on Linux, and a ProgID with a `shell\open\command` of `"app.exe" -- "%1"` on
Windows.

**Register Windows commands with `--` before `"%1"`** (`"app.exe" -- "%1"`, for
URL schemes and file types alike). Windows substitutes the link for `%1` without
escaping it, so a link that contains a `"` can close the quotes and add
arguments of its own (the class of Electron's CVE-2018-1000006). Everything
after `--` is a positional argument: the hosts stop reading their own options
there, Chromium (CEF) treats it as the end of its switches, and the runtime
should do the same. As a second line of defence, a CEF launch whose positional
arguments include a URL drops every Chromium switch from its command line, and a
packaged app (one with a `laufey-launch.json` or a runtime next to its
executable) never takes `--runtime` from the command line.

The OS also has to have _seen_ that metadata: macOS registers schemes through
LaunchServices when the `.app` is installed (or after `lsregister -f`), Linux
needs the `.desktop` file in `~/.local/share/applications` and
`update-desktop-database`, and the Windows keys are normally written by an
installer.

## How a link or a file reaches the app

The platforms hand links and files over in different shapes. macOS delivers them
to the _already running_ app as an Apple Event (`application:openURLs:`),
launching it first if necessary, so one process sees every link. Windows and
Linux instead **start a new process** with the URL or path in `argv`, whether or
not the app is already running; macOS does the same when the binary inside the
bundle is run directly instead of through LaunchServices.

laufey does not parse `argv` into URLs or files. What is a link, a file or a
flag is the embedder's call, made the same way on every OS: at a cold start from
its own arguments, and in a running app from the arguments the
[single-instance lock](#single-instance) forwards.

| How the app is started                                                  | App not running                                                        | App already running                                                                                                                      |
| ----------------------------------------------------------------------- | ---------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| macOS, through LaunchServices (link click, `open`, Finder, Dock)        | the app starts; `on_open_url` gets the URL (buffered until registered) | `on_open_url` in the running app; no new process                                                                                         |
| macOS, binary exec'd directly (`App.app/Contents/MacOS/app "acme://…"`) | `argv`                                                                 | with `singleInstance`: `on_second_instance` in the running app, and the new process exits; without: a second instance, URL in its `argv` |
| Windows (`app.exe "acme://…"`), Linux (`app %u`)                        | `argv`                                                                 | with `singleInstance`: `on_second_instance` in the running app, and the new process exits; without: a second instance, URL in its `argv` |

**Reading `argv`.** The runtime is a shared library loaded into the backend
process, so it sees the backend's own process arguments: `std::env::args()` in a
Rust runtime, `Deno.args` / `process.argv` in a JavaScript one. They are the
arguments the OS or the launcher gave the backend executable, including any
backend options (such as `--runtime <path>`) that were on that command line, and
the `--` a Windows registration puts before the link; treat everything after a
`--` as positional. (On Linux the CEF backend hands Chromium a copy of `argv`:
Chromium sets the process title by rewriting the argument strings in place,
which would otherwise garble what the runtime reads.)

On macOS a file or URL on the command line of a directly exec'd binary is only
in `argv`. The WebView and CEF backends also turn off AppKit's
`NSTreatUnknownArgumentsAsOpen` default, which historically turned such
arguments into open-document events as well; macOS 15 didn't do that in our
tests either way, so this is a safeguard.

## Opened files on macOS

The WebView, CEF and Winit backends implement the application delegate's
`application:openURLs:`. AppKit calls that method for URLs and for files alike
(and then doesn't call `application:openFile:` / `application:openFiles:`), so a
file opened with the app reaches `on_open_url` as a `file://` URL, percent
encoded (`file:///Users/me/My%20Notes.txt`). Decode it with a URL parser before
using it as a path. The method is in place before `[NSApp run]` finishes
launching, so a file that starts the app is not missed: it is buffered like a
launch URL. laufey registers no Apple Event handler of its own, so each event is
delivered once.

## macOS: `on_open_url` needs LaunchServices

`on_open_url` fires on macOS only. On Windows and Linux it is a no-op, and
`set_open_url_handler` is `NULL` on those backends, so an embedder can detect
the absence rather than register a handler that silently never fires. There,
links and files arrive through `argv` and `on_second_instance`, as above.

## Single instance

Single-instance mode is off by default. Turn it on in the app's
[launch file](launch-config.md), together with an app id:

```json
{ "appId": "com.example.acme", "singleInstance": true }
```

or with `LAUFEY_SINGLE_INSTANCE=1` (and `LAUFEY_APP_ID`) in the environment;
`LAUFEY_SINGLE_INSTANCE=0` turns it off even when the file turns it on. Without
an app id the backend prints a warning and runs unlocked. The backend decides
this in `main()`, before it starts a web engine or loads the runtime, so it
works the same when the app is started by the OS, a shortcut or `exec`.

With it on:

- The first process of the app becomes the **primary**: it takes a lock for this
  user and this app id and listens for later launches.
- A later launch finds the lock taken, sends its arguments (everything after the
  executable name) and working directory to the primary, waits for the primary
  to acknowledge them, and **exits 0**, before any window, web engine or runtime
  starts. If the primary does not answer within 10 seconds, it prints an error
  and exits 1. On Windows it first calls `AllowSetForegroundWindow(ASFW_ANY)`,
  so the primary may take the focus.
- The primary brings the app to the front (it restores and activates its focused
  or first visible window; windows the app hid stay hidden), then calls the
  `on_second_instance` handler on the UI thread with the arguments and the
  directory, like Electron's `second-instance` event.

Launches forwarded before the runtime registers a handler are buffered, up to
`LAUFEY_MAX_PENDING_SECOND_INSTANCES` (16), and delivered when it does, just
like [cold-start URLs](#cold-start-is-buffered-not-dropped).

| Backend            | Single instance                                                      |
| ------------------ | -------------------------------------------------------------------- |
| WebView (all OSes) | yes                                                                  |
| CEF (all OSes)     | yes                                                                  |
| Winit              | no: it reads no launch file; `set_second_instance_handler` is `NULL` |
| iOS                | no (the OS keeps one instance)                                       |

### CEF

A CEF app with a persistent profile (an app id or data dir) already allows only
one process per profile: without single-instance mode, a second launch exits
during `CefInitialize` and the running instance ignores it
([App data](app-data.md#one-instance-per-profile-cef)). With single-instance
mode the second launch forwards and exits in `main()` first, so CEF's own check
is never reached. Two CEF instances run side by side only with different data
directories.

### The lock

| OS      | Lock and channel                                                                                                                                                                                                                                                                                                      |
| ------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Linux   | `flock()` on `laufey-si-<hash>.lock` and a Unix domain socket `laufey-si-<hash>.sock` (mode `0600`) in `$XDG_RUNTIME_DIR`, or in `/tmp/laufey-<uid>` (created `0700`) if that is unset or unsafe. The directory must be owned by the user and closed to everyone else. The server checks the peer with `SO_PEERCRED`. |
| macOS   | The same, in the per-user temporary directory (`confstr(_CS_DARWIN_USER_TEMP_DIR)`, what `$TMPDIR` points to); the server checks the peer with `getpeereid()`.                                                                                                                                                        |
| Windows | A named pipe `\\.\pipe\laufey-si-<hash>-<user SID>-<session id>` created with `FILE_FLAG_FIRST_PIPE_INSTANCE` (only one process can create it) and `PIPE_REJECT_REMOTE_CLIENTS`, with a DACL that grants access to the current user only. The client checks that the pipe's server process runs as the same user.     |

`<hash>` is 16 hex digits of the app id's FNV-1a hash, which keeps socket paths
short. The lock belongs to the process: the OS releases it when the primary
exits or crashes, and the next launch becomes the primary (a leftover socket
file is replaced). The descriptors are close-on-exec, so child processes the
runtime starts do not keep the lock. Two first launches at the same moment end
with exactly one primary.

The lock is per user, per app id and, on Windows, per session. On Linux, a
process started without `XDG_RUNTIME_DIR` (from some service managers or
`sudo -u`) uses the `/tmp` fallback and does not see a primary that uses
`$XDG_RUNTIME_DIR`.

On macOS, LaunchServices normally sends a link or file to the running app
instead of starting another one. If it does start a new process anyway
(`open
-n`, or a second copy of the app at another path), that process forwards
its `argv` and exits before `[NSApp run]`, so a URL or file delivered to it as
an Apple Event is not forwarded.

On Wayland, a compositor may refuse to give a window focus without an activation
token from the launching app; the primary still presents its window, but it may
only be marked as wanting attention.

### The message

Arguments and the working directory are sent as UTF-8 (on Linux and macOS, bytes
that are not valid UTF-8 are replaced with U+FFFD; on Windows they come from
`GetCommandLineW`), length-prefixed. The primary reads at most 1 MiB, at most
4096 arguments, one client at a time, and gives each client 5 seconds. It
rejects a message that is truncated, has trailing bytes, exceeds those limits,
or contains invalid UTF-8 or NUL, and delivers nothing from it.

## Security

**Treat everything you receive here as untrusted input.** Any program running as
the same user can start your app with any arguments, open any URL with it, or
connect to the lock and send any arguments and working directory. Check a URL's
scheme against the one you registered, validate the rest before routing on it,
and don't treat a path in the arguments as something the user chose.

What the lock does:

- Another user cannot connect: the socket's directory and the socket are owner
  only and the server checks the peer's user id; the pipe's DACL admits only the
  current user and rejects network clients.
- Your arguments don't go to another user's process: on Windows, a pipe served
  by another user is refused (the app then prints a warning and runs unlocked);
  on Linux and macOS the socket lives in a directory only you can write to.
- A malformed or oversized message is rejected without being delivered.

What it does not do:

- It is not a boundary between programs of the same user. They can send you
  arbitrary arguments, or take the lock first and keep your app from starting as
  the primary (on Windows another user can also create the pipe name first; your
  app then runs without the lock).
- It does not authenticate what the arguments mean.

## Cold start is buffered, not dropped

A launch URL — the case where clicking the link _starts_ the app — arrives while
the runtime is still coming up. The backend loads the runtime shared library on
a worker thread, so at the moment AppKit dispatches the Apple Event there is no
handler to call yet.

Rather than lose it, the backend buffers URLs while no handler is registered and
flushes them, in order, the instant `on_open_url` is called. Registering a
handler at startup is therefore enough to catch both cases:

```rust
// Fires for a link clicked five minutes from now, and for the link that
// launched the app a moment ago.
laufey::on_open_url(|url| handle(url));
```

Up to `LAUFEY_MAX_PENDING_OPEN_URLS` (16) URLs are held; beyond that the oldest
are discarded, since the newest link is the one the user is waiting on.
Forwarded second-instance launches are buffered the same way.

## Threading

The callbacks run on the backend's UI thread, like every other event handler —
with one exception: URLs and launches replayed from the buffer run on the thread
that called `on_open_url` / `on_second_instance`, because the flush happens
inside that call.

## Validate the URL

The URL is passed through exactly as the OS delivered it, with no filtering.
Anything on the machine can invoke your app with an arbitrary URL, so treat it
as untrusted input: check the scheme against the one you registered, and don't
route on it without validating the rest.

```rust
laufey::on_open_url(|url| {
  let Some(rest) = url.strip_prefix("acme://") else {
    return; // not ours
  };
  route(rest);
});
```

## Testing

Deep links need OS-level registration to exercise for real, which automated
tests can't rely on. The `test_trigger_open_url` hook (API ≥ 35) synthesizes a
delivery through the same dispatch path — buffer included, so a call made before
any handler is registered is replayed on registration exactly like a cold start:

```rust
assert!(!laufey::test_trigger_open_url("acme://cold")); // buffered
laufey::on_open_url(|url| { /* receives "acme://cold" immediately */ });
assert!(laufey::test_trigger_open_url("acme://live")); // delivered
```

See [`docs/e2e-testing.md`](e2e-testing.md) and the probe in
`examples/native_e2e`. For an end-to-end check, build a bundle with a scheme in
its `Info.plist`, register it with `lsregister -f`, and `open 'acme://…'` with
the app both running and closed.

`scripts/single-instance-e2e-run.sh <webview|cef>` drives real processes: the
runtime reading its cold-start arguments, a second launch forwarded to the first
(and exiting 0 without a display on Linux), two unlocked instances side by side,
and on macOS files opened with the bundle through `open -a` at a cold start and
while running. The lock itself (framing, limits, UTF-8, naming, stale sockets,
simultaneous first launches) is unit-tested in
`backend-common/tests/single_instance_test.cc`.
