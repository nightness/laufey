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
#       its arguments and working directory, even with
#       LAUFEY_SINGLE_INSTANCE=0 and LAUFEY_DATA_DIR in its environment (the
#       file pins the app id, and with it the lock and the data directory).
#       An invalid app id setup warns and runs unlocked; without an app id in
#       the file, LAUFEY_SINGLE_INSTANCE=0 overrides it. A headless worker
#       launch while the primary runs (`<exe> run <script>`, how Deno Desktop
#       starts its update helper, or `<exe> <script>` with NODE_CHANNEL_FD, a
#       forked worker) is not forwarded and never reaches the lock: it runs
#       the runtime headless and exits.
#   (g) a launch while the primary is ending (quit() called, still holding
#       the lock): refused by the primary, it becomes the primary itself and
#       runs, instead of being acknowledged and lost.
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
rt_file="$rt"

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
# A path for a JSON string: forward slashes on Windows too (C:/...), so
# nothing needs escaping.
json_path() {
  if [ "$platform" = windows ]; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

app_id=dev.laufey.e2e.single-instance
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
  *) [ "$backend" = cef ] && colocated_rt="${bin%.exe}.runtime.dll" ||
    colocated_rt="${bin%.exe}.dll" ;;
esac
write_launch_file() { # <json>
  mkdir -p "$(dirname "$launch_file")"
  printf '%s\n' "$1" >"$launch_file"
  cp "$rt_file" "$colocated_rt"
  echo "== launch file $launch_file: $(cat "$launch_file")"
}
remove_launch_file() { rm -f "$launch_file" "$colocated_rt"; }

pids=()
cleanup() {
  for p in ${pids[@]+"${pids[@]}"}; do kill -9 "$p" 2>/dev/null || true; done
  remove_launch_file
  if [ -z "${KEEP_SCRATCH:-}" ]; then
    for _ in 1 2 3 4 5; do rm -rf "$scratch" 2>/dev/null && break; sleep 1; done
  fi
}
trap cleanup EXIT
remove_launch_file

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
# The file pins the app id, so its data directory and lock come from the file
# alone: the second launch below can't turn the lock off or move the profile
# from its environment.
write_launch_file "$(printf '{ "appId": "%s", "dataDir": "%s", "singleInstance": true }' \
  "$app_id" "$(json_path "$scratch/data-primary")")"

# The primary stays up until released (after the worker launch below), so
# that launch meets a held lock.
start primary \
  "${cold_expect[@]}" "${second_expect[@]}" \
  LAUFEY_E2E_SI_SECOND_CWD="$cwd_native" \
  LAUFEY_E2E_SI_HOLD_FILE="$(native "$scratch/release-primary")" -- "${cold_args[@]}"
primary_pid=$started_pid
# The Windows CEF executable is CEF's bootstrap, which moves the process to
# the executable's directory before laufey runs; laufey changes back to the
# directory LAUFEY_CWD names (docs/backends.md). A launch without it starts in
# the executable's directory.
second_cwd_env=()
if [ "$platform" = windows ] && [ "$backend" = cef ]; then
  second_cwd_env=(LAUFEY_CWD="$cwd_native")
fi
if wait_for primary '^\[e2e\] ready' 90; then
  direct second "$cwd_dir" LAUFEY_SINGLE_INSTANCE=0 ${second_cwd_env[@]+"${second_cwd_env[@]}"} \
    LAUFEY_DATA_DIR="$(native "$scratch/data-second")" -- "${second_args[@]}"
  if [ "$direct_rc" = 0 ] && [ "$direct_secs" -le 10 ] &&
    ! grep -q '^\[e2e\]' "$scratch/logs/second.log"; then
    pass "second launch forwarded and exited 0 without loading the runtime (${direct_secs}s)"
  else
    fail "second launch (exit $direct_rc after ${direct_secs}s; see $scratch/logs/second.log)"
  fi
  if grep -q 'LAUFEY_SINGLE_INSTANCE is ignored' "$scratch/logs/second.log"; then
    pass "LAUFEY_SINGLE_INSTANCE=0 can't turn off the lock of a pinned app id (reported)"
  else
    fail "ignored LAUFEY_SINGLE_INSTANCE not reported (see $scratch/logs/second.log)"
  fi
  # The lock is held: a worker launch must still run (headless, no window),
  # not be forwarded to the primary as a second instance.
  direct worker "$cwd_dir" -- run worker-script.ts worker-arg
  if ! kill -0 "$primary_pid" 2>/dev/null; then
    fail "the primary exited before the worker launch finished"
  elif [ "$direct_rc" = 0 ] &&
    grep -q '^\[e2e\] headless worker args=\["run", "worker-script.ts", "worker-arg"\]' \
      "$scratch/logs/worker.log"; then
    pass "worker launch (run <script>) ran headless while the primary holds the lock (${direct_secs}s)"
  else
    fail "worker launch not run headless (exit $direct_rc; see $scratch/logs/worker.log)"
  fi
  # A worker launched the env way (spawn(execPath, [script], ipc) in Deno
  # Desktop: no `run`, only NODE_CHANNEL_FD marks it) runs headless too,
  # before the single-instance check: not forwarded, and it never reaches the
  # lock (which would report the LAUFEY_SINGLE_INSTANCE it can't override).
  direct env-worker "$cwd_dir" NODE_CHANNEL_FD=3 LAUFEY_SINGLE_INSTANCE=0 \
    -- worker-script.ts env-arg
  if ! kill -0 "$primary_pid" 2>/dev/null; then
    fail "the primary exited before the env worker launch finished"
  elif [ "$direct_rc" = 0 ] &&
    grep -q '^\[e2e\] headless worker args=\["worker-script.ts", "env-arg"\]' \
      "$scratch/logs/env-worker.log" &&
    ! grep -q 'LAUFEY_SINGLE_INSTANCE is ignored' "$scratch/logs/env-worker.log"; then
    pass "worker launch (NODE_CHANNEL_FD, no run) ran headless without reaching the lock (${direct_secs}s)"
  else
    fail "env worker launch not run headless (exit $direct_rc; see $scratch/logs/env-worker.log)"
  fi
