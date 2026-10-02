#!/usr/bin/env bash
#
# Run the backend-agnostic native_e2e_runtime under a given backend and
# propagate its PASS/FAIL exit code. See docs/e2e-testing.md.
#
#   scripts/native-e2e-run.sh <winit|webview|cef> [--layer1|--scheme-body|--lifetime|--lna|--window-api|--hidpi|--io|--system|--devtools-off|--menus-notifications|--auth-thread]
#
# --layer1 (Linux only) wraps the run in the D-Bus StatusNotifier/dbusmenu
# observer (native_e2e_driver) under a private session bus: it checks the
# tray's StatusNotifierItem and dbusmenu layout from outside, fires a menu
# Event that must reach the app, and fails unless the battery passed.
# --scheme-body runs only the custom-scheme request-body round trip (for
# backends where the full battery can't run in CI).
# --lna runs only the Local Network Access checks (lna_checks.rs): the
# custom-scheme page's fetch and WebSocket to a loopback server, and (CEF) a
# page on another origin that must not reach loopback.
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
set -euo pipefail

backend="${1:?usage: native-e2e-run.sh <winit|webview|cef> [--layer1|--scheme-body|--lifetime|--lna|--window-api|--hidpi|--io|--system|--devtools-off|--menus-notifications|--auth-thread]}"
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
if [ "$mode" = "--auth-thread" ]; then
  export LAUFEY_E2E_ONLY=auth-thread
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

# Local Network Access (CEF; lna_checks.rs). The battery's custom-scheme page
# reaches its loopback echo server (fetch and WebSocket) with Chromium's
# checks ON: laufey grants local network access to the app's own declared
# schemes. The negative check needs a page on an origin that is neither the
# app's nor loopback: a loopback port this run declares public, which must be
# known at launch. Skipped when that port is taken.
args=()
if [ "$backend" = "cef" ]; then
  export LAUFEY_E2E_PUBLIC_PORT="${LAUFEY_E2E_PUBLIC_PORT:-$((40000 + RANDOM % 20000))}"
  args+=("--ip-address-space-overrides=127.0.0.1:${LAUFEY_E2E_PUBLIC_PORT}=public")
  # Chromium's password store would ask the private session bus's keyring
  # to unlock: gnome-keyring then shows gcr-prompter, which grabs the
  # pointer and keyboard for the rest of the run, so real X input (xdotool)
  # goes to the prompt instead of the app.
  if [ "$(uname -s)" = "Linux" ]; then
    args+=(--password-store=basic)
  fi
  # On Windows the display itself is scaled (windows-display-scale.ps1).
  if [ "$mode" = "--hidpi" ] && [ "$(uname -s)" = "Linux" ]; then
    args+=("--force-device-scale-factor=$LAUFEY_E2E_EXPECT_SCALE")
  fi
fi
echo "== native-e2e: backend=$backend bin=$bin runtime=$rt =="

is_linux() { [ "$(uname -s)" = "Linux" ]; }

# What runs: the backend, or (--layer1) the D-Bus observer that starts it.
target=("$bin")
if [ "$mode" = "--layer1" ]; then
  is_linux || { echo "--layer1 is Linux-only"; exit 2; }
  export LAUFEY_E2E_HOLD=1
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
  if is_linux; then
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
# Let tee drain what the process wrote last.
sleep 1
echo "== native-e2e: backend exited with status $status =="
if ! grep -q '^\[e2e\] OVERALL ' "$log"; then
  echo "native e2e exited before reporting an overall result" >&2
  rm -f "$log"
  exit 1
fi
rm -f "$log"
exit "$status"
