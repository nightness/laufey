#!/usr/bin/env bash
#
# Single-instance / deep-link e2e (docs/deep-links.md). Drives a backend with
# the single_instance_e2e_runtime:
#
#   (a) cold start: the runtime sees the arguments the backend was started
#       with (std::env::args()).
#   (b) singleInstance on (from laufey-launch.json): a second launch, started
#       without a display on Linux, exits 0 quickly without loading the
#       runtime, and the first instance gets `second_instance` with exactly
#       its arguments and working directory. An invalid app id setup warns
#       and runs unlocked; LAUFEY_SINGLE_INSTANCE=0 overrides the file.
#   (c) singleInstance off: two instances run side by side (on CEF with
#       separate data directories: one CEF profile allows one process, see
#       docs/app-data.md; scripts/storage-e2e-run.sh covers that refusal).
#   (f) Windows and Linux: a test URL scheme registered with the OS the way
#       an installer does (HKCU\Software\Classes on Windows; a .desktop file
#       with x-scheme-handler/ and xdg-mime on Linux), then a link opened
#       through the OS (Start-Process / xdg-open): at a cold start the
#       runtime sees the URL in its arguments, and while an instance runs
#       the OS-started second launch forwards it to `second_instance`.
#   (e) macOS: a file opened with the bundle through LaunchServices
#       (`open -a <App>.app <file>`) at a cold start, then a custom-scheme URL
#       and another file while it runs, reach `open_url` (files as file://
#       URLs), and a file passed on the command line of a directly exec'd
#       binary reaches argv only, not `open_url`.
#
# (d), the buffered open_url round trip (test_trigger_open_url), is part of
# scripts/native-e2e-run.sh.
#
#   scripts/single-instance-e2e-run.sh <webview|cef>
#
# Build first: `cargo build --release -p single_instance_e2e_runtime` and the
# backend.
set -euo pipefail

backend="${1:?usage: single-instance-e2e-run.sh <webview|cef>}"

rt=""
for c in \
  target/release/libsingle_instance_e2e_runtime.so \
  target/release/libsingle_instance_e2e_runtime.dylib \
  target/release/single_instance_e2e_runtime.dll; do
  if [ -f "$c" ]; then rt="$PWD/$c"; break; fi
done
[ -n "$rt" ] || { echo "single_instance_e2e_runtime cdylib not found (build it first)"; exit 1; }

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
bin="$PWD/$bin"

case "$(uname -s)" in
  Darwin) platform=macos ;;
  Linux) platform=linux ;;
  *) platform=windows ;;
esac

if [ "$platform" = windows ]; then
  # Keep Git bash from rewriting arguments that look like paths.
  export MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1
  rt="$(cygpath -w "$rt")"
  scratch="$(cygpath -u "${TEMP:-/tmp}")/laufey-si-e2e"
else
  scratch="${TMPDIR:-/tmp}"
  scratch="${scratch%/}/laufey-si-e2e"
fi
rm -rf "$scratch"
mkdir -p "$scratch/logs"

native() {
  if [ "$platform" = windows ]; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

app_id=dev.laufey.e2e.single-instance
case "$platform" in
  macos) launch_file="${bin%/MacOS/*}/Resources/laufey-launch.json" ;;
  *) launch_file="$(dirname "$bin")/laufey-launch.json" ;;
esac

pids=()
cleanup() {
  for p in ${pids[@]+"${pids[@]}"}; do kill -9 "$p" 2>/dev/null || true; done
  rm -f "$launch_file"
  if [ -z "${KEEP_SCRATCH:-}" ]; then
    for _ in 1 2 3 4 5; do rm -rf "$scratch" 2>/dev/null && break; sleep 1; done
  fi
}
trap cleanup EXIT
rm -f "$launch_file"

failed=0
pass() { echo "[si-e2e] PASS $*"; }
fail() { echo "[si-e2e] FAIL $*"; failed=1; }

# Only the variables a step sets reach the backend.
clean_env=(env -u LAUFEY_APP_ID -u LAUFEY_DATA_DIR -u LAUFEY_SINGLE_INSTANCE
  -u LAUFEY_CUSTOM_SCHEMES LAUFEY_RUNTIME_PATH="$rt")