else
  fail "primary never became ready"
fi
touch "$scratch/release-primary"
finish primary "$primary_pid" 60

# (g) A launch while the primary is ending (quit() called, the process still
# alive and holding the lock for a few seconds): the primary refuses it
# (kSingleInstanceEnding) instead of acknowledging a launch it would never
# deliver, and the launch becomes the primary itself once the lock is free.
late_args=("late-arg" "acme://late/start")
late_expect=()
while IFS= read -r line; do late_expect+=("$line"); done \
  < <(expect_vars LAUFEY_E2E_SI_COLD "${late_args[@]}")
start ending LAUFEY_E2E_SI_LINGER_MS=4000 \
  LAUFEY_E2E_SI_HOLD_FILE="$(native "$scratch/release-ending")" --
ending_pid=$started_pid
if wait_for ending '^\[e2e\] ready' 90; then
  touch "$scratch/release-ending"
  if wait_for ending '^\[e2e\] quitting; lingering' 30; then
    sleep 0.5
    if kill -0 "$ending_pid" 2>/dev/null; then
      start late "${late_expect[@]}" LAUFEY_E2E_SI_HOLD_MS=500 -- "${late_args[@]}"
      late_pid=$started_pid
      finish late "$late_pid" 90
      # Forwarded to the ending primary and acknowledged, it would have exited
      # without running the runtime (no [e2e] lines).
      if grep -q '^\[e2e\] OVERALL PASS' "$scratch/logs/late.log"; then
        pass "a launch while the primary was ending became the primary itself"
      else
        fail "the launch while the primary was ending didn't run (see $scratch/logs/late.log)"
      fi
    else
      fail "the ending primary exited before the late launch (raise LINGER)"
    fi
  else
    fail "the ending primary never quit"
  fi
else
  fail "ending never became ready"
fi
finish ending "$ending_pid" 60

# Without an app id in the file, LAUFEY_SINGLE_INSTANCE=0 wins over its
# singleInstance: two instances (CEF: separate profiles). The first stays up
# until released through its hold file, not for a fixed time: a second
# instance on a fresh profile can take several seconds to start on a slow
# runner, and a timed hold then ran out before it finished.
write_launch_file '{ "singleInstance": true }'
start env-off-a LAUFEY_APP_ID="$app_id" LAUFEY_SINGLE_INSTANCE=0 \
  LAUFEY_DATA_DIR="$(native "$scratch/data-off-a")" \
  LAUFEY_E2E_SI_HOLD_FILE="$(native "$scratch/release-env-off-a")" --
a_pid=$started_pid
if wait_for env-off-a '^\[e2e\] ready' 90; then
  start env-off-b LAUFEY_APP_ID="$app_id" LAUFEY_SINGLE_INSTANCE=0 \
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
remove_launch_file

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
  bin_native="$(native "$bin")"
  if [ "$platform" = windows ]; then
    key='HKCU\Software\Classes\'"$scheme"
    reg add "$key" /ve /d "URL:laufey e2e" /f >/dev/null
    reg add "$key" /v "URL Protocol" /d "" /f >/dev/null
    # The registered form: options end at "--", the link after it is a
    # positional argument. The launch file below makes this a packaged app,
    # which loads the runtime next to its executable (write_launch_file puts
    # it there), never one a command line or LAUFEY_RUNTIME_PATH names.
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
  write_launch_file "$(printf '{ "appId": "%s", "dataDir": "%s", "singleInstance": true }' \
    "$app_id" "$(json_path "$scratch/data-os")")"

  # Cold start: the OS starts the app with the URL; the runtime reads it from
  # its arguments. Its output goes to a result file (Windows doesn't hand an
  # OS-started process our stderr).
  result="$scratch/os-cold.result"
  echo "== [os-cold] open $url_cold through the OS"
  (
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
  start os-warm LAUFEY_E2E_SI_WAIT_MS=60000 "${warm_expect[@]}" --
  warm_pid=$started_pid
  if wait_for os-warm '^\[e2e\] ready' 90; then
    echo "== [os-warm] open $url_warm through the OS while it runs"
    # Should the OS-started launch not forward and load the runtime itself
    # instead, its own verdict lands here (it must stay empty).
    stray="$scratch/os-warm-second.result"
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
  remove_launch_file
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
