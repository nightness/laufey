#!/usr/bin/env bash
#
# The Linux hosts end cleanly on SIGTERM, SIGINT and SIGHUP: through the
# app's own quit (windows closed, runtime shut down, the engine's profile and
# web storage written), not the signal's default action (WebKitGTK) or
# Chromium's session-end path (CEF), which _exit()s the browser process and
# leaves the profile marked "SessionEnded" and the child processes orphaned.
#
#   scripts/signal-exit-e2e-run.sh [cef|webview] [--dialog alert|confirm] \
#     [SIGTERM SIGINT SIGHUP]
#
# The backend defaults to cef. Runs the storage_e2e runtime (cargo build
# --release -p storage_e2e_runtime) under Xvfb and a private session bus: it
# writes a fresh value to localStorage and a cookie and then waits. For each
# signal: the host exits within LIMIT seconds (default 15) with status 0
# (not killed by a signal: 143 / 130 / 129, nor 133 for a SIGTRAP crash),
# every process it started is gone, no core file or apport report appeared,
# and the next launch reads the value back. CEF: the profile's exit_type is
# "Normal".
#
# --dialog: the runtime also has a dialog up when the signal comes, from a
# thread of its own (as Deno's uncaught-error handler): the quit ends it as a
# cancel. `alert` then calls laufey::exit(1) as Deno.exit(1) does, so the
# status may be 1; `confirm` must return false.
set -euo pipefail

[ "$(uname -s)" = Linux ] || { echo "Linux only"; exit 2; }
backend=cef
case "${1:-}" in
  cef | webview) backend="$1"; shift ;;
esac
dialog=""
if [ "${1:-}" = --dialog ]; then
  dialog="${2:?--dialog alert|confirm}"
  shift 2
fi
case "$backend" in
  cef) bin="$PWD/cef/build/Release/laufey" ;;
  webview) bin="$PWD/webview/build/laufey_webview" ;;
esac
rt="$PWD/target/release/libstorage_e2e_runtime.so"
[ -x "$bin" ] || { echo "$bin not built (make $backend)"; exit 1; }
[ -f "$rt" ] || { echo "$rt not built (cargo build --release -p storage_e2e_runtime)"; exit 1; }
signals=("$@")
[ "${#signals[@]}" -gt 0 ] || signals=(SIGTERM SIGINT SIGHUP)
limit="${LIMIT:-15}"
extra=()
[ "$backend" = cef ] && extra=(--password-store=basic)

fail=0
check() { # <ok?> <message>
  if [ "$1" = 1 ]; then echo "[e2e] PASS $2"; else echo "[e2e] FAIL $2"; fail=1; fi
}

# Every process below <pid>, recursively.
descendants() {
  local child
  for child in $(pgrep -P "$1" || true); do
    echo "$child"
    descendants "$child"
  done
}

# launch <work> <log> [VAR=value ...]: starts the host in the background;
# its pid goes to <work>/host.pid and its status to <work>/host.status.
launch() {
  local work="$1" log="$2"
  shift 2
  rm -f "$work/host.pid" "$work/host.status"
  # The host is the shell's direct child, so the shell sees its real status.
  (
    cd "$work"
    ulimit -c unlimited || true
    env LAUFEY_DATA_DIR="$work/data" XDG_DATA_HOME="$work/xdg-data" \
      LAUFEY_E2E_STORAGE_PORT="$port" E2E_WORK="$work" "$@" \
      xvfb-run -a dbus-run-session -- sh -c '
      "$@" &
      echo $! >"$E2E_WORK/host.pid"
      wait $!
      echo $? >"$E2E_WORK/host.status"' sh \
      "$bin" --runtime "$rt" ${extra[@]+"${extra[@]}"} >"$log" 2>&1
  ) &
}

# wait_status <work> <seconds>: waits for the host's status.
wait_status() {
  local i
  for ((i = 0; i < $2 * 10; i++)); do
    [ -f "$1/host.status" ] && break
    sleep 0.1
  done
}

now() { date +%s%N; }

