// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The backend makes no network request of its own
//! (LAUFEY_E2E_ONLY=network-quiet; scripts/native-e2e-run.sh
//! --network-quiet). See docs/backends.md, "No network requests of its
//! own".
//!
//! The battery's part is an ordinary app session: the custom-scheme page
//! loads (its fetch to the loopback echo server is the run's one expected
//! request), then the app sits idle for LAUFEY_E2E_QUIET_SECS seconds
//! (default 10) and quits through quit(), so the backend shuts down and
//! finishes what it logs. The verdict is the script's: on CEF it runs the
//! host with Chromium's net log and fails on any host in it other than
//! loopback (scripts/netlog-hosts.py).

use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{Value, Window};

use crate::{arg_string, check, na, wait_for};

pub(crate) async fn run(echo_url: Option<&str>) {
  // Open for the whole idle period, as an app's window would be.
  let _window = if laufey::scheme_handlers_supported() {
    let report: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let w = {
      let report = report.clone();
      Window::new(480, 360)
        .title("native-e2e-network-quiet")
        .bind("schemeReport", move |call| {
          // The page's cross-origin fetch result (see scheme_page_html).
          *report.lock().unwrap() = Some(arg_string(&call.args, 8));
          call.resolve(Value::Bool(true));
        })
        .bind("schemeProbe", |call| call.resolve(Value::Bool(true)))
        .load("laufey-e2e://app/")
    };
    w.show();
    let reported =
      wait_for(|| report.lock().unwrap().is_some(), 200, 100).await;
    let echo = report.lock().unwrap().clone().unwrap_or_default();
    if echo_url.is_some() {
      check(
        &format!(
          "the app page loads and reaches its loopback server \
           (reported {reported}, got {echo:?})"
        ),
        echo == "origin=laufey-e2e://app",
      );
    } else {
      check(&format!("the app page loads (got {echo:?})"), reported);
    }
    w
  } else {
    na("the app page (backend has no web engine)");
    Window::new(320, 200).title("native-e2e-network-quiet")
  };

  let secs = std::env::var("LAUFEY_E2E_QUIET_SECS")
    .ok()
    .and_then(|s| s.parse().ok())
    .unwrap_or(10u64);
  eprintln!("[e2e] idle for {secs} s");
  tokio::time::sleep(Duration::from_secs(secs)).await;

  // Quit the usual way, so the backend shuts its engine down (CEF writes the
  // end of its net log then), and return instead of exiting (as the
  // lifetime checks do).
  laufey::quit();
  check(
    "quit() ends the event loop",
    wait_for(laufey::should_shutdown, 300, 50).await,
  );
  let failed = crate::FAILED.load(Ordering::SeqCst);
  eprintln!("[e2e] OVERALL {}", if failed { "FAIL" } else { "PASS" });
  let _ = std::io::Write::flush(&mut std::io::stderr());
  if failed {
    unsafe { crate::libc_exit(1) };
  }
  crate::exit_guard::arm();
}
