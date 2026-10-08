# Backends

A backend is the native executable that hosts a browser (or windowing) engine
and implements the [C ABI](c-abi.md). laufey ships three; a fourth is on a
branch. All implement the same `laufey_backend_api_t`, so a runtime is portable
across them — the differences are in engine, process model, size, and a few
features that a given engine can't express on a given OS (see
[the feature pages](window-management.md)).

| Backend                                                           | Engine        | Process model | Bundled | JS bridge |
| ----------------------------------------------------------------- | ------------- | ------------- | ------- | --------- |
| [CEF](https://github.com/littledivy/laufey/tree/main/cef)         | Chromium 144  | multi-process | yes     | yes       |
| [WebView](https://github.com/littledivy/laufey/tree/main/webview) | system native | single        | no      | yes       |
| [Winit](https://github.com/littledivy/laufey/tree/main/winit)     | none          | single        | n/a     | no        |

Platform support is x86_64 + aarch64 on macOS and Linux, x86_64 on Windows.
There is also an **iOS** backend (UIKit + WKWebView, statically linked) — see
[iOS](ios.md). Android is not supported.

## CEF

Embeds Chromium 144 through the Chromium Embedded Framework and runs Chromium's
real multi-process architecture — a browser process plus renderer, GPU, and
utility subprocesses, with the same rendering and DevTools you get in Chrome.
The engine is bundled into the app, so binaries are large but rendering is
identical everywhere and independent of the host OS.

Sources live in [`cef/`](https://github.com/littledivy/laufey/tree/main/cef);
shared native features come from `backend-common`. On Windows the backend links
the static CRT (`/MT`), so everything it links — including `backend-common` — is
built `/MT`.

Linux caveat: the application menu doesn't work under CEF (a `GtkMenuBar` must
be packed into a GtkWindow above the browser, and reparenting CEF into a
client-owned GtkWindow via `CefWindowInfo::SetAsChild` breaks on XWayland).
Context menus do work, because `GtkMenu` popups need no GtkWindow container.

Custom schemes: Chromium registers them at process start, before the runtime is
loaded, so besides calling `register_scheme_handler` the embedder declares them
when launching the host — `--laufey-custom-schemes=myapp` or
`LAUFEY_CUSTOM_SCHEMES=myapp` (`cef/src/custom_schemes.h`). See
[Custom URL schemes](custom-schemes.md).

### No network requests of its own

The CEF host sends no request the app didn't ask for: every request in a laufey
app comes from its pages or its runtime. laufey's browsers are Chrome-style, so
Chrome's profile services run in them, and `--disable-background-networking`
leaves several that contact Google. Found from Chromium net logs of a launch and
an idle period (each request's traffic annotation names the feature), and what
the host does about each in the browser process (`cef/src/app.cc`,
`backend-common/include/laufey_cef_network_quiet.h`):

| Request                                                           | Cause (traffic annotation)                                                                 | Default                                                |
| ----------------------------------------------------------------- | ------------------------------------------------------------------------------------------ | ------------------------------------------------------ |
| `clients2.google.com/time/1/current`                              | network time tracker (`network_time_component`)                                            | `--disable-features=NetworkTimeServiceQuerying`        |
| `www.google.com/async/folae`                                      | AI Mode eligibility (`aim_eligibility_fetch`)                                              | `--disable-features=AimEnabled`                        |
| preconnects to `www.google.com`                                   | default search engine preconnector                                                         | `--disable-features=PreconnectToSearch`                |
| `accounts.google.com/ListAccounts`                                | the Google accounts in the cookie jar (`gaia_auth_list_accounts`), asked by signin metrics | `--gaia-url=https://localhost:9` (see below)           |
| `update.googleapis.com`, `edgedl.me.gvt1.com` (60 s after launch) | component updater (`update_client`)                                                        | `--disable-component-update`                           |
| `redirector.gvt1.com/edgedl/chrome/dict/*.bdic` (Linux)           | Hunspell dictionary download (`spellcheck_hunspell_dictionary`)                            | only dictionaries already on disk are kept (see below) |

- **Google accounts.** No switch or feature turns the ListAccounts check off:
  Chrome's signin metrics services ask for the accounts in the cookie jar when
  the profile starts. The host points Chrome's Google-accounts origin at port 9
  on localhost, which is on Chromium's restricted-port list, so each check ends
  with `ERR_UNSAFE_PORT` before any DNS lookup or socket. Pages are not
  affected: `accounts.google.com` loads as any other site (Chrome only stops
  treating it as its own sign-in origin, which laufey never used).
- **Spellcheck.** macOS and Windows check spelling with the OS's own
  spellchecker and download nothing. On Linux Chromium uses Hunspell and
  downloads each language's dictionary from Google the first time it is used, so
  at startup the host keeps only the languages whose dictionary is already in
  the profile's `Dictionaries` folder (`<data dir>/CEF/Dictionaries`, a file
  such as `en-US-10-1.bdic`), and spelling is not checked otherwise. An app that
  wants it ships the `.bdic` files and copies them there.

An app's own command line wins: a feature named in `--enable-features` is not
disabled, a `--gaia-url` of its own is kept, and `--component-updater=...`
(configuring the updater) keeps the component updater running. On Windows the
net log also shows DNS lookups of `wpad`: the system's proxy auto-discovery
("Automatically detect settings"), which follows the OS's proxy settings.

The `--network-quiet` e2e mode checks it on every CEF leg in CI
(`scripts/native-e2e-run.sh cef --network-quiet`, see
[End-to-end testing](e2e-testing.md)): the host runs with Chromium's net log
over a launch, the app page and an idle period (75 s in CI, past the component
updater's first check), and the run fails on any host in the log other than
loopback.

### The Chromium sandbox

The CEF backend runs Chromium's renderer, GPU and utility processes in
Chromium's sandbox on macOS, Linux and Windows. The WebView backends are not
affected: WKWebView, WebView2 and WebKitGTK sandbox their web content processes
themselves.

`--no-sandbox` on the host's command line still turns it off for a debugging
session; a deep-link launch drops that switch with every other Chromium switch
(see [Deep links](deep-links.md)).

- **macOS.** Every helper app (`laufey Helper*.app`, `cef/src/helper.cc`) enters
  the sandbox before it loads the framework: `CefScopedSandboxContext` loads the
  distribution's `libcef_sandbox.dylib` from the framework's `Libraries`
  directory and applies the Seatbelt profile the browser process passes for the
  helper's role (`main_mac.mm` sets `no_sandbox = false`). Nothing changes in
  packaging or signing: the helpers keep their entitlements
  (`cef/macos/entitlements-helper.plist`, JIT for V8), and the framework ships
  `libcef_sandbox.dylib` signed with it.
- **Linux.** Chromium runs a renderer under seccomp-bpf plus a layer-1 sandbox
  (its own PID, network and user namespaces), which comes from one of two
  places:
  - unprivileged user namespaces, where the kernel allows them. Ubuntu 23.10 and
    later restrict them with AppArmor
    (`kernel.apparmor_restrict_unprivileged_userns=1`) unless an AppArmor
    profile grants `userns` to the executable;
  - the setuid helper `chrome-sandbox` next to the executable, owned by root
    with mode 4755. Chromium uses it only when it can't create user namespaces.
    A system package can install it that way: Deno Desktop's `.deb` and `.rpm`
    do.

  With neither, Chromium refuses to start sandboxed ("No usable sandbox!"), and
  as root it refuses to start at all. So the host (`main_linux.cc`,
  `laufey_cef_sandbox.h`) probes the machine the way Chromium will, before
  `CefInitialize`: a child started in a new user namespace (a raw
  `clone(CLONE_NEWUSER)`, as Chromium does) that maps its ids and creates a
  nested one, and the helper's owner and mode. A helper can't gain root, and
  counts as unusable, when the process runs with `no_new_privs` (a container, a
  systemd unit with `NoNewPrivileges=`) or the executable sits on a `nosuid`
  mount. The host turns the sandbox off only when Chromium would have no usable
  sandbox, or as the root user; when the probe itself can't run (the child can't
  be started, say at `RLIMIT_NPROC`), it leaves the sandbox on and Chromium
  chooses. Every launch logs one line, the mode (`namespace`, `setuid`,
  `chromium` or `off`) and why:

  ```
  laufey: sandbox: off (unprivileged user namespaces are restricted by AppArmor (kernel.apparmor_restrict_unprivileged_userns=1), and there is no chrome-sandbox helper next to the executable; install the app from its .deb or .rpm package to run web content sandboxed)
  ```

  That is the case for a tarball or an AppImage (mounted `nosuid`) on Ubuntu
  23.10 and later. The mode and the reason are also platform facts, `sandbox`
  and `sandboxReason` ([Platform features](platform-features.md)). By default
  the app then runs with web content unsandboxed; an app that would rather not
  start sets `"requireSandbox": true` in its launch file (or the launcher sets
  `LAUFEY_REQUIRE_SANDBOX=1`): where the mode would be `off`, the host prints
  `laufey: this app requires the Chromium sandbox (requireSandbox), which is not
  available here: <reason>`
  and exits with status 78 before CEF or the runtime loads
  ([Launch configuration](launch-config.md)). The decision rests on the machine
  and the installed files only, never on the command line
  (`CHROME_DEVEL_SANDBOX`, Chromium's override of the helper's path, can only
  make Chromium abort on a bad helper, never turn the sandbox off). The GPU
  process's seccomp sandbox is Chromium's own decision: Chromium skips it when
  the GL driver started threads before the sandbox, as Mesa's llvmpipe (software
  GL, e.g. under Xvfb) does.
- **Windows.** CEF 149's Windows sandbox comes with the bootstrap model
  (`USE_SANDBOX=ON`, CEF's default, defines `CEF_USE_BOOTSTRAP`): the sandbox
  information is created by the distribution's `bootstrap.exe`
  (`cef_sandbox_info_create` is linked into it, not into `libcef.dll`), so the
  bootstrap is the app's executable and laufey's CEF host is a DLL it loads. The
  build ships the two side by side in `cef/build/Release`:
  - `laufey.exe`, CEF's `bootstrap.exe` copied under laufey's name. It loads the
    client DLL named after itself (`<app>.exe` loads `<app>.dll`) and calls its
    `RunWinMain` in every process, with the sandbox information;
  - `laufey.dll`, the host (`main_windows.cc`): `RunWinMain` checks that the
    bootstrap comes from the CEF release the host was built against, then passes
    the sandbox information to `CefExecuteProcess` and `CefInitialize`
    (`no_sandbox = false`). It also runs the headless worker mode.

  So a packaged Windows CEF app is laid out as:

  ```
  <App>/
    <App>.exe          CEF's bootstrap, with the app's icon and version resources
    <App>.dll          laufey's CEF host (laufey.dll, renamed)
    <App>.runtime.dll  the runtime
    libcef.dll, ...
  ```

  The runtime is `<app>.runtime.dll` because `<app>.dll` is the host: the
  Windows CEF host's co-located runtime lookup (`LaufeyFindColocatedRuntime`)
  looks for `<executable name>.runtime.dll`. The WebView backend is not
  affected: its host is the executable itself and keeps loading `<app>.dll`. A
  packager renames `laufey.exe` and `laufey.dll` together and replaces the
  bootstrap's icon and version resources (CEF's own: "CEF bootstrap" in Task
  Manager) with the app's. A signed bootstrap loads only a client DLL signed
  with the same certificate, and it verifies its own signature first
  (WinVerifyTrust): one that doesn't chain to a trusted root, such as a
  self-signed test certificate nobody trusted, stops the app at launch with a
  fatal "certificate checks" error. So the two are signed together, with a
  trusted certificate (a packager that signs every PE file in the app does).

  The bootstrap moves the browser process to the executable's directory before
  the host runs (`SetCwdForBrowserProcess` in CEF's `bootstrap_win.cc`; CEF does
  it so Chromium's child processes don't hold an arbitrary directory open), and
  the directory the app was started in is gone. A launcher that knows it passes
  it in `LAUFEY_CWD`: the host changes back to it and removes the variable (Deno
  Desktop's runtime does this for the workers it forks of its own executable and
  for its updater's detached launches, and `deno desktop`'s dev run for the app,
  so a dev server's workers keep the project directory). Because the host
  changes back before Chromium starts, the renderer, GPU and crashpad processes
  it starts inherit that directory as their working directory and hold it open
  while they run (it can't be removed or renamed until the app exits); that is
  harmless. Any other launch, from a shell or a shortcut, starts in the
  executable's (install) directory. Every laufey host, on every backend and OS,
  removes `LAUFEY_CWD` from its environment at startup, so the variable never
  reaches the programs an app starts. Since that directory can be anywhere, the
  host first takes the working directory out of the DLL search order
  (`SetDllDirectoryW(L"")`, before any library is loaded by name). It does not
  call `SetDefaultDllDirectories`, which would also drop `PATH` and so break a
  Node-API addon whose dependency is found on `PATH`, or a `Deno.dlopen()` of a
  bare name on `PATH`.

  The layout is new: a Windows CEF app packaged before it, as one unsandboxed
  `<App>.exe` with the runtime as `<App>.dll` (Deno Desktop runtime
  2.9.7-denext.9 and earlier), can't update itself into it with Deno Desktop's
  full-app updater. Reinstall the app.

  The renderers run at Untrusted integrity and the GPU process at Low.
  `-DUSE_SANDBOX=OFF` builds the unsandboxed single `laufey.exe` instead
  (`WinMain`, `no_sandbox = true`); it loads `<executable name>.runtime.dll`
  too.

The native e2e battery checks the result from outside the backend
(`examples/native_e2e/src/sandbox_checks.rs`,
`scripts/native-e2e-run.sh
cef --sandbox`): on Linux every renderer has a
seccomp-bpf filter (`/proc/<pid>/status` `Seccomp: 2`), `NoNewPrivs: 1` and a
PID namespace of its own (a nested `NSpid`); on macOS `sandbox_check()` reports
the renderer and GPU helpers sandboxed; on Windows the renderer and GPU
processes (by their `--type=`) run below Medium integrity or in an AppContainer,
while the browser process doesn't. `LAUFEY_E2E_EXPECT_SANDBOX=0` asserts the
opposite, for a Linux run without a usable sandbox or a Windows build with
`-DUSE_SANDBOX=OFF`. On Linux `LAUFEY_E2E_EXPECT_SANDBOX_MODE` names the layer
(`namespace`: the renderer has a user namespace of its own; `setuid`: it
doesn't), checked against the host's `laufey: sandbox:` line too.

## WebView

Delegates to the platform's native web engine — **WKWebView** on macOS,
**WebView2** on Windows, **WebKitGTK** on Linux. The engine is never bundled, so
apps stay small, at the cost of rendering that varies by OS and engine version.
Single-process.

Sources live in
[`webview/`](https://github.com/littledivy/laufey/tree/main/webview), one file
per platform (`webview_macos.mm`, `webview_windows.cc`, `webview_linux.cc`),
sharing `backend-common` for menus, tray, dialogs, dock, and notifications.

Custom schemes registered through `register_scheme_handler` are installed per
engine — a `WKURLSchemeHandler` per scheme on the `WKWebViewConfiguration`,
`webkit_web_context_register_uri_scheme` plus the security manager's secure/CORS
flags on WebKitGTK, `CoreWebView2CustomSchemeRegistration` (TreatAsSecure,
HasAuthorityComponent) on the WebView2 environment — so each is a real
`<scheme>://<host>` origin. WKWebView and WebView2 read the set when a web view
(WebView2: the first one) is created; register schemes before the first window.
See [Custom URL schemes](custom-schemes.md).

### WebKitGTK on X11: shared-memory frames

On an X11 display the host sets `WEBKIT_DMABUF_RENDERER_FORCE_SHM=1` before the
first web view, unless the user set it, `WEBKIT_DISABLE_DMABUF_RENDERER` or
`WEBKIT_DISABLE_COMPOSITING_MODE` (any value) themselves.

GTK 3 draws with GLX on X11, which WebKit's EGL display can't share, so its UI
process takes each frame as a GBM buffer and maps it to the CPU (`gbm_bo_map`)
to paint it with cairo (`AcceleratedBackingStore::BufferGBM`). With shared
memory the web process reads the frame back itself (`BufferSHM`), so the frame
costs the same one copy to the CPU on every GPU; only the side of the process
boundary changes, and the UI process doesn't use libgbm on X11 at all. On
Wayland GTK 3 shares EGL with WebKit and keeps the zero-copy `BufferEGLImage`
path; nothing is set there. `WEBKIT_DISABLE_DMABUF_RENDERER=1` is not used
instead: with it the web process aborts in `FrameRenderer::graphicsLayerFactory`
on the first `document.startViewTransition()`.

The SIGSEGV in `BufferGBM::didUpdateContents` seen on Mali / Panfrost (Fedora
44, webkit2gtk4.1 2.54.0, XFCE and i3) was not WebKit's: every crash came while
another thread was in `exit()`, after an exit handler registered after the
host's exit guard had destroyed WebKit's GBM device (unloading libgbm's backend,
which the next frame's `gbm_bo_map` called into). The host's `exit()` now parks
the UI thread before any exit handler runs (`InstallUiExitGuard`). Shared memory
stays on X11: it costs nothing there, and it keeps the UI process off libgbm
should an exit ever get past the guard.

## Display: Wayland or X11 (Linux)

Each backend goes to the display that is there, never by `XDG_SESSION_TYPE`,
which is only a hint (GDM's autologin into an Xorg session such as XFCE or i3
leaves it `wayland` with only `$DISPLAY` set): Wayland when `$WAYLAND_DISPLAY`
names a socket that exists (absolute, else under `$XDG_RUNTIME_DIR`) or
`$WAYLAND_SOCKET` is set, else X11 when `$DISPLAY` is set. CEF passes it as an
explicit `--ozone-platform=` (an app's own `--ozone-platform` or
`--ozone-platform-hint` wins; with no display at all,
`--ozone-platform-hint=auto`). The clipboard and global shortcut backends,
window placement and `platformFeatures().sessionType` follow the same choice.
WebKitGTK and Winit choose through GTK and winit, which connect the same way.

## Winit

Engine-free. It creates native windows via
[winit](https://github.com/rust-windowing/winit) for apps that draw their own
content — GPU surfaces, custom renderers — without loading a web engine. There
is no JS bridge; `get_window_handle` / `get_display_handle` expose the raw
handles needed to create a rendering surface. Sources in
[`winit/`](https://github.com/littledivy/laufey/tree/main/winit).

## Servo (experimental)

A [Servo](https://servo.org)-based backend is preserved on the
[`servo`](https://github.com/littledivy/laufey/tree/servo) branch for future
work and is not part of the mainline build.

## backend-common

CEF and WebView share their native-API implementations (menus, tray, dock,
dialogs, notifications, key mapping) in
[`backend-common/`](https://github.com/littledivy/laufey/tree/main/backend-common),
included as a CMake subdirectory by each backend. The winit backend shares its
non-engine pieces through `backend-winit-common` instead.

## How an app ends

The event loop ends when the last window closes (unless
`set_quit_on_last_window_closed(false)`), on `quit()`, or on `exit_app(code)`
(API 46), which is `quit()` with an exit code. Then the backend tells the
runtime (`laufey_runtime_shutdown`), waits for its thread (only briefly after
`exit_app`, whose caller may block for good, as `Deno.exit()` does), shuts its
engine down (`CefShutdown`, or the web views are released) and ends the process
with `exit_app`'s code, else 0.

A runtime ends the app with `exit_app` rather than its own `exit()`: an `exit()`
from the runtime's thread ends the process under a running engine. CEF never
shuts down, so cookies and web storage written just before can be lost (on
Windows the next launch read neither back, every time), and on Windows the
process can hang (below).

On Windows the backends end the process with `TerminateProcess` once the engine
has shut down, not by returning to the CRT. The CRT's exit is `ExitProcess`,
which ends every other thread and then runs each DLL's `DLL_PROCESS_DETACH`,
static destructors and `atexit` callbacks, and one of them can wait for good:
Chromium loads `Windows.Media.dll` on the browser's main thread in every CEF
app, and on windows-ci a CEF process sometimes never ended, its one thread left
waiting in combase's `MTAThreadWaitForCall` (a COM call into an apartment whose
thread was already gone) under that DLL's exit-time cleanup. Such a process
could not be killed and kept its profile locked, so the app's next launch
failed. Nothing an app relies on runs in that code: the profile is on disk after
`CefShutdown`, and the host's stdout and stderr are flushed first.
`scripts/exit-e2e-run.sh` checks each path (see
[e2e-testing.md](e2e-testing.md)).

The WebKit backends (WebKitGTK on Linux, WKWebView on macOS) also flush
`localStorage` before the process ends. WebKit writes it from its network
process, in an SQLite transaction committed 500 ms after the first write
(`SQLiteStorageArea`); a host that ended within that time lost the write now and
then (on CI 1 of 9 ends on macOS, 2 of 5 on Linux; cookies survived). The
network process commits when the host's connection closes, but only after the
host is gone, so a launch right after could still read the previous value. Once
the runtime has shut down, the backend (`FlushWebStorage`, at most 2 s):

- macOS: calls WebKit's `WKWebsiteDataStoreSyncLocalStorage` (C SPI, looked up
  with `dlsym`), which commits every open transaction and answers, and runs the
  main run loop until it does.
- Linux: fetches the `localStorage` records
  (`webkit_website_data_manager_fetch`), which the network process answers from
  its storage queue after the writes queued before; waits 600 ms, past the
  commit those writes armed; and fetches again, which is answered after that
  commit. WebKitGTK's API has no call that commits directly.

## Exit and shutdown (Linux)

On Linux, CEF and WebView install an exit guard: when the process exits from
another thread (the runtime's `Deno.exit()`), the UI thread is parked before the
libraries' destructors run (GTK kept drawing during exit and crashed in
pixman/cairo), and every UI dispatch still waiting is answered as not run. A
watchdog then ends the process if exit itself hangs (a destructor or exit
handler waiting on a lock or a thread that is gone):

- `LAUFEY_EXIT_WATCHDOG_SECS` sets how long exit may take before the watchdog
  ends the process: 5 seconds by default; `0` turns the watchdog off. It is read
  once, when the backend starts.
- On glibc the watchdog exits with the status `exit()` was given. Without glibc
  (musl, say) that status isn't available to the exit handler, so a hung exit
  ends with status 1.
