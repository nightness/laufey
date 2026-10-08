# DevTools

Every web engine laufey hosts has a web inspector. A runtime can open, close and
toggle it per window, and an app can turn it off entirely for release builds
(API ≥ 40; `open_devtools` itself is older).

## Rust

```rust
win.open_devtools();
win.close_devtools();
win.toggle_devtools();
let open = win.is_devtools_open();
let allowed = win.is_devtools_enabled(); // read back from the engine
let allowed_here = laufey::devtools_enabled(); // the launch setting
```

## The C ABI

```c
void (*open_devtools)(void* backend_data, uint32_t window_id);
void (*close_devtools)(void* backend_data, uint32_t window_id);    // API 40
bool (*is_devtools_open)(void* backend_data, uint32_t window_id);  // API 40
bool (*is_devtools_enabled)(void* backend_data, uint32_t window_id);  // API 40
```

All of them may be called from any thread. `is_devtools_enabled` with a window
id reads the window's engine setting back from the engine; with `0` it returns
the launch setting. `LAUFEY_SYSTEM_CAP_DEVTOOLS` in `system_capabilities` says
whether the backend has DevTools at all (not on Winit, which has no web engine).

## Turning DevTools off

DevTools are on by default. An app turns them off for the whole process with the
`LAUFEY_INSPECTABLE` environment variable or the `"inspectable"` key of
[`laufey-launch.json`](launch-config.md):

```json
{ "appId": "com.example.app", "inspectable": false }
```

`LAUFEY_INSPECTABLE=0` (or `false`) turns them off, `1` (or `true`) on, unless
the launch file says `"inspectable": false`: a shipped "off" can't be turned
back on from the environment (the variable is reported and ignored). The setting
is read once, at startup, because the engines take it when a web view is
created. Off means:

| Engine    | What laufey sets                                                                                                                                                                                                                                                                                                                                                                                   |
| --------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| WKWebView | `inspectable = NO` (macOS 13.3+: Safari's Develop menu and the context menu's Inspect Element can't reach the page) and the `developerExtrasEnabled` preference off (before 13.3)                                                                                                                                                                                                                  |
| WebView2  | `ICoreWebView2Settings::AreDevToolsEnabled = FALSE`: F12, Ctrl+Shift+I, the context menu's Inspect and `OpenDevToolsWindow` all stop working                                                                                                                                                                                                                                                       |
| WebKitGTK | `enable-developer-extras` off: no inspector, no Inspect Element                                                                                                                                                                                                                                                                                                                                    |
| CEF       | the remote-debugging switches (`--remote-debugging-port`, `-pipe`, `-address`, `-io-pipes`, `--auto-open-devtools-for-tabs`) are stripped from the browser process and `LAUFEY_REMOTE_DEBUGGING_PORT` is ignored; Chrome's DevTools commands (F12, Ctrl/Cmd+Shift+I / J / C, Cmd+Option+I / J / C) are swallowed in `CefCommandHandler::OnChromeCommand`; the context menu loses its Inspect items |

In every backend `open_devtools` is then a no-op, and `is_devtools_enabled`
reports `false`.

WebView2 also reads environment variables of its own that would reopen what
"off" closes. A packaged app (one with a `laufey-launch.json` or a runtime next
to its executable) with DevTools off removes them from its environment, with a
warning, before it creates the WebView2 environment:
`WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` (extra Chromium switches such as a
remote-debugging port), `WEBVIEW2_BROWSER_EXECUTABLE_FOLDER` (another browser
binary), `WEBVIEW2_USER_DATA_FOLDER` (another profile),
`WEBVIEW2_PIPE_FOR_SCRIPT_DEBUGGER` and `WEBVIEW2_WAIT_FOR_SCRIPT_DEBUGGER`.
`WEBVIEW2_RELEASE_CHANNEL_PREFERENCE` and `WEBVIEW2_CHANNEL_SEARCH_KIND` are
kept: they only choose among the signed WebView2 runtimes installed. A
development launch, or an app with DevTools on, honours them all. The equivalent
group policies (`HKLM`/`HKCU\Software\Policies\Microsoft\Edge\WebView2`) are an
administrator's to set and are not touched.

CEF note: Chromium's `devtools.availability` preference is deliberately not
used. It would also refuse the in-process DevTools-protocol client that
`print_to_pdf` uses (`ExecuteDevToolsMethod` / `Page.printToPDF`), so PDFs would
stop working. `native_e2e --devtools-off` checks that they still do.

## Per engine

- **WKWebView:** the inspector is WebKit's `_WKInspector` (what Safari's Develop
  menu drives): `show`, `close` and `isVisible`. It is a private API on every
  macOS version laufey supports. Safari's Develop menu also lists inspectable
  web views from outside the app. As on WebKitGTK (below), `show` opens the
  inspector only once the page's web process answers, so a `close` while it is
  connected but not yet visible is remembered (for 10 s): it reads as closed
  meanwhile and is closed again once it has opened.
- **WebView2:** the DevTools open in a separate top-level window of the browser
  process, and WebView2 has no API to close them or ask whether they are open.
  laufey remembers the DevTools window `open_devtools` brought up (a new
  "DevTools" window of the browser process, or the existing one coming to the
  front), closes it with `WM_CLOSE` (what its close button does), and reports it
  open while that window exists. DevTools the user opened with F12 or the
  context menu are found the same way when the app has a single window; with
  several windows sharing a browser process they can't be attributed to a
  window, and `is_devtools_open` reports only those opened through the API. The
  window appears a moment after `OpenDevToolsWindow`; a `close_devtools` before
  that closes it as soon as it is found (and the DevTools read as closed
  meanwhile), instead of finding nothing to close.
- **WebKitGTK:** `webkit_web_inspector_show` / `_close`; a window's inspector is
  open while it has an inspector web view. `show` makes that web view at once
  but opens the inspector only when the page's web process answers, and that
  answer arrives even after a `close`: DevTools closed while still opening came
  back on their own. laufey remembers such a close (for 10 s), reports the
  inspector closed meanwhile, and closes it again once it has opened.
- **CEF:** `CefBrowserHost::ShowDevTools`, `CloseDevTools` and `HasDevTools`. On
  Windows the DevTools open in a popup window.

## Testing

`native_e2e --system` checks, on every backend, that DevTools are enabled (the
launch setting and the engine's own setting) and that open, close and two
toggles each change `is_devtools_open`, and that DevTools closed right after
`open_devtools` (before they finished opening) stay closed.
`native_e2e --devtools-off` runs under `LAUFEY_INSPECTABLE=0` and checks the
engine reports them off and that `open_devtools` / `toggle_devtools` open
nothing (and, on CEF, that `print_to_pdf` still works).
