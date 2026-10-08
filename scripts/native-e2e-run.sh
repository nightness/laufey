#!/usr/bin/env bash
#
# Run the backend-agnostic native_e2e_runtime under a given backend and
# propagate its PASS/FAIL exit code. See docs/e2e-testing.md.
#
#   scripts/native-e2e-run.sh <winit|webview|cef> [--layer1|--late-tray-host|--scheme-body|--bridge-origin|--lifetime|--lna|--window-api|--hidpi|--io|--system|--devtools-off|--menus-notifications|--auth-thread|--launch-visibility|--platform|--title-bar|--file-chooser|--secret-store|--sandbox|--network-quiet]
#
# --layer1 (Linux only) wraps the run in the D-Bus StatusNotifier/dbusmenu
# observer (native_e2e_driver) under a private session bus: it checks the
# tray's StatusNotifierItem and dbusmenu layout from outside, fires a menu
# Event that must reach the app, and fails unless the battery passed.
# --late-tray-host (Linux only) is --layer1 with no tray host at first: the
# battery's first tray must be refused with a reason (platform_features,
# API 45), then the observer starts its StatusNotifierWatcher and the
# backend must see it (trayHost true) and register the next tray as usual.
# --scheme-body runs only the custom-scheme request-body round trip (for
# backends where the full battery can't run in CI).
# --lna runs only the Local Network Access checks (lna_checks.rs): the
# custom-scheme page's fetch and WebSocket to a loopback server, and (CEF) a
# page on another origin that must not reach loopback.
# --bridge-origin runs only the API 44 bridge pin checks
# (bridge_origin_checks.rs) under a launch file it writes next to the backend.
# --lifetime runs only the app-lifetime checks (keep-alive with no window,
# then quit() ending the event loop); they end the process, so they can't
# share the main battery's run.
# --window-api runs only the API 38 window checks (state, constraints,
# screens, title bar, backdrops). On Linux it runs a window manager (openbox)
# under Xvfb, so maximize / minimize / fullscreen must really apply: the
# battery fails them instead of reporting N/A.
# --hidpi runs the same checks at a device scale factor of 2: CEF with
# --force-device-scale-factor=2, WebKitGTK with GDK_SCALE=2, and on Windows
# whatever scale the display was set to before (LAUFEY_E2E_EXPECT_SCALE,
# see scripts/windows-display-scale.ps1). The battery checks the reported
# scale, that sizes and positions stay in DIPs, and the window's physical
# size on screen.
#
# On Linux, --window-api, --hidpi, --io, --system and --menus-notifications
# also hand the battery xdotool (LAUFEY_E2E_XDOTOOL) for real X input: key
# presses for global shortcuts and menu accelerators, and an XDND drag from
# a GTK drag source (laufey_xdnd_source, built with the backend's tests).
# LAUFEY_E2E_WM=none turns the window manager off.
# --io runs only the API 39 checks: drag and drop (through the test hook),
# real file dialogs driven by the test hook, the rich clipboard.
# --system runs only the API 40 checks: global shortcuts (including a
# conflict with a second process), launch at login (only in CI or with
# LAUFEY_E2E_LOGIN_ITEM=1) and DevTools open / close / toggle.
# --devtools-off runs the DevTools checks under LAUFEY_INSPECTABLE=0.
# --menus-notifications runs only the API 41 checks: menu accelerators, the
# context-menu close callback, notification responses, live callbacks and
# scheduling. On Linux it starts a stand-in notification server
# (laufey_mock_notification_server) on the run's private session bus.
# --auth-thread runs only the API 42 checks: UI-thread tasks (proved with the
# OS's own notion of the UI thread, refused after quit()) and auth sessions
# (a real ASWebAuthenticationSession round trip on macOS, not_supported
# elsewhere). Ends with quit().
# --platform runs only the platform-features checks (API 45,
# platform_checks.rs): the probe and the tray agree, and the
# LAUFEY_E2E_EXPECT_* facts given for the session hold. With
# LAUFEY_E2E_HOST_SESSION=1 (Linux) it runs in the caller's own desktop
# session instead of Xvfb and a private bus: the per-desktop matrix check.
# --title-bar runs only the title-bar-preferences checks (API 47,
# title_bar_checks.rs): the JSON, the OS's button side and, on Linux, the
# portal Settings answer and its live changes (under Xvfb the battery serves
# a stub portal on the private bus; with LAUFEY_E2E_HOST_SESSION=1 the
# session's own portal, with LAUFEY_E2E_TITLEBAR_SET_CMD changing a setting).
# --file-chooser (Linux, API 47; file_chooser_checks.rs): which chooser the
# file dialogs use (the portal's FileChooser where the portal offers one,
# GTK's otherwise) agrees with platform_features, and a real dialog closes
# with cancel_file_dialog.
# --secret-store (API 47; secret_checks.rs): on Linux and macOS every
# secure-store call answers within its timeout (a value, not found, or
# unavailable with a reason); LAUFEY_E2E_EXPECT_SECRET=ok|unavailable for a
# real session (macOS CI: ok, the round trip in the login keychain).
# --sandbox runs only the CEF sandbox checks (sandbox_checks.rs): the
# renderer and GPU processes run in Chromium's sandbox, as the OS reports
# them (LAUFEY_E2E_EXPECT_SANDBOX=0 expects the host to have turned it off;
# on Linux LAUFEY_E2E_EXPECT_SANDBOX_MODE=namespace|setuid|off names the
# layer, checked against the host's `laufey: sandbox:` line too).
# --launch-visibility runs only the launch checks (launch_checks.rs): the
# first window of a fresh launch is on screen, its page visible and drawing
# frames, a non-activating window's page too, and a hidden window stays
# hidden. On macOS a window from another process covers the screen first
# (scripts/launch-occluder.swift), as an editor or terminal would.
# --network-quiet (CEF) runs the host with Chromium's net log over an app
# session (network_quiet_checks.rs: the app page, LAUFEY_E2E_QUIET_SECS of
# idle, quit()) and fails on any host in the log other than loopback (and
# WPAD on Windows, the system's proxy auto-discovery): the backend makes no
# request of its own (scripts/netlog-hosts.py). LAUFEY_E2E_NETLOG_OUT keeps
# a copy of the log there.
set -euo pipefail

