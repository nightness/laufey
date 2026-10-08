#!/usr/bin/env bash
#
# The cold-start click on Linux: a click on one of the app's notifications
# while the app isn't running starts it (D-Bus activates the app's
# `<app id>.service`) and the click reaches its notification response
# handler as the launch. With --schedule, a scheduled notification is
# posted by the app's systemd user timer while the app is closed, then its
# click starts the app. See docs/notifications.md.
#
#   scripts/notification-coldstart-e2e.sh <webview|cef> [--schedule]
#
# Needs a built backend and target/release/libnative_e2e_runtime.so, a
# session bus, and (for --schedule) a systemd user manager. Copies the
# backend and the runtime (as its colocated runtime, with a launch file
# naming dev.laufey.e2e.coldstart) to a temporary folder, installs
# `<app id>.desktop` and `<app id>.service` (Exec: the copy with
# --laufey-dbus-activated, and LAUFEY_E2E_COLDSTART=1 for the runtime, whose
# arguments (laufey::args_os) leave it out) under $XDG_DATA_HOME, runs the app once to post
# (or schedule) the notification, and lets it quit. The click is then what
# the desktop's shell sends for a click (gnome-shell and KDE's portal):
# org.freedesktop.Application.ActivateAction("laufey-notification",
# [target]) on the app's D-Bus name, which nothing owns, so D-Bus starts the
# app. The runtime's cold-start mode writes what its response handler
# received to $TMPDIR/laufey-coldstart-result.txt. In a desktop session the
# notification is real (the portal when it is available). Any process on the
# session bus can call ActivateAction, so laufey accepts only targets MAC'd
# with the install's key ($XDG_DATA_HOME/<app id>/laufey-notification-key):
# a forged click (a well-formed target without a MAC) is sent first, starts
# the app and must not arrive, nor use up the launch; the genuine target is
# signed here with the key, as laufey signs what it posts. To click it on
# screen instead, set LAUFEY_E2E_CLICK=manual and click it within 60 s, or
# set LAUFEY_E2E_CLICK_CMD to a command that clicks it (it gets the
# notification's title as $1: a desktop-specific test hook, such as Plasma's
# org.kde.NotificationManager.InvokeAction).
set -euo pipefail

backend="${1:?usage: notification-coldstart-e2e.sh <webview|cef> [--schedule]}"
mode="${2:-}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app_id="dev.laufey.e2e.coldstart"
app_path="/dev/laufey/e2e/coldstart"
runtime="$root/target/release/libnative_e2e_runtime.so"
[ -f "$runtime" ] || { echo "no $runtime (cargo build --release -p native_e2e_runtime)"; exit 1; }
case "$backend" in
  webview) src="$root/webview/build"; exe_name="laufey_webview" ;;
  cef) src="$root/cef/build/Release"; exe_name="laufey" ;;
  *) echo "unknown backend $backend"; exit 1 ;;
esac

dir="$(mktemp -d "${TMPDIR:-/tmp}/laufey-coldstart-XXXXXX")"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
desktop_file="$data_home/applications/$app_id.desktop"
service_file="$data_home/dbus-1/services/$app_id.service"
result="${TMPDIR:-/tmp}/laufey-coldstart-result.txt"
log="$dir/app.log"
for f in "$desktop_file" "$service_file"; do
  [ ! -e "$f" ] || { echo "refusing to replace $f"; exit 1; }
done
cleanup() {
  rm -f "$desktop_file" "$service_file" "$result"
  if [ "$mode" = "--schedule" ] && command -v systemctl >/dev/null; then
    systemctl --user stop "laufey-$app_id-$(printf '[0-9a-f]%.0s' $(seq 16)).timer" \
      2>/dev/null || true
  fi
  pkill -f "$dir/$exe_name" 2>/dev/null || true
  rm -rf "$dir" "$data_home/$app_id"
}
trap cleanup EXIT

if [ "$backend" = "webview" ]; then
  cp "$src/$exe_name" "$dir/"
else
  cp -r "$src/." "$dir/"
fi
exe="$dir/$exe_name"
cp "$runtime" "$exe.so"
printf '{"appId": "%s"}\n' "$app_id" > "$dir/laufey-launch.json"
mkdir -p "$(dirname "$desktop_file")" "$(dirname "$service_file")"
printf '[Desktop Entry]\nType=Application\nName=laufey cold-start e2e\nExec=%s\nNoDisplay=true\n' \
  "$exe" > "$desktop_file"
printf '[D-BUS Service]\nName=%s\nExec=/usr/bin/env LAUFEY_E2E_COLDSTART=1 %s --laufey-dbus-activated\n' \
  "$app_id" "$exe" > "$service_file"
command -v update-desktop-database >/dev/null &&
  update-desktop-database -q "$data_home/applications" || true
rm -f "$result"
# gnome-shell learns of a new desktop entry several seconds later (its
# app-info monitor: 4 to 6 s on GNOME 50); until then it refuses the app's
# notifications ("InvalidApp") while the portal still answers
# AddNotification. A package installs the entry long before.
sleep "${LAUFEY_E2E_ENTRY_WAIT:-10}"

name_owned() {
  gdbus call --session --dest org.freedesktop.DBus \
    --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner "$app_id" | grep -q true
}

fail() { echo "FAIL: $*"; echo "--- app log"; cat "$log" 2>/dev/null || true; exit 1; }

