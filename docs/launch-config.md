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
  "inspectable": false
}
```

Every key is optional, and each key stands in for one environment variable:

| Key              | Environment variable     | Value                                                             |
| ---------------- | ------------------------ | ----------------------------------------------------------------- |
| `appId`          | `LAUFEY_APP_ID`          | `A-Z a-z 0-9 . _ -` only, not `.` or `..`                         |
| `customSchemes`  | `LAUFEY_CUSTOM_SCHEMES`  | array of URL scheme names (a letter, then letters, digits, `+-.`) |
| `dataDir`        | `LAUFEY_DATA_DIR`        | absolute path                                                     |
| `singleInstance` | `LAUFEY_SINGLE_INSTANCE` | `true` / `false` (the variable: `1` / `0`, or `true` / `false`)   |
| `inspectable`    | `LAUFEY_INSPECTABLE`     | `true` / `false` (the variable: `1` / `0`, or `true` / `false`)   |

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
- `inspectable: false`: `LAUFEY_INSPECTABLE=1` cannot turn DevTools back on (it
  is reported and ignored); `LAUFEY_INSPECTABLE=0` still turns them off.

The keys are resolved one at a time before they are combined. For example, a
file with `dataDir` launched with `LAUFEY_APP_ID` set still uses the file's
`dataDir`, because `LAUFEY_DATA_DIR` isn't set and a data dir outranks an app id
([App data](app-data.md)). CEF's `--laufey-custom-schemes` switch is added to
the list from `LAUFEY_CUSTOM_SCHEMES` or `customSchemes`.

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
  reported and ignored, and the file's value applies.
- A value of the wrong type, an empty string, a string containing NUL, or a
  value that breaks the rules above (an unsafe `appId`, a relative `dataDir`) is
  reported, and that key is ignored. It behaves as if it were absent.
- An invalid entry in `customSchemes` is reported and skipped. The other entries
  are kept.
- If a key appears more than once, the last one wins, and this is reported.

## Trust

The file belongs to the installed app. It sits inside the (possibly signed)
`.app` bundle or next to the executable in the install directory, is written by
the app's packager or installer, and is trusted like the executable itself.
laufey reads it only from that location, never from the working directory or
from a per-user location. Keep it where only the installer can write it, just
like the executable.