backend="${1:?usage: native-e2e-run.sh <winit|webview|cef> [--layer1|--late-tray-host|--scheme-body|--lifetime|--lna|--window-api|--hidpi|--io|--system|--devtools-off|--menus-notifications|--auth-thread|--launch-visibility|--platform|--title-bar|--file-chooser|--secret-store|--sandbox|--network-quiet]}"
mode="${2:-}"

# Locate the runtime cdylib (.so / .dylib / .dll).
rt=""
for c in \
  target/release/libnative_e2e_runtime.so \
  target/release/libnative_e2e_runtime.dylib \
  target/release/native_e2e_runtime.dll; do
  if [ -f "$c" ]; then rt="$PWD/$c"; break; fi
done
[ -n "$rt" ] || { echo "native_e2e_runtime cdylib not found (build it first)"; exit 1; }
export LAUFEY_RUNTIME_PATH="$rt"

# The battery serves a page over its own custom scheme (laufey-e2e://app/)
# and asserts it is a real origin. WebView backends learn the scheme from the
# runtime's register_scheme_handler call, but CEF registers custom schemes at
# process start (before the runtime is loaded) and must be told up front —
# see custom_schemes.h. Harmless for the other backends.
export LAUFEY_CUSTOM_SCHEMES=laufey-e2e
# Lets the battery tell backends that share a target OS apart (the late
# scheme registration check differs between WebKitGTK and CEF on Linux).
export LAUFEY_E2E_BACKEND="$backend"
if [ "$mode" = "--scheme-body" ]; then
  export LAUFEY_E2E_ONLY=scheme-body
fi
if [ "$mode" = "--lifetime" ]; then
  export LAUFEY_E2E_ONLY=lifetime
fi
if [ "$mode" = "--bridge-origin" ]; then
  export LAUFEY_E2E_ONLY=bridge-origin
fi
if [ "$mode" = "--lna" ]; then
  export LAUFEY_E2E_ONLY=lna
fi
if [ "$mode" = "--window-api" ] || [ "$mode" = "--hidpi" ]; then
  export LAUFEY_E2E_ONLY=window-api
fi
if [ "$mode" = "--hidpi" ]; then
  export LAUFEY_E2E_EXPECT_SCALE="${LAUFEY_E2E_EXPECT_SCALE:-2}"
  # WebKitGTK (and GTK in the other backends) take the scale from GDK_SCALE;
  # CEF gets --force-device-scale-factor below.
  export GDK_SCALE="${LAUFEY_E2E_EXPECT_SCALE%%.*}"