tag="cold-tag"
action="cold-action"
data='{"c":1}'
target='laufey=1&tag=cold-tag&action=cold-action&data=%7B%22c%22%3A1%7D'
if [ "$mode" = "--schedule" ]; then
  command -v systemctl >/dev/null || { echo "SKIP: no systemctl"; exit 77; }
  systemctl --user is-system-running >/dev/null 2>&1 ||
    systemctl --user show-environment >/dev/null 2>&1 ||
    { echo "SKIP: no systemd user manager"; exit 77; }
  LAUFEY_E2E_NOTIFY_SCHEDULE_MS=10000 "$exe" >"$log" 2>&1 ||
    fail "the scheduling run failed"
  grep -q '"notificationScheduleWhileClosed":true' "$log" ||
    fail "platform features don't report scheduling while closed"
  timer="$(systemctl --user list-timers --all --no-legend "laufey-$app_id-*" | wc -l)"
  [ "$timer" -ge 1 ] || fail "no laufey-$app_id-*.timer after scheduling"
  echo "PASS: the app quit, its timer is set:"
  systemctl --user list-timers --all --no-legend "laufey-$app_id-*"
  # The timer posts it while the app isn't running: the entry leaves the
  # schedule file.
  schedule="$data_home/$app_id/laufey-notifications.json"
  deadline=$((SECONDS + 40))
  while grep -q sched-tag "$schedule" 2>/dev/null; do
    [ "$SECONDS" -lt "$deadline" ] || fail "the timer didn't post sched-tag"
    sleep 1
  done
  echo "PASS: the timer's launch posted the notification and claimed it"
  journalctl --user -n 20 --no-pager -u "laufey-$app_id-*.service" 2>/dev/null |
    grep -i laufey | tail -5 || true
  tag="sched-tag"
  action="sched-action"
  data='{"s":1}'
  target='laufey=1&tag=sched-tag&action=sched-action&data=%7B%22s%22%3A1%7D'
else
  LAUFEY_E2E_NOTIFY_POST=1 "$exe" >"$log" 2>&1 || fail "the posting run failed"
  grep -q 'posted cold-tag: Ok(Some("Shown"))' "$log" || fail "the notification wasn't shown"
  echo "PASS: posted cold-tag, and the app quit"
  grep -o '"notificationTransport":"[a-z]*"\|"notificationColdStart":[a-z]*' "$log" || true
fi
name_owned && fail "something still owns $app_id"

title="laufey cold-start e2e"
[ "$mode" = "--schedule" ] && title="laufey scheduled e2e"
if [ "${LAUFEY_E2E_CLICK:-}" = "manual" ]; then
  echo "Click the notification (its body or its button) within 60 s"
  click_wait=60
elif [ -n "${LAUFEY_E2E_CLICK_CMD:-}" ]; then
  bash -c "$LAUFEY_E2E_CLICK_CMD" click "$title" || fail "the click command failed"
  click_wait=30
else
  activate() {
    gdbus call --session --dest "$app_id" --object-path "$app_path" \
      --method org.freedesktop.Application.ActivateAction \
      laufey-notification "[<'$1'>]" "{}" >/dev/null
  }
  # Forged: D-Bus starts the app, which drops it.
  activate "laufey=1&tag=forged-tag&data=%7B%22f%22%3A1%7D" ||
    fail "ActivateAction on $app_id failed (D-Bus activation)"
  sleep 5
  [ ! -s "$result" ] || fail "a forged click reached the response handler: $(cat "$result")"
  echo "PASS: a forged click (no MAC) started the app and was dropped"
  # Genuine: signed with the install's key, as the app signs what it posts.
  # (The app makes the key when it first signs: posting through the portal.
  # Through org.freedesktop.Notifications nothing is signed, so the started
  # app made it just now to check the forged click.)
  key_file="$data_home/$app_id/laufey-notification-key"
  [ -f "$key_file" ] || fail "no $key_file"
  [ "$(stat -c %a "$key_file")" = 600 ] || fail "$key_file isn't owner-only"
  mac="$(printf 'laufey-notification-click/1\n%s' "$target" |
    openssl dgst -sha256 -mac HMAC -macopt "hexkey:$(tr -d '[:space:]' < "$key_file")" |
    sed 's/.*= //')"
  activate "$target&mac=$mac" || fail "ActivateAction on $app_id failed"
  click_wait=30
fi
deadline=$((SECONDS + click_wait))
while [ ! -s "$result" ]; do
  [ "$SECONDS" -lt "$deadline" ] || fail "the started app wrote no result"
  sleep 0.25
done
mapfile -t lines < "$result"
echo "result: ${lines[*]}"
[ "${lines[0]}" = "$tag" ] || fail "tag ${lines[0]}, expected $tag"
if [ -z "${LAUFEY_E2E_CLICK:-}${LAUFEY_E2E_CLICK_CMD:-}" ]; then
  [ "${lines[1]}" = "$action" ] || fail "action ${lines[1]}, expected $action"
fi
[ "${lines[2]}" = "$data" ] || fail "data ${lines[2]}, expected $data"
[ "${lines[3]}" = "true" ] || fail "launch ${lines[3]}, expected true"
[ "${lines[4]}" = "args-clean" ] ||
  fail "the runtime saw --laufey-dbus-activated (${lines[4]})"
# The host leaves the process's argv as it is: the runtime library's
# .init_array function saw no NULL below argc (Deno's crashed on one).
[ "${lines[5]}" = "argv-intact" ] ||
  fail "the runtime library loaded with a changed argv (${lines[5]})"
echo "PASS: the click started the app and reached its response handler as the launch"