# start <name> [VAR=value ...] -- [args...]: starts one backend instance in
# the background (under Xvfb on Linux) with the runtime; sets $started_pid.
start() {
  local name="$1"
  shift
  local vars=()
  while [ $# -gt 0 ] && [ "$1" != -- ]; do vars+=("$1"); shift; done
  [ $# -gt 0 ] && shift
  local cmd=("${clean_env[@]}" ${vars[@]+"${vars[@]}"})
  if [ "$platform" = linux ]; then
    cmd+=(xvfb-run -a dbus-run-session -- "$bin")
  else
    cmd+=("$bin")
  fi
  echo "== [$name] ${vars[*]-} -- $*"
  "${cmd[@]}" "$@" >"$scratch/logs/$name.log" 2>&1 &
  started_pid=$!
  pids+=("$started_pid")
}

# wait_for <name> <pattern> <seconds>: waits until the log matches.
wait_for() {
  local i
  for ((i = 0; i < $3 * 5; i++)); do
    grep -q "$2" "$scratch/logs/$1.log" 2>/dev/null && return 0
    sleep 0.2
  done
  return 1
}

# finish <name> <pid> <seconds>: waits for the process (killing it after the
# timeout), prints its [e2e] lines, and checks OVERALL PASS.
finish() {
  local name="$1" pid="$2" secs="$3" i
  for ((i = 0; i < secs * 5; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.2
  done
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  grep -E '^\[e2e\]|^laufey:' "$scratch/logs/$name.log" | sed 's/^/    /' || true
  if grep -q '^\[e2e\] OVERALL PASS' "$scratch/logs/$name.log"; then
    pass "$name"
  else
    fail "$name (see $scratch/logs/$name.log)"
    if [ -n "${CI:-}" ]; then sed 's/^/    | /' "$scratch/logs/$name.log" | tail -60; fi
  fi
}

# direct <name> <cwd> [VAR=value ...] -- [args...]: runs the backend in the
# foreground from <cwd> without a display (Linux) or runtime window, with a
# timeout; sets $direct_rc and $direct_secs.
direct() {
  local name="$1" dir="$2"
  shift 2
  local vars=()
  while [ $# -gt 0 ] && [ "$1" != -- ]; do vars+=("$1"); shift; done
  [ $# -gt 0 ] && shift
  echo "== [$name] (cwd $dir) ${vars[*]-} -- $*"
  local t0 t1
  t0=$(date +%s)
  direct_rc=0
  (
    cd "$dir"
    if [ "$platform" = linux ]; then unset DISPLAY WAYLAND_DISPLAY; fi
    exec "${clean_env[@]}" ${vars[@]+"${vars[@]}"} "$bin" "$@"
  ) >"$scratch/logs/$name.log" 2>&1 &
  local pid=$!
  pids+=("$pid")
  local i
  for ((i = 0; i < 30 * 5; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.2
  done
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" || direct_rc=$?
  t1=$(date +%s)
  direct_secs=$((t1 - t0))
  sed 's/^/    /' "$scratch/logs/$name.log" | head -20
  echo "    (exit $direct_rc after ${direct_secs}s)"
}

echo "== single-instance-e2e: backend=$backend bin=$bin"

cold_args=("cold-arg" "acme://cold/start?x=1&y=2")
second_args=("second arg with spaces" "acme://open/doc?id=42&y=2" "caf$(printf '\xc3\xa9')")
cwd_dir="$scratch/cwd dir"
mkdir -p "$cwd_dir"
if [ "$platform" = windows ]; then
  cwd_native="$(cd "$cwd_dir" && cygpath -w "$(pwd -P)")"
else
  cwd_native="$(cd "$cwd_dir" && pwd -P)"
fi
expect_vars() { # <prefix> args...
  local prefix="$1" i=0
  shift
  echo "${prefix}_ARGC=$#"
  for a in "$@"; do echo "${prefix}_ARG_$i=$a"; i=$((i + 1)); done
}
# (No mapfile: macOS ships bash 3.2.)
cold_expect=()
while IFS= read -r line; do cold_expect+=("$line"); done \
  < <(expect_vars LAUFEY_E2E_SI_COLD "${cold_args[@]}")
second_expect=()
while IFS= read -r line; do second_expect+=("$line"); done \
  < <(expect_vars LAUFEY_E2E_SI_SECOND "${second_args[@]}")

# --- (a) + (b): the lock, configured by the launch file ----------------------
mkdir -p "$(dirname "$launch_file")"
printf '{ "appId": "%s", "singleInstance": true }\n' "$app_id" >"$launch_file"
echo "== launch file $launch_file: $(cat "$launch_file")"

start primary LAUFEY_DATA_DIR="$(native "$scratch/data-primary")" \
  "${cold_expect[@]}" "${second_expect[@]}" \
  LAUFEY_E2E_SI_SECOND_CWD="$cwd_native" -- "${cold_args[@]}"
primary_pid=$started_pid
if wait_for primary '^\[e2e\] ready' 90; then
  direct second "$cwd_dir" -- "${second_args[@]}"
  if [ "$direct_rc" = 0 ] && [ "$direct_secs" -le 10 ] &&
    ! grep -q '^\[e2e\]' "$scratch/logs/second.log"; then
    pass "second launch forwarded and exited 0 without loading the runtime (${direct_secs}s)"
  else
    fail "second launch (exit $direct_rc after ${direct_secs}s; see $scratch/logs/second.log)"
  fi
else
  fail "primary never became ready"
fi
finish primary "$primary_pid" 60

# LAUFEY_SINGLE_INSTANCE=0 wins over the file: two instances (CEF: separate
# profiles). The first stays up until released through its hold file, not for
# a fixed time: a second instance on a fresh profile can take several seconds
# to start on a slow runner, and a timed hold then ran out before it finished.
start env-off-a LAUFEY_SINGLE_INSTANCE=0 \
  LAUFEY_DATA_DIR="$(native "$scratch/data-off-a")" \
  LAUFEY_E2E_SI_HOLD_FILE="$(native "$scratch/release-env-off-a")" --
a_pid=$started_pid
if wait_for env-off-a '^\[e2e\] ready' 90; then
  start env-off-b LAUFEY_SINGLE_INSTANCE=0 \
    LAUFEY_DATA_DIR="$(native "$scratch/data-off-b")" LAUFEY_E2E_SI_HOLD_MS=500 --
  finish env-off-b "$started_pid" 90
  if kill -0 "$a_pid" 2>/dev/null; then
    pass "LAUFEY_SINGLE_INSTANCE=0 overrides the file: both instances ran together"
  else
    fail "first instance exited before the second finished"
  fi
else
  fail "env-off-a never became ready"
fi
touch "$scratch/release-env-off-a"
finish env-off-a "$a_pid" 60
rm -f "$launch_file"

# Without an app id the lock can't be keyed: warn and run unlocked.
start no-app-id LAUFEY_SINGLE_INSTANCE=1 --
finish no-app-id "$started_pid" 60
if grep -q 'single-instance mode needs an app id' "$scratch/logs/no-app-id.log"; then
  pass "missing app id reported"
else
  fail "missing app id not reported (see $scratch/logs/no-app-id.log)"
fi

# --- (c): no singleInstance, two instances side by side -----------------------
side_a_dir=(LAUFEY_DATA_DIR="$(native "$scratch/data-side-a")")
side_b_dir=(LAUFEY_DATA_DIR="$(native "$scratch/data-side-b")")
if [ "$backend" = webview ]; then
  # Same app and data dir: nothing stops a second WebView instance.
  side_b_dir=("${side_a_dir[@]}")
fi
start side-a LAUFEY_APP_ID="$app_id" "${side_a_dir[@]}" \
  LAUFEY_E2E_SI_HOLD_FILE="$(native "$scratch/release-side-a")" --
a_pid=$started_pid
if wait_for side-a '^\[e2e\] ready' 90; then
  start side-b LAUFEY_APP_ID="$app_id" "${side_b_dir[@]}" LAUFEY_E2E_SI_HOLD_MS=500 --
  finish side-b "$started_pid" 90
  if kill -0 "$a_pid" 2>/dev/null; then
    pass "without singleInstance two instances run side by side"
  else
    fail "first instance exited before the second finished"
  fi
else
  fail "side-a never became ready"
fi
touch "$scratch/release-side-a"
finish side-a "$a_pid" 60

# --- (f): a registered URL scheme opened through the OS (Windows, Linux) ------
if [ "$platform" != macos ]; then
  # Letters only: xdg-open takes a "scheme" with digits for a file name.
  scheme=laufey-si-test
  url_cold="$scheme://open/cold?id=7"
  url_warm="$scheme://open/doc?id=42"
  rt_native="$(native "$rt")"
  bin_native="$(native "$bin")"
  if [ "$platform" = windows ]; then
    key='HKCU\Software\Classes\'"$scheme"
    reg add "$key" /ve /d "URL:laufey e2e" /f >/dev/null
    reg add "$key" /v "URL Protocol" /d "" /f >/dev/null
    # The registered form: options end at "--", the link after it is a
    # positional argument. The launch file below makes this a packaged app,
    # which takes its runtime from LAUFEY_RUNTIME_PATH, never a command line.
    reg add "$key\shell\open\command" /ve \
      /d "\"$bin_native\" -- \"%1\"" /f >/dev/null
    echo "== registered $scheme: $(reg query "$key\shell\open\command" /ve | tr -d '\r' | grep REG_)"
    # ShellExecute, as a browser or `start` does.
    os_open() { powershell -NoProfile -Command "Start-Process '$1'"; }
    unregister() { reg delete "$key" /f >/dev/null 2>&1 || true; }
  else
    # A private XDG home, so the registration stays in this run. xdg-open
    # (no desktop environment: its generic path) asks xdg-mime for the
    # x-scheme-handler/ default and runs the entry's Exec in the foreground
    # with the same environment. Its Exec parsing doesn't handle quotes.
    export XDG_DATA_HOME="$scratch/xdg-data" XDG_CONFIG_HOME="$scratch/xdg-config"
    mkdir -p "$XDG_DATA_HOME/applications" "$XDG_CONFIG_HOME"
    case "$bin$rt" in
      *" "*) echo "paths with spaces can't go in this test's Exec line"; exit 1 ;;
    esac
    cat >"$XDG_DATA_HOME/applications/$scheme.desktop" <<EOF2
[Desktop Entry]
Type=Application
Name=laufey e2e
Exec=$bin %u
MimeType=x-scheme-handler/$scheme;
NoDisplay=true
EOF2
    xdg-mime default "$scheme.desktop" "x-scheme-handler/$scheme"
    echo "== registered $scheme: $(xdg-mime query default "x-scheme-handler/$scheme")"
    os_open() {
      xvfb-run -a dbus-run-session -- env -u LAUFEY_SINGLE_INSTANCE xdg-open "$1"
    }
    unregister() { :; }
  fi
  printf '{ "appId": "%s", "singleInstance": true }\n' "$app_id" >"$launch_file"

  # Cold start: the OS starts the app with the URL; the runtime reads it from
  # its arguments. Its output goes to a result file (Windows doesn't hand an
  # OS-started process our stderr).
  result="$scratch/os-cold.result"
  echo "== [os-cold] open $url_cold through the OS"
  (
    export LAUFEY_DATA_DIR="$(native "$scratch/data-os")"
    export LAUFEY_RUNTIME_PATH="$rt_native"
    # Windows registers "-- %1"; a .desktop Exec passes %u as one argument
    # (GTK would drop a "--" from argv anyway).
    if [ "$platform" = windows ]; then
      export LAUFEY_E2E_SI_COLD_ARGC=2 LAUFEY_E2E_SI_COLD_ARG_0=-- \
        LAUFEY_E2E_SI_COLD_ARG_1="$url_cold"
    else
      export LAUFEY_E2E_SI_COLD_ARGC=1 LAUFEY_E2E_SI_COLD_ARG_0="$url_cold"
    fi
    export \
      LAUFEY_E2E_SI_HOLD_MS=500 LAUFEY_E2E_SI_RESULT_FILE="$(native "$result")"
    os_open "$url_cold"
  ) >"$scratch/logs/os-cold.log" 2>&1 &
  os_pid=$!
  pids+=("$os_pid")
  for ((i = 0; i < 90 * 5; i++)); do
    grep -q '^\[e2e\] OVERALL' "$result" 2>/dev/null && break
    sleep 0.2
  done
  wait "$os_pid" 2>/dev/null || true
  sed 's/^/    /' "$result" 2>/dev/null || true
  if grep -q '^\[e2e\] OVERALL PASS' "$result" 2>/dev/null; then
    pass "a link opened through the OS starts the app with the URL in its arguments"
  else
    fail "os-cold (see $result and $scratch/logs/os-cold.log)"
    sed 's/^/    | /' "$scratch/logs/os-cold.log" | tail -40
  fi
  # Let the cold instance exit (and its lock and profile go) before the next
  # one starts on the same data directory.
  if [ "$platform" = windows ]; then
    sleep 3
  else
    for ((i = 0; i < 50; i++)); do
      pgrep -f "$url_cold" >/dev/null 2>&1 || break
      sleep 0.2
    done
  fi

  # Running: the OS starts a second launch, which forwards the URL (with the
  # rest of its command line) to the running instance.
  if [ "$platform" = windows ]; then
    warm_expect=(LAUFEY_E2E_SI_SECOND_ARGC=2 LAUFEY_E2E_SI_SECOND_ARG_0=--
      LAUFEY_E2E_SI_SECOND_ARG_1="$url_warm")
  else
    warm_expect=(LAUFEY_E2E_SI_SECOND_ARGC=1
      LAUFEY_E2E_SI_SECOND_ARG_0="$url_warm")
  fi
  start os-warm LAUFEY_DATA_DIR="$(native "$scratch/data-os")" \
    LAUFEY_E2E_SI_WAIT_MS=60000 "${warm_expect[@]}" --
  warm_pid=$started_pid
  if wait_for os-warm '^\[e2e\] ready' 90; then
    echo "== [os-warm] open $url_warm through the OS while it runs"
    # Should the OS-started launch not forward and load the runtime itself
    # instead, its own verdict lands here (it must stay empty).
    stray="$scratch/os-warm-second.result"
    LAUFEY_RUNTIME_PATH="$rt_native" \
      LAUFEY_E2E_SI_RESULT_FILE="$(native "$stray")" LAUFEY_E2E_SI_WAIT_MS=1000 \
      os_open "$url_warm" >"$scratch/logs/os-warm-open.log" 2>&1 ||
      echo "    (opener exited $?)"
    sed 's/^/    /' "$scratch/logs/os-warm-open.log" | head -20
    wait_for os-warm '^\[e2e\] second_instance' 60 || true
    if [ -f "$stray" ]; then
      fail "the OS-started second launch ran the runtime itself:"
      sed 's/^/    | /' "$stray"
    fi
  else
    fail "os-warm never became ready"
  fi
  finish os-warm "$warm_pid" 60
  rm -f "$launch_file"
  unregister
fi

# --- (e): macOS LaunchServices ------------------------------------------------
if [ "$platform" = macos ]; then
  app="${bin%/Contents/MacOS/*}"
  file_one="$scratch/file one.txt"
  file_two="$scratch/file two.txt"
  echo one >"$file_one"
  echo two >"$file_two"
  log="$scratch/logs/open-a.log"
  : >"$log"
  warm_url="laufey-e2e-si://open/doc?id=42"
  echo "== [open-a] open -a $app \"$file_one\", then $warm_url and \"$file_two\" while running"
  open -n -a "$app" --stderr "$log" \
    --env LAUFEY_RUNTIME_PATH="$rt" \
    --env LAUFEY_E2E_SI_OPEN_URLS=3 \
    --env LAUFEY_E2E_SI_OPEN_URL_SUFFIX_0="/file%20one.txt" \
    --env LAUFEY_E2E_SI_OPEN_URL_SUFFIX_1="$warm_url" \
    --env LAUFEY_E2E_SI_OPEN_URL_SUFFIX_2="/file%20two.txt" \
    "$file_one"
  if wait_for open-a '^\[e2e\] ready' 90; then
    # `open -a` hands a URL to that app even without a registered scheme.
    open -a "$app" "$warm_url"
    wait_for open-a 'open_url "laufey-e2e-si' 30 || true
    open -a "$app" "$file_two"
  fi
  wait_for open-a '^\[e2e\] OVERALL' 60 || true
  grep -E '^\[e2e\]|^laufey:' "$log" | sed 's/^/    /' || true
  if grep -q '^\[e2e\] OVERALL PASS' "$log"; then
    pass "open -a: a cold-start file, a URL and a file while running reach open_url"
  else
    fail "open -a (see $log)"
  fi
  pkill -f "$bin" 2>/dev/null || true

  # A file on the command line of a directly exec'd binary is argv, not an
  # open-document event.
  start argv-file LAUFEY_E2E_SI_COLD_ARGC=1 LAUFEY_E2E_SI_COLD_ARG_0="$file_one" \
    LAUFEY_E2E_SI_NO_OPEN_URL=1 LAUFEY_E2E_SI_HOLD_MS=3000 -- "$file_one"
  finish argv-file "$started_pid" 60
fi

if [ "$failed" = 0 ]; then
  echo "[si-e2e] OVERALL PASS"
else
  echo "[si-e2e] OVERALL FAIL"
  exit 1
fi
