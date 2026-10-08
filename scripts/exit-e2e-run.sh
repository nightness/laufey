#!/usr/bin/env bash
#
# How an app ends, end to end: launches a backend with the storage_e2e_runtime
# that writes a fresh value to localStorage and a cookie and then ends at once
# by one path, and checks that the process exits (no hang), with the exit code
# that path asks for, and that the next launch of the same app reads the value
# back (the engine wrote its profile before the process ended). See
# docs/backends.md, "How an app ends".
#
#   scripts/exit-e2e-run.sh <webview|cef> [iterations] [paths...]
#
# Paths (default: quit close exit):
#   quit          close the window, then quit()
#   close         close the last window
#   exit          exit_app (laufey::exit) with code 7, the thread then blocked
#                 for good, as Deno.exit() does
#   process-exit  std::process::exit(5) from the runtime thread while the
#                 engine still runs (how a runtime ended before API 46). Only
#                 when named: it is not expected to keep what was written.
#
# A launch that hasn't ended LAUFEY_E2E_EXIT_TIMEOUT seconds (default 60)
# after it started is a hang. The run stops at the first one: on Windows such
# a process may not be killable, and it keeps its profile locked.
#
# Build first: `cargo build --release -p storage_e2e_runtime` and the backend.
set -euo pipefail