fi
if [ "$mode" = "--io" ]; then
  export LAUFEY_E2E_ONLY=io
fi
if [ "$mode" = "--system" ]; then
  export LAUFEY_E2E_ONLY=system
  # Names the login entry (HKCU Run value, XDG autostart file).
  export LAUFEY_APP_ID="${LAUFEY_APP_ID:-dev.laufey.e2e.system}"
fi
if [ "$mode" = "--devtools-off" ]; then
  export LAUFEY_E2E_ONLY=devtools-off
  export LAUFEY_INSPECTABLE=0
fi
if [ "$mode" = "--sandbox" ]; then
  export LAUFEY_E2E_ONLY=sandbox
fi
if [ "$mode" = "--auth-thread" ]; then
  export LAUFEY_E2E_ONLY=auth-thread
fi
if [ "$mode" = "--network-quiet" ]; then
  export LAUFEY_E2E_ONLY=network-quiet
fi
occluder_pid=""
if [ "$mode" = "--platform" ]; then
  export LAUFEY_E2E_ONLY=platform
  # The private session bus gets a Secret Service that is installed but
  # never starts (activatable, Exec=/bin/false), whatever this machine has
  # installed: no one here could answer its unlock prompt, so CEF must pick
  # --password-store=basic by itself. (No service at all would be left to
  # Chromium's own fallback; LAUFEY_E2E_EXPECT_COOKIES checks the choice.)
  if [ "$(uname -s)" = "Linux" ] && [ -z "${LAUFEY_E2E_HOST_SESSION:-}" ]; then
    secrets_dir="$(mktemp -d "${TMPDIR:-/tmp}/laufey-e2e-secrets.XXXXXX")"
    mkdir -p "$secrets_dir/dbus-1/services"
    printf '[D-BUS Service]\nName=org.freedesktop.secrets\nExec=/bin/false\n' \
      >"$secrets_dir/dbus-1/services/org.freedesktop.secrets.service"
    export XDG_DATA_DIRS="$secrets_dir:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
  fi
fi
# --io drives its dialogs through the test hook, which can accept only GTK's
# chooser: under Xvfb (a private bus may activate xdg-desktop-portal) the
# dialogs are GTK's. The portal's FileChooser has its own battery
# (--file-chooser) and ctest (file_chooser_dbus_test). In a real session
# (LAUFEY_E2E_HOST_SESSION=1) the session's own choice stands; there the
# accept steps report N/A when the dialog is the portal's.
if [ "$mode" = "--io" ] && [ "$(uname -s)" = "Linux" ] &&
  [ -z "${LAUFEY_E2E_HOST_SESSION:-}" ]; then
  export LAUFEY_FILE_CHOOSER="${LAUFEY_FILE_CHOOSER:-gtk}"
fi
if [ "$mode" = "--secret-store" ]; then
  export LAUFEY_E2E_ONLY=secret-store
fi
if [ "$mode" = "--file-chooser" ]; then
  export LAUFEY_E2E_ONLY=file-chooser
fi
if [ "$mode" = "--title-bar" ]; then
  export LAUFEY_E2E_ONLY=title-bar
  # Under Xvfb the battery serves the private bus's portal Settings itself;
  # in a real session the session's own portal answers.
  if [ "$(uname -s)" = "Linux" ] && [ -z "${LAUFEY_E2E_HOST_SESSION:-}" ]; then
    export LAUFEY_E2E_TITLEBAR_STUB=1
  fi
fi
if [ "$mode" = "--launch-visibility" ]; then
  export LAUFEY_E2E_ONLY=launch-visibility
  if [ "$(uname -s)" = "Darwin" ]; then
    occluder="$(mktemp -d "${TMPDIR:-/tmp}/laufey-occluder.XXXXXX")"
    swiftc -O scripts/launch-occluder.swift -o "$occluder/launch-occluder"
    "$occluder/launch-occluder" >"$occluder/out" 2>&1 &
    occluder_pid=$!
    trap 'kill "$occluder_pid" 2>/dev/null || true' EXIT
    for _ in $(seq 1 300); do
      grep -q '^occluder ready' "$occluder/out" 2>/dev/null && break
      sleep 0.1
    done
    grep '^occluder ready' "$occluder/out" ||
      { cat "$occluder/out"; echo "the occluder did not come up" >&2; exit 1; }
  fi
