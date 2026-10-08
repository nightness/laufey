//! Platform features (API 45) in the session the run is in
//! (LAUFEY_E2E_ONLY=platform; scripts/native-e2e-run.sh --platform).
//!
//! The probe and the tray must agree with each other everywhere: a tray
//! host means create_tray_icon returns an icon, no host means a refusal
//! (id 0) with the reason in "trayReason", never a dead icon. Run in a real
//! desktop session (LAUFEY_E2E_HOST_SESSION=1, no Xvfb) with the facts of
//! that session as expectations, it is the per-desktop matrix check:
//!
//!   LAUFEY_E2E_EXPECT_TRAY_HOST=1|0       a StatusNotifier / XEmbed host
//!   LAUFEY_E2E_EXPECT_SESSION=wayland|x11|tty
//!   LAUFEY_E2E_EXPECT_SECRET_SERVICE=available|locked|activatable|absent
//!   LAUFEY_E2E_EXPECT_COOKIES=os|basic    (CEF)
//!   LAUFEY_E2E_EXPECT_PORTALS=Notification,Settings,...  (each present)
//!   LAUFEY_E2E_EXPECT_NOTIFICATION_SERVER=1|0  a server owns
//!                                         org.freedesktop.Notifications
//!
//! An unset expectation is not checked.

use laufey::TrayIcon;

use crate::check;

fn expect(name: &str) -> Option<String> {
  std::env::var(name).ok().filter(|v| !v.is_empty())
}

pub(crate) async fn run() {
  let Some(f) = laufey::platform_features() else {
    check("platform_features answers (API 45)", false);
    return;
  };
  eprintln!("[e2e] platform features: {f}");
  let has = |needle: &str| f.contains(needle);
  check(
    "platform_features is a JSON object",
    f.starts_with("{\"os\":\"") && f.ends_with('}'),
  );
  if let Some(want) = expect("LAUFEY_E2E_EXPECT_TRAY_HOST") {
    let want = want == "1";
    check(
      &format!("trayHost is {want} in this session"),
      has(&format!("\"trayHost\":{want}")),
    );
  }
  if let Some(want) = expect("LAUFEY_E2E_EXPECT_SESSION") {
    check(
      &format!("sessionType is {want}"),
      has(&format!("\"sessionType\":\"{want}\"")),
    );
  }
  if let Some(want) = expect("LAUFEY_E2E_EXPECT_SECRET_SERVICE") {
    check(
      &format!("secretService is {want}"),
      has(&format!("\"secretService\":\"{want}\"")),
    );
  }
  if let Some(want) = expect("LAUFEY_E2E_EXPECT_COOKIES") {
    check(
      &format!("cookieEncryption is {want}"),
      has(&format!("\"cookieEncryption\":\"{want}\"")),
    );
  }
  if let Some(want) = expect("LAUFEY_E2E_EXPECT_NOTIFICATION_SERVER") {
    let running = !has("\"notificationServer\":null");
    check(
      &format!("a notification server runs: {}", want == "1"),
      running == (want == "1")
        && (running || has("\"notificationReason\":\"no ")),
    );
  }
  if let Some(want) = expect("LAUFEY_E2E_EXPECT_PORTALS") {
    for iface in want.split(',').filter(|s| !s.is_empty()) {
      check(
        &format!("the portal offers {iface}"),
        has(&format!("\"{iface}\":")),
      );
    }
  }

  // The tray agrees with the probe.
  let tray = TrayIcon::new();
  if has("\"trayHost\":true") {
    check(
      "a tray host: create_tray_icon returns an icon",
      tray.id() != 0,
    );
  } else {
    check(
      "no tray host: create_tray_icon refuses, and platform_features says why",
      tray.id() == 0 && has("\"trayReason\":\"no tray"),
    );
  }
  drop(tray);
}
