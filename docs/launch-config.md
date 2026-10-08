# Launch configuration

Some settings have to be known when the backend process starts, before the
runtime library is loaded. Examples are the Wayland `app_id` that WebKitGTK sets
in `main()` and the settings CEF reads while it initializes, in every process.
An embedder can pass these as environment variables, but a packaged app is often
started directly by Explorer, a Start menu or desktop shortcut, the Dock, or
`exec`. Nothing sets environment variables in that case. Such an app can instead
ship the settings in a JSON file next to the executable.

## Where the file goes

| Platform        | Path                                               |
| --------------- | -------------------------------------------------- |
| Windows, Linux  | `<directory of the executable>/laufey-launch.json` |
| macOS (bundled) | `<App>.app/Contents/Resources/laufey-launch.json`  |
| macOS (bare)    | `<directory of the executable>/laufey-launch.json` |

The directory comes from the running executable's real path
(`GetModuleFileNameW`, `/proc/self/exe`, `_NSGetExecutablePath` plus
`realpath`). It never comes from the working directory. CEF's helper apps
(`<App>.app/Contents/Frameworks/<App> Helper*.app`) read the main app's file.

## Schema

```json
{
  "appId": "com.example.myapp",
  "customSchemes": ["myapp"],
  "dataDir": "/absolute/path",
  "singleInstance": true,
  "inspectable": false,
  "requireSandbox": true,
  "bridgeOrigins": ["myapp://app"],
  "passkeyRpIds": ["example.com"]
}
```

Every key is optional, and each key but `passkeyRpIds` and `bridgeOrigins`
stands in for one environment variable:

| Key              | Environment variable     | Value                                                             |
| ---------------- | ------------------------ | ----------------------------------------------------------------- |
| `appId`          | `LAUFEY_APP_ID`          | `A-Z a-z 0-9 . _ -` only, not `.` or `..`                         |
| `customSchemes`  | `LAUFEY_CUSTOM_SCHEMES`  | array of URL scheme names (a letter, then letters, digits, `+-.`) |
| `dataDir`        | `LAUFEY_DATA_DIR`        | absolute path                                                     |
| `singleInstance` | `LAUFEY_SINGLE_INSTANCE` | `true` / `false` (the variable: `1` / `0`, or `true` / `false`)   |
| `inspectable`    | `LAUFEY_INSPECTABLE`     | `true` / `false` (the variable: `1` / `0`, or `true` / `false`)   |
| `requireSandbox` | `LAUFEY_REQUIRE_SANDBOX` | `true` / `false` (the variable: `1` / `0`, or `true` / `false`)   |
| `bridgeOrigins`  | none (file only)         | array of origins, `<scheme>://*` or `*` (see below)               |
| `passkeyRpIds`   | none (file only)         | array of RP IDs: domain names, no scheme, port or path            |

Values follow the same rules as the environment variables, and each key does
what its variable does:

- `appId` selects the per-app data directory ([App data](app-data.md)) on every
  backend. On Linux it also sets the Wayland `app_id` and X11 `WM_CLASS`
  (WebView and CEF).
- `dataDir` overrides that directory ([App data](app-data.md)).
- `customSchemes` declares the CEF backend's custom schemes at startup
  ([Custom URL schemes](custom-schemes.md)). CEF's helper processes read the
  same file, and the browser process also forwards the list to them. The WebView
  backends learn their schemes from `register_scheme_handler` and don't need
  this key.