fi
mock=""
if [ "$mode" = "--menus-notifications" ]; then
  export LAUFEY_E2E_ONLY=menus-notifications
  # Names the Windows AppUserModelID registration and the Linux
  # desktop-entry hint; the schedule file lives in this data directory.
  export LAUFEY_APP_ID="${LAUFEY_APP_ID:-dev.laufey.e2e.notifications}"
  if [ "$(uname -s)" = "Linux" ]; then
    export LAUFEY_DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/laufey-e2e-data.XXXXXX")"
    mock="$(ls webview/build/backend-common/laufey_mock_notification_server \
      cef/build/backend-common/laufey_mock_notification_server \
      cef/build/laufey_mock_notification_server 2>/dev/null | head -1 || true)"
    if [ -n "$mock" ]; then
      export LAUFEY_E2E_NOTIFY_MOCK="$PWD/$mock"
    fi
  fi
fi

# Resolve the backend binary (handles macOS .app bundles).
case "$backend" in
  winit)
    bin="$(ls target/release/laufey_winit target/release/laufey_winit.exe 2>/dev/null | head -1 || true)" ;;
  webview)
    bin="$(ls \
      webview/build/laufey_webview.app/Contents/MacOS/laufey_webview \
      webview/build/laufey_webview \
      webview/build/laufey_webview.exe 2>/dev/null | head -1 || true)" ;;
  cef)
    bin="$(ls \
      cef/build/Release/laufey.app/Contents/MacOS/laufey \
      cef/build/Release/laufey \
      cef/build/Release/laufey.exe 2>/dev/null | head -1 || true)" ;;
  *) echo "unknown backend: $backend"; exit 2 ;;
esac
[ -n "$bin" ] || { echo "backend binary for '$backend' not found (build it first)"; exit 1; }

