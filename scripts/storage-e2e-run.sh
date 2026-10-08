#!/usr/bin/env bash
#
# Per-app web storage e2e: relaunches a backend with the storage_e2e_runtime
# under different LAUFEY_APP_ID / LAUFEY_DATA_DIR settings and asserts that
# localStorage + cookies persist per app, stay isolated between apps, and that
# an unconfigured launch behaves as before. See docs/app-data.md. Then does the
# same from a launch file (laufey-launch.json next to the executable, no
# LAUFEY_* environment; docs/launch-config.md), on CEF over a custom scheme the
# file declares.
#
#   scripts/storage-e2e-run.sh <webview|cef>
#
# Build first: `cargo build --release -p storage_e2e_runtime` and the backend.
# Uses fixed app ids (dev.laufey.e2e.storage-{a,b,launch}) and removes their
# data directories and the launch file when done; every run writes a fresh random value, so data left
# by an earlier run can't make a check pass.
set -euo pipefail

backend="${1:?usage: storage-e2e-run.sh <webview|cef>}"

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

os="$(uname -s)"
case "$os" in
  Darwin) platform=macos ;;
  Linux) platform=linux ;;
  *) platform=windows ;;
esac

# Scratch root at a fixed path: WKWebView keys its store on the
# LAUFEY_DATA_DIR string, so a fixed path reuses one store across runs.
if [ "$platform" = windows ]; then
  scratch="$(cygpath -u "${TEMP:-/tmp}")/laufey-storage-e2e"
else
  scratch="${TMPDIR:-/tmp}"
  scratch="${scratch%/}/laufey-storage-e2e"
fi
rm -rf "$scratch"
mkdir -p "$scratch/logs"

# Where LAUFEY_APP_ID resolves to. On Linux point XDG_DATA_HOME into the
# scratch dir so the test exercises the XDG path without touching ~/.local.
case "$platform" in
  macos) base="$HOME/Library/Application Support" ;;
  linux)
    export XDG_DATA_HOME="$scratch/xdg-data"
    mkdir -p "$XDG_DATA_HOME"
    base="$XDG_DATA_HOME" ;;
  windows) base="$(cygpath -u "$LOCALAPPDATA")" ;;
esac

