// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The launch file's bridge pin (API 44; docs/launch-config.md). Run with
//! `LAUFEY_E2E_ONLY=bridge-origin` by `native-e2e-run.sh --bridge-origin`,
//! which writes a laufey-launch.json next to the backend pinning
//! `"bridgeOrigins": ["app://e2e-bridge-ok"]`.
//!
//! - A window on the pinned origin gets the bridge, and its calls reach the
//!   binding carrying that origin (`call.origin`).
//! - A window on another origin of the same scheme gets no bridge namespace,
//!   and a call it posts to the engine's message channel itself never reaches
//!   the binding.

use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{SchemeRequest, Value, Window};

use super::{arg_string, check, na, respond, wait_for};

pub const OK_ORIGIN: &str = "app://e2e-bridge-ok";
const NO_ORIGIN: &str = "app://e2e-bridge-no";

/// Reports every bridge-capable page sends: it calls `originReport` with its
/// own `location.origin`, every 200 ms, as long as it has a namespace.
const PAGE_HTML: &str = r#"<!doctype html><html><head><meta charset="utf-8">
<title>bridge origin</title></head><body>origin
<script>
setInterval(() => {
  if (typeof Laufey !== 'undefined') {
    Laufey.originReport(String(location.origin), 'bridge').catch(() => {});
  }
}, 200);
</script></body></html>"#;

/// Serves the two pages; hands every other request back.
pub fn serve(req: SchemeRequest) -> Option<SchemeRequest> {
  let path = req.url.split(['?', '#']).next().unwrap_or("").to_string();
  let path = path.trim_end_matches('/');
  if path == OK_ORIGIN || path == NO_ORIGIN {
    respond(req, 200, "text/html; charset=utf-8", PAGE_HTML.as_bytes());
    return None;
  }
  Some(req)
}

/// (page origin argument, call.origin, how it was sent)
type Reports = Arc<Mutex<Vec<(String, String, String)>>>;

fn window(url: &str, reports: &Reports) -> Window {
  let reports = reports.clone();
  Window::new(480, 320)
    .bind("originReport", move |call| {
      reports.lock().unwrap().push((
        arg_string(&call.args, 0),
        call.origin.clone(),
        arg_string(&call.args, 1),
      ));
      call.resolve(Value::Bool(true));
    })
    .load(url)
}

async fn eval(win: &Window, script: &str) -> Option<Value> {
  for _ in 0..30 {
    let (tx, rx) = tokio::sync::oneshot::channel();
    win.execute_js(
      script,
      Some(move |r: Result<Value, Value>| {
        let _ = tx.send(r);
      }),
    );
    match tokio::time::timeout(Duration::from_secs(1), rx).await {
      Ok(Ok(Ok(v))) => return Some(v),
      _ => tokio::time::sleep(Duration::from_millis(200)).await,
    }
  }
  None
}

pub async fn run() {
  if !laufey::scheme_handlers_supported() {
    na("bridge-origin (backend has no web engine)");
    return;
  }
  let ok_reports: Reports = Arc::new(Mutex::new(Vec::new()));
  let no_reports: Reports = Arc::new(Mutex::new(Vec::new()));
  let ok_win = window(&format!("{OK_ORIGIN}/"), &ok_reports);
  let no_win = window(&format!("{NO_ORIGIN}/"), &no_reports);

  let arrived =
    wait_for(|| !ok_reports.lock().unwrap().is_empty(), 150, 100).await;
  let first = ok_reports.lock().unwrap().first().cloned();
  check(
    &format!(
      "bridge-origin: the pinned origin's calls reach the binding ({first:?})"
    ),
    arrived,
  );
  if let Some((page, origin, _)) = first {
    check(
      &format!("bridge-origin: call.origin is the calling document's origin ({origin:?}, the page says {page:?})"),
      origin == OK_ORIGIN && page == OK_ORIGIN,
    );
  }

  // The other origin: no namespace, and a call posted to the engine's own
  // message channel (the page knows the bridge's message format) is refused.
  let ns = eval(
    &no_win,
    "String(location.origin) + ' ' + typeof window.Laufey",
  )
  .await
  .and_then(|v| v.as_string().map(str::to_string));
  check(
    &format!("bridge-origin: another origin's document gets no bridge namespace ({ns:?})"),
    ns.as_deref() == Some(format!("{NO_ORIGIN} undefined").as_str()),
  );
  let forge = r#"(function () {
  var m = {callId: 7, method: 'originReport', args: ['forged', 'forged']};
  try { window.webkit.messageHandlers.laufey.postMessage(m); } catch (e) {}
  try { window.webkit.messageHandlers.laufey.postMessage(JSON.stringify(m)); } catch (e) {}
  try { window.chrome.webview.postMessage(JSON.stringify(m)); } catch (e) {}
  return 'posted';
})()"#;
  let posted = eval(&no_win, forge)
    .await
    .and_then(|v| v.as_string().map(str::to_string));
  tokio::time::sleep(Duration::from_millis(1500)).await;
  let no = no_reports.lock().unwrap().clone();
  check(
    &format!("bridge-origin: a call another origin posts itself never reaches the binding ({posted:?}, {} arrived)", no.len()),
    posted.is_some() && no.is_empty(),
  );
  let _ = (&ok_win, &no_win);
}