# --bridge-origin: a launch file next to the backend pins the bridge to one
# origin (docs/launch-config.md); removed when the run ends.
if [ "$mode" = "--bridge-origin" ]; then
  case "$bin" in
    */Contents/MacOS/*) launch_file="${bin%/MacOS/*}/Resources/laufey-launch.json" ;;
    *) launch_file="$(dirname "$bin")/laufey-launch.json" ;;
  esac
  # A launch file makes the backend a packaged app, which loads only the
  # runtime next to its executable (never LAUFEY_RUNTIME_PATH): copy it there
  # for the run. (Git Bash finds "laufey" for "laufey.exe", so ask the OS.)
  case "$(uname -s)" in
    # The Windows CEF executable is CEF's bootstrap, and <exe>.dll is the
    # host it loads, so the runtime is <exe>.runtime.dll there.
    MINGW* | MSYS* | CYGWIN*)
      if [ "$backend" = cef ]; then
        colocated_rt="${bin%.exe}.runtime.dll"
      else
        colocated_rt="${bin%.exe}.dll"
      fi ;;
    Darwin) colocated_rt="$bin.dylib" ;;
    *) colocated_rt="$bin.so" ;;
  esac
  [ ! -e "$launch_file" ] || { echo "refusing to replace $launch_file"; exit 1; }
  [ ! -e "$colocated_rt" ] || { echo "refusing to replace $colocated_rt"; exit 1; }
  mkdir -p "$(dirname "$launch_file")"
  printf '{ "bridgeOrigins": ["app://e2e-bridge-ok"] }\n' >"$launch_file"
  trap 'rm -f "$launch_file" "$colocated_rt"' EXIT
  # First without the runtime: a packaged app that ships none must exit at
  # once with status 3 and say why, not wait (a dialog, an empty window).
  missing_log="$(mktemp "${TMPDIR:-/tmp}/native-e2e-missing.XXXXXX")"
  missing_cmd=("$bin")
  if [ "$(uname -s)" = Linux ]; then
    missing_cmd=(xvfb-run -a "$bin")
  fi
  "${missing_cmd[@]}" >"$missing_log" 2>&1 &
  missing_pid=$!
  for _ in $(seq 1 200); do
    kill -0 "$missing_pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$missing_pid" 2>/dev/null; then
    kill -9 "$missing_pid" 2>/dev/null || true
    cat "$missing_log"
    echo "[e2e] FAIL a packaged app without its runtime exits at once (still running after 20 s)"
    exit 1
  fi
  set +e
  wait "$missing_pid"
  missing_status=$?
  set -e
  if [ "$missing_status" = 3 ] && grep -q 'ships no runtime library' "$missing_log"; then
    echo "[e2e] PASS a packaged app without its runtime exits at once (status 3)"
  else
    cat "$missing_log"
    echo "[e2e] FAIL a packaged app without its runtime exits at once (status $missing_status)"
    exit 1
  fi
  rm -f "$missing_log"
  cp "$rt" "$colocated_rt"
  echo "== launch file $launch_file: $(cat "$launch_file")"
fi

# Local Network Access (CEF; lna_checks.rs). The battery's custom-scheme page
# reaches its loopback echo server (fetch and WebSocket) with Chromium's
# checks ON: laufey grants local network access to the app's own declared
# schemes. The negative check needs a page on an origin that is neither the
# app's nor loopback: a loopback port this run declares public, which must be
# known at launch. The battery serves that page on the first of the ports
# that it can bind. They come from 20000-32767, below every OS's ephemeral
# range (Windows and macOS 49152-65535, Linux 32768-60999): a port the OS
# hands out for an outgoing connection (the engine's own, a server bound to
# port 0) is never one of them. Ports from 40000-59999 collided with those on
# Windows runners, whose ephemeral ports had reached 49700-49900 by then.
args=()
if [ "$backend" = "cef" ]; then
  if [ -z "${LAUFEY_E2E_PUBLIC_PORT:-}" ]; then
    base=$((20000 + RANDOM % 12000))
    LAUFEY_E2E_PUBLIC_PORT="$base,$((base + 251)),$((base + 503))"
  fi
  export LAUFEY_E2E_PUBLIC_PORT
  overrides=""
  for p in ${LAUFEY_E2E_PUBLIC_PORT//,/ }; do
    overrides="${overrides:+$overrides,}127.0.0.1:$p=public"
  done
  args+=("--ip-address-space-overrides=$overrides")
  # Chromium's password store would ask the private session bus's keyring
  # to unlock: gnome-keyring then shows gcr-prompter, which grabs the
  # pointer and keyboard for the rest of the run, so real X input (xdotool)
  # goes to the prompt instead of the app. --platform checks the backend's
  # own choice (API 45: basic when no one can answer the prompt) instead.
  if [ "$(uname -s)" = "Linux" ] && [ "$mode" != "--platform" ]; then
    args+=(--password-store=basic)
  fi
  # On Windows the display itself is scaled (windows-display-scale.ps1).
  if [ "$mode" = "--hidpi" ] && [ "$(uname -s)" = "Linux" ]; then
    args+=("--force-device-scale-factor=$LAUFEY_E2E_EXPECT_SCALE")
  fi
  # Everything the network stack does, for netlog-hosts.py after the run.
  # Git Bash: the native host and Python need a Windows path.
  if [ "$mode" = "--network-quiet" ]; then
    netlog="$(mktemp "${TMPDIR:-/tmp}/laufey-netlog.XXXXXX")"
    if command -v cygpath >/dev/null; then netlog="$(cygpath -w "$netlog")"; fi
    args+=("--log-net-log=$netlog" --net-log-capture-mode=Default)
  fi
fi
echo "== native-e2e: backend=$backend bin=$bin runtime=$rt =="

is_linux() { [ "$(uname -s)" = "Linux" ]; }

# What runs: the backend, or (--layer1) the D-Bus observer that starts it.
target=("$bin")
if [ "$mode" = "--layer1" ] || [ "$mode" = "--late-tray-host" ]; then
  is_linux || { echo "$mode is Linux-only"; exit 2; }
  export LAUFEY_E2E_HOLD=1
  if [ "$mode" = "--late-tray-host" ]; then
    # Written by the battery once its first tray was refused; the driver
    # starts the watcher then.
    LAUFEY_E2E_LATE_TRAY_HOST="$(mktemp -u "${TMPDIR:-/tmp}/laufey-late-tray.XXXXXX")"
    export LAUFEY_E2E_LATE_TRAY_HOST LAUFEY_E2E_REQUIRE_TRAY=1
  fi
  driver="$(ls target/release/native_e2e_driver 2>/dev/null | head -1 || true)"
  [ -n "$driver" ] || { echo "native_e2e_driver not built"; exit 1; }
  target=("$PWD/$driver" "$bin")
fi

# Linux extras for the modes that drive real windows: a window manager, real
# X input through xdotool, and the XDND drag source.
screen="1280x1024x24"
if is_linux; then
  case "$mode" in
    --window-api | --hidpi | --io | --system | --menus-notifications)
      wm="${LAUFEY_E2E_WM:-openbox}"
      if [ "$wm" != none ] && command -v "$wm" >/dev/null; then
        export LAUFEY_E2E_WM="$wm"
      else
        unset LAUFEY_E2E_WM
        echo "== no window manager ($wm not found); WM-dependent checks are N/A =="
      fi
      if command -v xdotool >/dev/null; then
        export LAUFEY_E2E_XDOTOOL="$(command -v xdotool)"
      fi
      src="$(ls webview/build/backend-common/laufey_xdnd_source \
        cef/build/backend-common/laufey_xdnd_source \
        cef/build/laufey_xdnd_source 2>/dev/null | head -1 || true)"
      if [ -n "$src" ]; then
        export LAUFEY_E2E_XDND_SOURCE="$PWD/$src"
      fi
      ;;
    *) unset LAUFEY_E2E_WM ;;
  esac
  # Room for a 2x window.
  if [ "$mode" = "--hidpi" ]; then screen="2560x1600x24"; fi
fi
# Windows: the OLE drag source the --io battery drags real files out of
# (laufey_ole_drag_source, built with the backend's tests).
if [ "$mode" = "--io" ]; then
  src="$(ls webview/build/backend-common/laufey_ole_drag_source.exe \
    cef/build/backend-common/laufey_ole_drag_source.exe 2>/dev/null | head -1 || true)"
  if [ -n "$src" ]; then
    export LAUFEY_E2E_OLE_SOURCE="$PWD/$src"
  fi
fi

# Layer 0: stream the output (so a hang shows how far the battery got) and
# keep a copy, so an unexpected native termination cannot masquerade as
# success merely because macOS reports an exit code of 0. On Linux, run
# headless via Xvfb + a private session bus (some tray implementations need
# it).
#
# A watchdog bounds the run (LAUFEY_E2E_WATCHDOG_SECS, default 300; the whole
# battery takes well under a minute). When it fires it prints every thread's
# stack (macOS `sample`, Linux gdb when installed) before killing the
# process, so a hang leaves evidence instead of only a step timeout.
log="$(mktemp "${TMPDIR:-/tmp}/native-e2e.XXXXXX")"
run_backend() {
  if is_linux && [ -n "${LAUFEY_E2E_HOST_SESSION:-}" ]; then
    # The caller's own desktop session (its display and session bus).
    exec "${target[@]}" ${args[@]+"${args[@]}"}
  elif is_linux; then
    # One X server and private session bus for the run. The window manager
    # (when wanted) manages the display before the backend starts, and the
    # stand-in notification server owns its name on the same bus.
    exec xvfb-run -a -s "-screen 0 $screen" dbus-run-session -- sh -c '
      if [ -n "${LAUFEY_E2E_WM:-}" ]; then
        "$LAUFEY_E2E_WM" >/dev/null 2>&1 &
        i=0
        until xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q "window id"; do
          i=$((i + 1))
          if [ "$i" -gt 100 ]; then
            echo "native e2e: $LAUFEY_E2E_WM did not start" >&2
            exit 1
          fi
          sleep 0.1
        done
        echo "== window manager: $LAUFEY_E2E_WM ==" >&2
        export LAUFEY_E2E_WM_RUNNING=1
      fi
      if [ -n "${LAUFEY_E2E_NOTIFY_MOCK:-}" ]; then
        "$LAUFEY_E2E_NOTIFY_MOCK" &
        for _ in 1 2 3 4 5 6 7 8 9 10; do sleep 0.2; done
      fi
      exec "$@"' sh "${target[@]}" ${args[@]+"${args[@]}"}
  else
    exec "${target[@]}" ${args[@]+"${args[@]}"}
  fi
}
run_backend > >(tee "$log") 2>&1 &
pid=$!
watchdog_secs="${LAUFEY_E2E_WATCHDOG_SECS:-300}"
(
  waited=0
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$watchdog_secs" ]; then
      echo "native e2e: watchdog: no exit after ${watchdog_secs}s; stacks follow" >&2
      case "$(uname -s)" in
        Darwin)
          # The backend and its helper processes (CEF renderer / GPU).
          for p in "$pid" $(pgrep -P "$pid" 2>/dev/null || true); do
            sample "$p" 3 -mayDie 2>&1 | head -c 200000 >&2 || true
          done ;;
        Linux)
          if command -v gdb >/dev/null; then
            for p in $(pgrep -f "$bin" 2>/dev/null || true); do
              gdb -p "$p" -batch -ex "thread apply all bt" 2>&1 |
                head -c 200000 >&2 || true
            done
          fi ;;
      esac
      kill -9 "$pid" 2>/dev/null || true
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done
) &
watchdog=$!
set +e
wait "$pid"
status=$?
set -e
wait "$watchdog" 2>/dev/null || true
if [ -n "${LAUFEY_E2E_LATE_TRAY_HOST:-}" ]; then
  rm -f "$LAUFEY_E2E_LATE_TRAY_HOST"
fi
# Let tee drain what the process wrote last.
sleep 1
echo "== native-e2e: backend exited with status $status =="
if ! grep -q '^\[e2e\] OVERALL ' "$log"; then
  echo "native e2e exited before reporting an overall result" >&2
  rm -f "$log"
  exit 1
fi
# The Linux CEF host logs the sandbox it chose: one `laufey: sandbox: <mode>
# (<reason>)` line, which must be the mode the run expects.
if is_linux && [ "$backend" = cef ]; then
  sandbox_line="$(grep -m1 '^laufey: sandbox: ' "$log" || true)"
  if [ -z "$sandbox_line" ]; then
    echo "[e2e] FAIL the CEF host logs its sandbox mode (no 'laufey: sandbox:' line)"
    status=1
  elif [ -n "${LAUFEY_E2E_EXPECT_SANDBOX_MODE:-}" ]; then
    case "$sandbox_line" in
      "laufey: sandbox: $LAUFEY_E2E_EXPECT_SANDBOX_MODE ("*)
        echo "[e2e] PASS the host's sandbox mode: $sandbox_line" ;;
      *)
        echo "[e2e] FAIL the host's sandbox mode is $LAUFEY_E2E_EXPECT_SANDBOX_MODE: $sandbox_line"
        status=1 ;;
    esac
  else
    echo "== $sandbox_line =="
  fi
fi
# WebKitGTK window battery: GTK reports a call on a destroyed widget as a
# critical instead of crashing (the window the user closes in
# close_checks.rs is reached through a freed GtkWindow when its state
# outlives it). A clean run has none, so any is a failure.
if is_linux && [ "$backend" = webview ] &&
  { [ "$mode" = "--window-api" ] || [ "$mode" = "--hidpi" ]; } &&
  grep -q 'laufey_webview:[0-9]*): Gtk-CRITICAL' "$log"; then
  grep 'Gtk-CRITICAL' "$log" >&2
  echo "native e2e: GTK reported criticals (a call reached a destroyed widget)" >&2
  rm -f "$log"
  exit 1
fi
# --network-quiet: the hosts in the CEF host's net log (loopback only).
if [ "$mode" = "--network-quiet" ]; then
  if [ "$backend" != cef ]; then
    echo "[e2e] N/A  no network requests of its own (net log: CEF only)"
  else
    if [ -n "${LAUFEY_E2E_NETLOG_OUT:-}" ]; then
      cp "$netlog" "$LAUFEY_E2E_NETLOG_OUT" || true
    fi
    allow=()
    case "$(uname -s)" in
      MINGW* | MSYS* | CYGWIN*) allow+=(--allow wpad) ;;
    esac
    py="$(command -v python3 || command -v python || true)"
    echo "== hosts in the net log =="
    if [ -z "$py" ]; then
      echo "[e2e] FAIL no network requests of its own (no Python to read the net log)"
      status=1
    elif "$py" scripts/netlog-hosts.py "$netlog" --expect 127.0.0.1 \
      ${allow[@]+"${allow[@]}"}; then
      echo "[e2e] PASS no network requests of its own: only loopback in the net log"
    else
      echo "[e2e] FAIL no network requests of its own: the net log has other hosts (above)"
      status=1
    fi
    rm -f "$netlog"
  fi
fi
rm -f "$log"
exit "$status"