# Backend subdirectory holding the engine profile (none for WKWebView, which
# uses an identifier-based store instead of a directory).
case "$backend/$platform" in
  cef/*) sub=CEF ;;
  webview/linux) sub=WebKitGTK ;;
  webview/windows) sub=WebView2 ;;
  webview/macos) sub="" ;;
esac

id_a=dev.laufey.e2e.storage-a
id_b=dev.laufey.e2e.storage-b
id_f=dev.laufey.e2e.storage-launch
# Where the backend looks for its launch file (docs/launch-config.md).
case "$platform" in
  macos) launch_file="${bin%/MacOS/*}/Resources/laufey-launch.json" ;;
  *) launch_file="$(dirname "$bin")/laufey-launch.json" ;;
esac
# A launch file makes the backend a packaged app, which loads only the
# runtime next to its executable (never LAUFEY_RUNTIME_PATH): while a launch
# file is in place, the runtime is copied there as well.
case "$platform" in
  macos) colocated_rt="$bin.dylib" ;;
  linux) colocated_rt="$bin.so" ;;
  # Windows CEF: <exe>.dll is the host behind CEF's bootstrap.
  windows) [ "$backend" = cef ] && colocated_rt="${bin%.exe}.runtime.dll" ||
    colocated_rt="${bin%.exe}.dll" ;;
esac
write_launch_file() { # <json>
  mkdir -p "$(dirname "$launch_file")"
  printf '%s\n' "$1" >"$launch_file"
  cp "$rt" "$colocated_rt"
  echo "== launch file $launch_file: $(cat "$launch_file")"
}
remove_launch_file() { rm -f "$launch_file" "$colocated_rt"; }
explicit="$scratch/explicit-dir"
if [ "$platform" = windows ]; then
  explicit_native="$(cygpath -w "$explicit")"
else
  explicit_native="$explicit"
fi
port=$((20000 + RANDOM % 20000))
value="v$RANDOM$RANDOM$$"

# Engine helper processes (WebView2, CEF) can hold profile files for a moment
# after the backend exits, so retry, and never fail the run over cleanup.
remove() {
  for _ in 1 2 3 4 5; do
    rm -rf "$1" 2>/dev/null && return 0
    sleep 1
  done
  echo "[storage-e2e] warning: could not remove $1"
}
cleanup() {
  remove_launch_file
  remove "$base/$id_a"
  remove "$base/$id_b"
  remove "$base/$id_f"
  [ -n "${KEEP_SCRATCH:-}" ] || remove "$scratch"
}
trap cleanup EXIT
rm -rf "$base/$id_a" "$base/$id_b" "$base/$id_f"
remove_launch_file

failed=0
pass() { echo "[storage-e2e] PASS $*"; }
fail() { echo "[storage-e2e] FAIL $*"; failed=1; }

# launch <log-name> <timeout-s> [VAR=value ...] -- runs one backend launch with
# only the given LAUFEY_* storage variables set; waits for it to exit.
launch() {
  local name="$1" secs="$2"
  shift 2
  local log="$scratch/logs/$name.log"
  local cmd=(env -u LAUFEY_APP_ID -u LAUFEY_DATA_DIR -u LAUFEY_CUSTOM_SCHEMES
    -u LAUFEY_E2E_STORAGE_EXPECT -u LAUFEY_E2E_STORAGE_EXPECT_NOT
    -u LAUFEY_E2E_STORAGE_HOLD_MS -u LAUFEY_E2E_STORAGE_SCHEME
    LAUFEY_E2E_STORAGE_PORT="$port" "$@")
  if [ "$platform" = linux ]; then
    cmd+=(xvfb-run -a dbus-run-session -- "$bin")
  else
    cmd+=("$bin")
  fi
  echo "== [$name] ${*}"
  "${cmd[@]}" >"$log" 2>&1 &
  local pid=$!
  ( sleep "$secs"; kill -9 "$pid" 2>/dev/null ) &
  local watchdog=$!
  local rc=0
  wait "$pid" || rc=$?
  kill "$watchdog" 2>/dev/null || true
  wait "$watchdog" 2>/dev/null || true
  grep -E '^\[e2e\]|^laufey:' "$log" | sed "s/^/    /" || true
  echo "    (exit $rc)"
}

# step <name> [VAR=value ...] -- one launch that must report OVERALL PASS.
step() {
  local name="$1"
  shift
  launch "$name" 90 "$@"
  if grep -q '^\[e2e\] OVERALL PASS' "$scratch/logs/$name.log"; then
    pass "$name"
  else
    fail "$name (see $scratch/logs/$name.log)"
    if [ -n "${CI:-}" ]; then sed 's/^/    | /' "$scratch/logs/$name.log" | tail -60; fi
  fi
}

echo "== storage-e2e: backend=$backend bin=$bin port=$port value=$value"

# (a) persistence: the same app id reads back what an earlier launch wrote.
step write-a LAUFEY_APP_ID="$id_a" LAUFEY_E2E_STORAGE_MODE=write \
  LAUFEY_E2E_STORAGE_VALUE="$value"
step read-a LAUFEY_APP_ID="$id_a" LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT="$value"

# (b) isolation: another app id, same origin, doesn't see it.
step read-b LAUFEY_APP_ID="$id_b" LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT_NOT="$value"

# LAUFEY_DATA_DIR: persists on its own, isolated from the app-id store.
step write-dir LAUFEY_DATA_DIR="$explicit_native" LAUFEY_E2E_STORAGE_MODE=write \
  LAUFEY_E2E_STORAGE_VALUE="${value}d"
step read-dir LAUFEY_DATA_DIR="$explicit_native" LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT="${value}d"

# (c) unconfigured: nothing of the configured apps leaks into the default
# store, and nothing is created for it.
step read-default LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT_NOT="$value"
if [ "$backend" = cef ]; then
  # CEF's unconfigured profile is a throwaway per-process temp dir, as before.
  step write-default LAUFEY_E2E_STORAGE_MODE=write \
    LAUFEY_E2E_STORAGE_VALUE="${value}u"
  step read-default-again LAUFEY_E2E_STORAGE_MODE=read \
    LAUFEY_E2E_STORAGE_EXPECT_NOT="${value}u"
fi

# On-disk layout.
if [ -n "$sub" ]; then
  if [ -d "$base/$id_a/$sub" ]; then pass "profile at <app data>/$id_a/$sub"; else fail "no profile at $base/$id_a/$sub"; fi
  if [ -d "$explicit/$sub" ]; then pass "profile at \$LAUFEY_DATA_DIR/$sub"; else fail "no profile at $explicit/$sub"; fi
  if [ "$sub" = WebKitGTK ]; then
    if [ -f "$base/$id_a/WebKitGTK/data/cookies.sqlite" ]; then
      pass "persistent cookie store at WebKitGTK/data/cookies.sqlite"
    else
      fail "no cookie store at $base/$id_a/WebKitGTK/data/cookies.sqlite"
    fi
  fi
  if [ "$platform" != windows ]; then
    mode="$(stat -c %a "$base/$id_a/$sub" 2>/dev/null || stat -f %Lp "$base/$id_a/$sub")"
    if [ "$mode" = 700 ]; then pass "profile dir is owner-only (0700)"; else fail "profile dir mode is $mode, want 700"; fi
  fi
else
  if [ ! -e "$base/$id_a" ]; then pass "no data dir created (WKWebView uses an identifier-based store)"; else fail "unexpected $base/$id_a"; fi
fi

# macOS CEF runs Chromium with its mock keychain (cef/src/app.h): the key that
# encrypts cookies on disk is the same on every install (PBKDF2-SHA1 of
# "mock_password", salt "saltysalt", 1003 rounds; AES-128-CBC, an IV of 16
# spaces), so platform_features reports "cookieEncryption": "basic". Check the
# claim: the cookie write-a stored decrypts with that key.
if [ "$backend" = cef ] && [ "$platform" = macos ]; then
  hex=""
  for db in "$base/$id_a/CEF/Default/Cookies" "$base/$id_a/CEF/Default/Network/Cookies"; do
    [ -f "$db" ] || continue
    hex="$(sqlite3 -readonly "$db" \
      "SELECT hex(encrypted_value) FROM cookies WHERE name='laufey_e2e_storage'" 2>/dev/null | head -1)"
    [ -z "$hex" ] || break
  done
  plain=""
  if [ "${hex:0:6}" = 763130 ]; then # "v10"
    plain="$(printf '%s' "${hex:6}" | xxd -r -p |
      openssl enc -d -aes-128-cbc -K af0f762aaf6d7d11581b7aa8ce7218de \
        -iv 20202020202020202020202020202020 2>/dev/null | tail -c +33 || true)"
  fi
  if [ "$plain" = "$value" ]; then
    pass "a stored cookie decrypts with Chromium's mock-keychain key (cookieEncryption \"basic\" is true)"
  else
    fail "the stored cookie didn't decrypt with the mock-keychain key (prefix ${hex:0:6}, got '$plain')"
  fi
fi

# CEF allows one process per profile: a second launch of the same app must
# exit cleanly while the first keeps running.
if [ "$backend" = cef ]; then
  launch hold-a 90 LAUFEY_APP_ID="$id_a" LAUFEY_E2E_STORAGE_MODE=read \
    LAUFEY_E2E_STORAGE_EXPECT="$value" LAUFEY_E2E_STORAGE_HOLD_MS=15000 &
  hold_pid=$!
  for _ in $(seq 1 60); do
    grep -q 'page loaded' "$scratch/logs/hold-a.log" 2>/dev/null && break
    sleep 0.5
  done
  launch second-a 30 LAUFEY_APP_ID="$id_a" LAUFEY_E2E_STORAGE_MODE=read
  if grep -q 'another instance is already running' "$scratch/logs/second-a.log" &&
    ! grep -q '^\[e2e\]' "$scratch/logs/second-a.log"; then
    pass "second instance of the same app exits without starting"
  else
    fail "second instance (see $scratch/logs/second-a.log)"
  fi
  wait "$hold_pid" || true
  if grep -q '^\[e2e\] OVERALL PASS' "$scratch/logs/hold-a.log"; then
    pass "first instance unaffected by the second launch"
  else
    fail "first instance (see $scratch/logs/hold-a.log)"
  fi
fi

# (d) Launch file: the same configuration from laufey-launch.json next to the
# executable, with no LAUFEY_* variable in the environment (an app started
# directly). On CEF the page is served over a custom scheme that only the file
# declares, so it must come up as a secure `<scheme>://app` origin.
launch_scheme=laufey-e2e-launch
scheme_env=()
if [ "$backend" = cef ]; then
  scheme_env=(LAUFEY_E2E_STORAGE_SCHEME="$launch_scheme")
fi
write_launch_file \
  "$(printf '{ "appId": "%s", "customSchemes": ["%s"] }' "$id_f" "$launch_scheme")"
step file-write ${scheme_env[@]+"${scheme_env[@]}"} \
  LAUFEY_E2E_STORAGE_MODE=write LAUFEY_E2E_STORAGE_VALUE="${value}f"
step file-read ${scheme_env[@]+"${scheme_env[@]}"} \
  LAUFEY_E2E_STORAGE_MODE=read LAUFEY_E2E_STORAGE_EXPECT="${value}f"
if [ -n "$sub" ]; then
  if [ -d "$base/$id_f/$sub" ]; then pass "launch file appId: profile at <app data>/$id_f/$sub"; else fail "launch file appId: no profile at $base/$id_f/$sub"; fi
fi
# The file pins the app id: a LAUFEY_APP_ID in the environment (one
# inherited from another app) doesn't move the app into app b's store.
step file-env-override ${scheme_env[@]+"${scheme_env[@]}"} \
  LAUFEY_APP_ID="$id_b" LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT="${value}f"
# The pinned app id pins the data directory too: a LAUFEY_DATA_DIR in the
# environment (the explicit store, which holds "${value}d") is reported and
# ignored, and the app keeps the app id's store.
step file-env-data-dir ${scheme_env[@]+"${scheme_env[@]}"} \
  LAUFEY_DATA_DIR="$explicit_native" LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT="${value}f"
if grep -q 'LAUFEY_DATA_DIR is ignored' "$scratch/logs/file-env-data-dir.log"; then
  pass "LAUFEY_DATA_DIR ignored under a pinned app id is reported"
else
  fail "ignored LAUFEY_DATA_DIR not reported (see $scratch/logs/file-env-data-dir.log)"
fi
# A malformed file is reported and ignored: the app starts with the
# unconfigured default store.
printf '{ "appId": ' >"$launch_file"
step file-malformed LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT_NOT="${value}f"
if grep -q 'laufey-launch.json: not valid JSON' "$scratch/logs/file-malformed.log"; then
  pass "malformed launch file reported on stderr"
else
  fail "malformed launch file not reported (see $scratch/logs/file-malformed.log)"
fi
remove_launch_file

if [ "$failed" = 0 ]; then
  echo "[storage-e2e] OVERALL PASS"
else
  echo "[storage-e2e] OVERALL FAIL"
  exit 1
fi