echo "== signal-exit-e2e: backend=$backend dialog=${dialog:-none} signals=${signals[*]}"
for sig in "${signals[@]}"; do
  work="$(mktemp -d "${TMPDIR:-/tmp}/laufey-signal-e2e.XXXXXX")"
  mkdir -p "$work/xdg-data"
  port=$((20000 + RANDOM % 20000))
  value="v$RANDOM$RANDOM"
  crash_before="$(ls /var/crash 2>/dev/null | grep -c laufey || true)"
  log="$work/host.log"
  dialog_env=()
  [ -z "$dialog" ] || dialog_env=(LAUFEY_E2E_STORAGE_DIALOG="$dialog")
  launch "$work" "$log" LAUFEY_E2E_STORAGE_MODE=write \
    LAUFEY_E2E_STORAGE_VALUE="$value" LAUFEY_E2E_STORAGE_EXIT=wait \
    ${dialog_env[@]+"${dialog_env[@]}"}
  runner=$!
  # Up: the value is written and the runtime waits (and the dialog is up).
  up=0
  for _ in $(seq 1 600); do
    if [ -s "$work/host.pid" ] && grep -q '^\[e2e\] waiting for the app' "$log" 2>/dev/null &&
      { [ -z "$dialog" ] || grep -q "^\[e2e\] dialog $dialog up" "$log"; }; then
      up=1
      break
    fi
    sleep 0.1
  done
  host="$(cat "$work/host.pid" 2>/dev/null || true)"
  check "$up" "$sig: the app came up and wrote $value (host $host)"
  check "$(grep -q '^\[e2e\] OVERALL PASS' "$log" && echo 1)" "$sig: the write launch passed"
  # Let the dialog map and the page settle.
  sleep 3
  procs="$(descendants "$host" | tr '\n' ' ')"
  start=$(now)
  kill "-${sig#SIG}" "$host" 2>/dev/null || true
  # Long enough to see how long an unfixed host takes (a backstop of 30 s or
  # more), so the failure says so.
  wait_status "$work" 120
  ms=$((($(now) - start) / 1000000))
  status="$(cat "$work/host.status" 2>/dev/null || echo none)"
  hung=""
  if [ "$status" = none ]; then
    # Still running: what is left is checked below, then killed.
    hung=1
  else
    wait "$runner" 2>/dev/null || true
  fi
  sleep 1
  check "$([ "$ms" -lt $((limit * 1000)) ] && echo 1)" \
    "$sig: the host ended within ${limit}s (took ${ms} ms)"
  case "$dialog" in
    alert) ok_status="0 1" ;;
    *) ok_status="0" ;;
  esac
  check "$(echo " $ok_status " | grep -q " $status " && echo 1)" \
    "$sig: the host exits with status ${ok_status// / or } (got $status)"
  case "$dialog" in
    alert) check "$(grep -q '^\[e2e\] dialog alert returned' "$log" && echo 1)" \
      "$sig: the alert was ended" ;;
    confirm) check "$(grep -q '^\[e2e\] dialog confirm returned false' "$log" && echo 1)" \
      "$sig: the confirm ended as a cancel ($(grep -o 'dialog confirm returned.*' "$log" || echo 'never returned'))" ;;
  esac
  left=""
  for p in $procs; do
    if kill -0 "$p" 2>/dev/null; then left="$left$p "; fi
  done
  if [ "$backend" = cef ]; then
    left="$left$(pgrep -f "^$bin" | tr '\n' ' ' || true)"
  fi
  check "$([ -z "${left// /}" ] && echo 1)" "$sig: no process of the app is left (${left:-none})"
  if [ "$backend" = cef ]; then
    exit_type="$(grep -o '"exit_type":"[A-Za-z]*"' "$work/data/CEF/Default/Preferences" 2>/dev/null || true)"
    check "$([ "$exit_type" = '"exit_type":"Normal"' ] && echo 1)" \
      "$sig: the profile was shut down normally (${exit_type:-no exit_type})"
  fi
  cores="$(find "$work" -maxdepth 2 -name 'core*' | tr '\n' ' ')"
  crash_after="$(ls /var/crash 2>/dev/null | grep -c laufey || true)"
  check "$([ -z "$cores" ] && [ "$crash_after" = "$crash_before" ] && echo 1)" \
    "$sig: no core file or crash report (${cores:-no core}, /var/crash: $crash_before -> $crash_after)"
  if [ -n "${left// /}" ]; then kill -9 $left 2>/dev/null || true; fi
  if [ -n "$hung" ]; then
    kill -9 "$host" $procs 2>/dev/null || true
    wait "$runner" 2>/dev/null || true
  fi

  # The next launch reads what the signalled one wrote.
  rlog="$work/read.log"
  launch "$work" "$rlog" LAUFEY_E2E_STORAGE_MODE=read \
    LAUFEY_E2E_STORAGE_EXPECT="$value"
  runner=$!
  wait_status "$work" 90
  if [ ! -f "$work/host.status" ]; then
    kill -9 "$(cat "$work/host.pid" 2>/dev/null)" 2>/dev/null || true
  fi
  wait "$runner" 2>/dev/null || true
  check "$(grep -q '^\[e2e\] OVERALL PASS' "$rlog" && echo 1)" \
    "$sig: the next launch read $value back ($(grep -E '^\[e2e\] read local' "$rlog" || echo 'no read'))"

  if [ "$fail" = 1 ]; then
    echo "== host log ($sig) =="
    tail -40 "$log"
    echo "== read log ($sig) =="
    tail -20 "$rlog"
  fi
  rm -rf "$work"
done
exit "$fail"