backend="${1:?usage: exit-e2e-run.sh <webview|cef> [iterations] [paths...]}"
iterations="${2:-3}"
shift $(($# > 1 ? 2 : 1))
paths=("$@")
[ ${#paths[@]} -gt 0 ] || paths=(quit close exit)
timeout_s="${LAUFEY_E2E_EXIT_TIMEOUT:-60}"

rt=""
for c in \
  target/release/libstorage_e2e_runtime.so \
  target/release/libstorage_e2e_runtime.dylib \
  target/release/storage_e2e_runtime.dll; do
  if [ -f "$c" ]; then rt="$PWD/$c"; break; fi
done
[ -n "$rt" ] || { echo "storage_e2e_runtime cdylib not found (build it first)"; exit 1; }
export LAUFEY_RUNTIME_PATH="$rt"

case "$backend" in
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

case "$(uname -s)" in
  Darwin) platform=macos ;;
  Linux) platform=linux ;;
  *) platform=windows ;;
esac

if [ "$platform" = windows ]; then
  scratch="$(cygpath -u "${TEMP:-/tmp}")/laufey-exit-e2e"
else
  scratch="${TMPDIR:-/tmp}"
  scratch="${scratch%/}/laufey-exit-e2e"
fi
rm -rf "$scratch"
mkdir -p "$scratch/logs" "$scratch/data"
# Each path's app keeps its profile in its own LAUFEY_DATA_DIR under the
# scratch directory (WKWebView keys its store on the path string).
data_dir() { # <path>
  local d="$scratch/data/$1"
  if [ "$platform" = windows ]; then cygpath -w "$d"; else echo "$d"; fi
}
if [ "$platform" = linux ]; then
  export XDG_DATA_HOME="$scratch/xdg-data"
  mkdir -p "$XDG_DATA_HOME"
fi
cleanup() { [ -n "${KEEP_SCRATCH:-}" ] || rm -rf "$scratch" 2>/dev/null || true; }
trap cleanup EXIT

port=$((20000 + RANDOM % 20000))
failed=0
hung=0
pass() { echo "[exit-e2e] PASS $*"; }
fail() { echo "[exit-e2e] FAIL $*"; failed=1; }

# launch <log> [VAR=value ...]: one backend launch; sets `rc` (124 when it
# had to be killed) and `secs`.
launch() {
  local log="$scratch/logs/$1.log"
  shift
  local cmd=(env -u LAUFEY_APP_ID -u LAUFEY_DATA_DIR -u LAUFEY_E2E_STORAGE_EXIT
    -u LAUFEY_E2E_STORAGE_EXPECT -u LAUFEY_E2E_STORAGE_HOLD_MS
    LAUFEY_E2E_STORAGE_PORT="$port" "$@")
  if [ "$platform" = linux ]; then
    cmd+=(xvfb-run -a dbus-run-session -- "$bin")
  else
    cmd+=("$bin")
  fi
  local start=$SECONDS
  "${cmd[@]}" >"$log" 2>&1 &
  local pid=$!
  local waited=0
  while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$timeout_s" ]; do
    sleep 1
    waited=$((waited + 1))
  done
  rc=0
  if kill -0 "$pid" 2>/dev/null; then
    rc=124
    echo "[exit-e2e] $1: still running after ${timeout_s}s (hang); killing it"
    kill -9 "$pid" 2>/dev/null || true
    sleep 5
    if kill -0 "$pid" 2>/dev/null; then
      echo "[exit-e2e] $1: the hung process can't be killed (pid $pid)"
    fi
    wait "$pid" 2>/dev/null || true
  else
    wait "$pid" || rc=$?
  fi
  secs=$((SECONDS - start))
}

# Counters per path, as plain variables (macOS's bash 3.2 has no associative
# arrays): bump <counter> <path>, count <counter> <path>.
bump() { local v="$1_${2//-/_}"; eval "$v=\$((\${$v:-0} + 1))"; }
count() { local v="$1_${2//-/_}"; eval "echo \${$v:-0}"; }
echo "== exit-e2e: backend=$backend bin=$bin iterations=$iterations paths=${paths[*]}"
for ((i = 1; i <= iterations; i++)); do
  for path in "${paths[@]}"; do
    case "$path" in
      quit) mode=quit want=0 ;;
      close) mode=close want=0 ;;
      exit) mode=exit:7 want=7 ;;
      process-exit) mode=process-exit:5 want=5 ;;
      *) echo "unknown path: $path"; exit 2 ;;
    esac
    value="v$RANDOM$RANDOM$i"
    dir="$(data_dir "$path")"
    bump runs "$path"

    launch "$path-$i-write" LAUFEY_DATA_DIR="$dir" LAUFEY_E2E_STORAGE_MODE=write \
      LAUFEY_E2E_STORAGE_VALUE="$value" LAUFEY_E2E_STORAGE_EXIT="$mode"
    wlog="$scratch/logs/$path-$i-write.log"
    if ! grep -q '^\[e2e\] OVERALL PASS' "$wlog" || grep -q '^\[e2e\] FAIL' "$wlog"; then
      fail "$path #$i: the write launch (see below)"
      sed 's/^/    | /' "$wlog" | tail -40
    fi
    if [ "$rc" = 124 ]; then
      bump hangs "$path"
      hung=1
      fail "$path #$i: the process didn't end after the write (${secs}s)"
      break 2
    fi
    if [ "$rc" != "$want" ]; then
      bump bad_rc "$path"
      fail "$path #$i: exit code $rc, want $want"
    fi

    launch "$path-$i-read" LAUFEY_DATA_DIR="$dir" LAUFEY_E2E_STORAGE_MODE=read \
      LAUFEY_E2E_STORAGE_EXPECT="$value"
    rlog="$scratch/logs/$path-$i-read.log"
    if [ "$rc" = 124 ]; then
      bump hangs quit
      hung=1
      fail "$path #$i: the read launch didn't end (${secs}s)"
      break 2
    fi
    if grep -q '^\[e2e\] OVERALL PASS' "$rlog"; then
      echo "[exit-e2e] $path #$i: exit $want, the next launch read $value back"
    else
      bump lost "$path"
      if [ "$path" = process-exit ]; then
        echo "[exit-e2e] $path #$i: the next launch didn't read $value back:" \
          "$(grep -E '^\[e2e\] read ' "$rlog" || true)"
      else
        fail "$path #$i: the next launch didn't read $value back"
        sed 's/^/    | /' "$rlog" | tail -30
      fi
    fi
  done
done

for path in "${paths[@]}"; do
  echo "[exit-e2e] $path: $(count runs "$path") runs, $(count hangs "$path") hung," \
    "$(count bad_rc "$path") wrong exit codes, $(count lost "$path") lost the value"
done
if [ "$failed" = 0 ]; then
  echo "[exit-e2e] OVERALL PASS"
else
  [ "$hung" = 0 ] || echo "[exit-e2e] stopped at a hang"
  echo "[exit-e2e] OVERALL FAIL"
  exit 1
fi