- `singleInstance` turns on the single-instance lock: a second launch of the app
  forwards its arguments to the running one and exits
  ([Deep links, opened files and single instance](deep-links.md#single-instance)).
  It needs an app id (`appId` or `LAUFEY_APP_ID`); without one the backend warns
  and runs unlocked. The WebView and CEF backends read it; the Winit backend
  does not.
- `inspectable` (default `true`) set to `false` turns the web engine's DevTools
  off for the process: no inspector, no Inspect menu item, no DevTools shortcut,
  no remote debugging, and `open_devtools` does nothing
  ([DevTools](devtools.md)). Release builds of an app usually ship it `false`.
  The WebView and CEF backends read it.
- `requireSandbox` (default `false`) set to `true` makes the Linux CEF host
  refuse to start (exit status 78, one line on stderr) where it would run web
  content without the Chromium sandbox: no user namespaces and no usable
  `chrome-sandbox` helper, as for a tarball or an AppImage on Ubuntu 23.10 and
  later ([Backends](backends.md), "The Chromium sandbox"). Off, the app starts
  unsandboxed there and reports `"sandbox": "off"`
  ([Platform features](platform-features.md)). Other backends and platforms
  ignore it.
- `passkeyRpIds` lists the relying parties the app's native
  [passkey](passkeys.md) ceremonies may name. With the key, a request whose RP
  ID (`rp.id` for a registration, `rpId` for an authentication) is not in the
  list is refused with `invalid_rp` before any OS UI shows; names match without
  regard to case, and an empty list refuses every request. Without the key any
  RP ID goes to the OS, as before. It has no environment variable: only the
  installed app decides. The macOS and Windows backends (WebView and CEF) read
  it.

- `bridgeOrigins` (API 44) pins the documents the JavaScript bridge serves
  ([JavaScript interop](javascript-interop.md#which-documents-can-call)). Each
  entry is an origin (`"myapp://app"`, `"https://example.com:8443"`; nothing
  after the host and port but an optional `/`), `"<scheme>://*"` (every origin
  of that scheme), or `"*"` (every origin: no pin). A document on any other
  origin gets no bridge namespace, and the backend rejects a call from one
  before it reaches the runtime. Without the key, a file with `customSchemes`
  pins the bridge to those schemes (`"<scheme>://*"` each); a file with neither,
  or no file, leaves every origin free to call. There is no environment
  variable: only the installed app can set it. An invalid entry is reported and
  skipped, but the key still pins: an empty list (or one with no valid entry)
  gives the bridge to no document. The WebView and CEF backends read it (CEF's
  renderers too).

## Precedence

For each key separately, an environment variable that is set to a non-empty
value wins over the file. So a launcher can still force a value, and setups that
rely on the environment keep working unchanged. A key absent from the file and
from the environment is simply unset. The file is read once per process.

The exceptions are the keys that isolate or lock down the installed app; for
them the shipped file wins:

- `appId` and `dataDir`: when the file has the key, `LAUFEY_APP_ID` /
  `LAUFEY_DATA_DIR` are ignored. Environment variables are inherited, so an app
  started by another laufey app would otherwise open that app's profile and take
  its single-instance lock.
- A file with `appId` pins the rest of the app's identity with it. The data
  directory is the file's `dataDir`, or else the app id's default directory;
  `LAUFEY_DATA_DIR` is ignored even when the file has no `dataDir`. The custom
  schemes are the file's `customSchemes` only (none without the key):
  `LAUFEY_CUSTOM_SCHEMES` and CEF's `--laufey-custom-schemes` switch are
  ignored. Single-instance mode is the file's `singleInstance` (off without the
  key): `LAUFEY_SINGLE_INSTANCE` is ignored. Each ignored variable (and the
  switch) is reported on stderr once. Otherwise a variable inherited from
  another program, or a switch on a command line a link can add to, could move
  the app's profile, give a scheme of its choosing a secure origin, or turn off
  the lock that routes a second launch to the running instance.
- `inspectable: false`: `LAUFEY_INSPECTABLE=1` cannot turn DevTools back on (it
  is reported and ignored); `LAUFEY_INSPECTABLE=0` still turns them off.
- `requireSandbox: true`: `LAUFEY_REQUIRE_SANDBOX=0` cannot let the app start
  unsandboxed (it is reported and ignored); `LAUFEY_REQUIRE_SANDBOX=1` still
  requires it when the file is silent or says `false`.

Without `appId` in the file, the keys are resolved one at a time before they are
combined. For example, a file with `dataDir` launched with `LAUFEY_APP_ID` set
still uses the file's `dataDir`, because a data dir outranks an app id
([App data](app-data.md)). CEF's `--laufey-custom-schemes` switch is then added
to the list from `LAUFEY_CUSTOM_SCHEMES` or `customSchemes`.

## Runtime library

A launch file also marks the app as packaged. A packaged app (one with a
`laufey-launch.json`, or with a runtime library it ships) loads only that
library: the one next to its executable (`<executable name>.so`, `.dll` or
`.dylib`; `<executable name>.runtime.dll` for the Windows CEF backend, whose
`<executable name>.dll` is the host behind CEF's bootstrap, see
[Backends](backends.md#the-chromium-sandbox)), or, on the macOS WebView backend,
`Contents/Frameworks/libruntime.dylib` or `Contents/MacOS/libruntime.dylib` in
its bundle. It ignores `--runtime` and `LAUFEY_RUNTIME_PATH` (both reported on
stderr) and never searches the working directory or system directories such as
`/usr/lib/laufey`. A packaged app without a shipped runtime reports that and
loads none. Only a development host, with neither a launch file nor a shipped
runtime, takes `--runtime <path>`, then `LAUFEY_RUNTIME_PATH`, then its
development fallbacks.

## Validation

A problem with the file never stops the app. It is reported on stderr as
`laufey: <path>: <message>` and skipped:

- If there is no file, nothing happens. This is not an error.
- If the JSON is malformed, or the top level is not an object, the whole file is
  ignored.
- A file larger than 64 KiB is ignored.
- An unknown key is reported and ignored, so newer files keep working with older
  hosts.
- `LAUFEY_SINGLE_INSTANCE` set to anything but `1`, `0`, `true` or `false` is
  reported and ignored, and the file's value applies. With an `appId` in the
  file, any value is ignored (see [Precedence](#precedence)).
- A value of the wrong type, an empty string, a string containing NUL, or a
  value that breaks the rules above (an unsafe `appId`, a relative `dataDir`) is
  reported, and that key is ignored. It behaves as if it were absent.
- An invalid entry in `customSchemes` (including a reserved scheme such as
  `https` or `file`, see [Custom URL schemes](custom-schemes.md)) or in
  `passkeyRpIds` is reported and skipped. The other entries are kept. A
  `passkeyRpIds` entry is lowercased first, then held to the passkey parser's RP
  ID rule (a domain name in LDH labels, not an IP address).
- If a key appears more than once, the last one wins, and this is reported.

## Trust

The file belongs to the installed app. It sits inside the (possibly signed)
`.app` bundle or next to the executable in the install directory, is written by
the app's packager or installer, and is trusted like the executable itself.
laufey reads it only from that location, never from the working directory or
from a per-user location. Keep it where only the installer can write it, just
like the executable.
