//! Backend-agnostic native-chrome / windowing e2e battery.
//!
//! This runtime is a cdylib loaded by *any* backend via `--runtime <path>` /
//! `LAUFEY_RUNTIME_PATH`, so the same assertions run under CEF, WebView, and
//! Winit. Because backends implement different subsets of the C ABI (Winit has
//! no web engine, some tray/menu fns may be absent), every assertion is
//! *capability-probed*: it emits one of
//!
//!   [e2e] PASS <name>   assertion held
//!   [e2e] FAIL <name>   supported here but wrong  -> process exits 1
//!   [e2e] N/A  <name>   capability absent on this backend (not a failure)
//!
//! This covers Layer 0 (in-process readback + event-callback round-trips),
//! menu/tray *click* round-trips via the `test_click_menu_item` C ABI hook
//! (API 30+; `N/A` on backends without it), the close-handler round-trip
//! via `test_trigger_close_requested` (API 31+; `N/A` on backends without
//! it), synthetic pointer / key / wheel events via `test_inject_input`, the
//! custom-scheme origin contract (a page served over the battery's own
//! `laufey-e2e://` scheme is a secure `<scheme>://<host>` origin; `N/A` on
//! engine-less backends), and a request-body round trip over the `app://`
//! scheme (see `body_echo.rs`; `N/A` on engine-less backends).
//! OS-observer *structure* checks
//! (Layer 1: the Linux D-Bus driver; macOS/Windows pending a backend hook)
//! live outside this runtime. See docs/e2e-testing.md.
//!
//! Mirrors the execution model of `examples/cef_e2e` (tokio runtime, spawned
//! event-loop pump, PASS/FAIL + exit code) so the existing runtime loader drives
//! it unchanged.

mod auth_thread_checks;
mod body_echo;
mod io_checks;
mod menu_notification_checks;
mod os_view;
mod stream_checks;
mod system_checks;

use std::collections::HashMap;
use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::{Arc, Mutex};

use laufey::{
  CursorEnterLeaveEvent, KeyState, KeyboardEvent, MenuItem, MouseButton,
  MouseButtonState, MouseClickEvent, MouseMoveEvent, SchemeRequest, TestInput,
  TrayIcon, Value, WheelDeltaMode, WheelEvent, Window, WindowOptions,
  LAUFEY_MOD_SHIFT, LAUFEY_MOUSE_BUTTON_LEFT, LAUFEY_WHEEL_DELTA_LINE,
};

static FAILED: AtomicBool = AtomicBool::new(false);

/// Minimal valid 1x1 transparent PNG. Used to give the tray a real icon so the
/// Linux D-Bus observer's `IconThemePath`/`IconName` assertion holds.
const TINY_PNG: &[u8] = &[
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49,
  0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06,
  0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44,
  0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D,
  0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
  0x60, 0x82,
];

fn check(name: &str, ok: bool) {
  if ok {
    eprintln!("[e2e] PASS {name}");
  } else {
    eprintln!("[e2e] FAIL {name}");
    FAILED.store(true, Ordering::SeqCst);
  }
}

/// Capability absent on this backend — informational, never fails the run.
fn na(name: &str) {
  eprintln!("[e2e] N/A  {name}");
}

/// N/A for a change only a window manager applies (maximize, minimize,
/// fullscreen on Linux), unless the run started one: scripts/native-e2e-run.sh
/// sets LAUFEY_E2E_WM_RUNNING when it runs a window manager under Xvfb, and
/// then nothing applying the change is a failure.
fn wm_na(name: &str) {
  if std::env::var_os("LAUFEY_E2E_WM_RUNNING").is_some() {
    check(&format!("{name} (a window manager is running)"), false);
  } else {
    na(name);
  }
}

/// Decorated macOS / Windows chrome puts the content origin below (and
/// usually a bit to the right of) the frame origin. Linux reports the
/// compositor's frame as the content, so the two coincide.
fn expect_title_bar_offset() -> bool {
  cfg!(any(target_os = "macos", target_os = "windows"))
}

fn check_inner_vs_frame(
  name: &str,
  frame: (i32, i32),
  inner: (i32, i32),
  expect_chrome: bool,
) {
  if frame == (0, 0) && inner == (0, 0) {
    na(&format!("{name} (window not placed yet)"));
    return;
  }
  // Winit's get_position only reports a not-yet-applied set, so after
  // realize the frame reads as (0, 0) while inner is the real content
  // origin. That is a getter limitation, not a chrome-offset bug.
  if frame == (0, 0) && inner != (0, 0) {
    na(&format!("{name} (frame origin not readable)"));
    return;
  }
  let dx = inner.0 - frame.0;
  let dy = inner.1 - frame.1;
  if expect_chrome {
    check(name, dy > 0 && dx >= 0 && dy > dx);
  } else {
    check(name, dx == 0 && dy == 0);
  }
}

/// The page's `[innerWidth, innerHeight]` in `win`, or `None` where there is
/// no page to ask (an engine-less backend) or it does not answer in time.
async fn page_inner_size(win: &Window) -> Option<(i32, i32)> {
  let (tx, rx) = tokio::sync::oneshot::channel();
  win.execute_js(
    "[window.innerWidth, window.innerHeight]",
    Some(move |r: Result<Value, Value>| {
      let _ = tx.send(r);
    }),
  );
  let num = |v: &Value| match v {
    Value::Int(n) => Some(*n),
    Value::Double(d) => Some(d.round() as i32),
    _ => None,
  };
  match tokio::time::timeout(std::time::Duration::from_secs(1), rx).await {
    Ok(Ok(Ok(Value::List(l)))) if l.len() == 2 => {
      Some((num(&l[0])?, num(&l[1])?))
    }
    _ => None,
  }
}

/// A number the page computes from `expr`, or `None` without a page.
async fn page_number(win: &Window, expr: &str) -> Option<f64> {
  for _ in 0..30 {
    let (tx, rx) = tokio::sync::oneshot::channel();
    win.execute_js(
      expr,
      Some(move |r: Result<Value, Value>| {
        let _ = tx.send(r);
      }),
    );
    match tokio::time::timeout(std::time::Duration::from_secs(1), rx).await {
      Ok(Ok(Ok(Value::Int(n)))) => return Some(n as f64),
      Ok(Ok(Ok(Value::Double(d)))) => return Some(d),
      // No answer yet (the page may still be loading): ask again.
      _ => tokio::time::sleep(std::time::Duration::from_millis(100)).await,
    }
  }
  None
}

/// The page fills the size set_size asked for: `get_size` is the content
/// (page) area on every backend, in CSS pixels.
async fn check_page_size(name: &str, win: &Window, want: (i32, i32)) {
  // A resize reaches the renderer a frame or two after the native window.
  let mut got = None;
  for _ in 0..30 {
    got = page_inner_size(win).await;
    match got {
      // No answer yet (the page may still be loading): ask again.
      None => tokio::time::sleep(std::time::Duration::from_millis(100)).await,
      Some((w, h)) if (w - want.0).abs() <= 1 && (h - want.1).abs() <= 1 => {
        break
      }
      _ => tokio::time::sleep(std::time::Duration::from_millis(100)).await,
    }
  }
  match got {
    None => na(&format!("{name} (no page to measure on this backend)")),
    Some((w, h)) => check(
      &format!("{name} (want {}x{}, page {w}x{h})", want.0, want.1),
      (w - want.0).abs() <= 1 && (h - want.1).abs() <= 1,
    ),
  }
}

fn check_outer_vs_inner(
  name: &str,
  inner: (i32, i32),
  outer: (i32, i32),
  expect_chrome: bool,
) {
  if inner == (0, 0) && outer == (0, 0) {
    na(&format!("{name} (size not available yet)"));
    return;
  }
  if expect_chrome {
    check(name, outer.0 >= inner.0 && outer.1 > inner.1);
  } else {
    check(name, outer == inner);
  }
}

async fn wait_for<F: Fn() -> bool>(f: F, attempts: u32, step_ms: u64) -> bool {
  for _ in 0..attempts {
    if f() {
      return true;
    }
    tokio::time::sleep(std::time::Duration::from_millis(step_ms)).await;
  }
  f()
}

fn expected_handle_type() -> (&'static str, &'static [i32]) {
  // (label, accepted type enums). Linux may be X11 or Wayland; under Xvfb it's
  // X11. See LAUFEY_WINDOW_HANDLE_* in the capi.
  if cfg!(target_os = "macos") {
    ("APPKIT", &[laufey::LAUFEY_WINDOW_HANDLE_APPKIT])
  } else if cfg!(target_os = "windows") {
    ("WIN32", &[laufey::LAUFEY_WINDOW_HANDLE_WIN32])
  } else {
    (
      "X11/WAYLAND",
      &[
        laufey::LAUFEY_WINDOW_HANDLE_X11,
        laufey::LAUFEY_WINDOW_HANDLE_WAYLAND,
      ],
    )
  }
}

// ---- custom scheme origin (technique C) --------------------------------
//
// The battery registers its own scheme and serves a page over it, then reads
// back what the page observed. On every web-engine backend a registered
// scheme must be a real origin: `location.origin == "laufey-e2e://app"`, a
// secure context (crypto.subtle works), same-origin fetch streams, storage
// works, and a cross-origin fetch carries `Origin: laufey-e2e://app`.

/// The custom scheme this battery registers. Must be declared to the CEF
/// backend at launch (`LAUFEY_CUSTOM_SCHEMES=laufey-e2e`, done by
/// scripts/native-e2e-run.sh) — see custom_schemes.h in cef/src.
const E2E_SCHEME: &str = "laufey-e2e";
const E2E_ORIGIN: &str = "laufey-e2e://app";
/// Never registered: fetching it must fail without reaching the handler.
const UNREGISTERED_SCHEME: &str = "laufey-e2e-unregistered";
/// Registered only after the first window exists, breaking the ordering
/// contract on purpose: that window must not serve it. Deliberately left out
/// of LAUFEY_CUSTOM_SCHEMES — CEF serves a scheme declared at launch in every
/// window whenever it is registered.
const LATE_SCHEME: &str = "laufey-e2e-late";
/// Body of `/stream`, sent as three separate writes with pauses in between.
const STREAM_CHUNKS: [&str; 3] = ["chunk-1|", "chunk-2|", "chunk-3|"];

/// What the custom-scheme page reported back through the `schemeReport`
/// binding (see the page script in `serve_scheme_request`).
#[derive(Clone, Debug, Default)]
struct SchemeReport {
  origin: String,
  secure: bool,
  subtle: String,
  digest_bytes: i32,
  storage: String,
  storage_prev: String,
  stream_text: String,
  stream_chunks: i32,
  echo_body: String,
}

fn arg_string(args: &[Value], i: usize) -> String {
  match args.get(i) {
    Some(Value::String(s)) => s.clone(),
    Some(Value::Bool(b)) => b.to_string(),
    Some(Value::Int(n)) => n.to_string(),
    Some(Value::Double(d)) => d.to_string(),
    Some(Value::Null) | None => String::new(),
    Some(_) => "<non-scalar>".to_string(),
  }
}

fn arg_bool(args: &[Value], i: usize) -> bool {
  args.get(i).and_then(|v| v.as_bool()).unwrap_or(false)
}

fn arg_int(args: &[Value], i: usize) -> i32 {
  match args.get(i) {
    Some(Value::Int(n)) => *n,
    Some(Value::Double(d)) => *d as i32,
    _ => 0,
  }
}

/// The page served at `laufey-e2e://app/`. Gathers the origin facts, exercises
/// crypto.subtle / localStorage / a streamed same-origin fetch / a cross-origin
/// fetch to the echo server, then reports through the `schemeReport` binding.
/// Every step is wrapped so one failure still lets the others report.
fn scheme_page_html(echo_url: &str) -> String {
  format!(
    r#"<!doctype html><html><head><meta charset="utf-8"><title>scheme</title></head><body>
<script>
// The namespace is a plain object on CEF but a callable Proxy on the WebView
// backends (typeof 'function'), so test only for the binding itself.
async function waitForBinding(name) {{
  for (let i = 0; i < 100; i++) {{
    if (typeof Laufey !== 'undefined' && typeof Laufey[name] === 'function') return;
    await new Promise(r => setTimeout(r, 50));
  }}
  throw new Error('binding never appeared: ' + name);
}}
const r = {{ origin: location.origin, secure: !!window.isSecureContext,
             subtle: typeof (self.crypto && crypto.subtle), digestBytes: 0,
             storage: '', storagePrev: '', streamText: '', streamChunks: 0,
             echoBody: '' }};
// Whatever happens, the runtime hears about it: an uncaught error is folded
// into the report instead of leaving the test to time out silently.
window.addEventListener('error', e => report('uncaught: ' + (e && e.message)));
window.addEventListener('unhandledrejection', e => report('unhandled: ' + (e && e.reason)));
let reported = false;
// Watchdog: if a step hangs (a stream that never ends, a fetch that never
// settles), report the partial state so the failure says where it stuck.
setTimeout(() => report('watchdog: stuck with ' + JSON.stringify(r)), 8000);
async function report(problem) {{
  if (reported) return;
  reported = true;
  if (problem) r.echoBody = problem + ' | ' + r.echoBody;
  await waitForBinding('schemeReport');
  await Laufey.schemeReport(r.origin, r.secure, r.subtle, r.digestBytes, r.storage,
                            r.storagePrev, r.streamText, r.streamChunks, r.echoBody);
}}
(async () => {{
  try {{
    const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode('laufey'));
    r.digestBytes = d.byteLength;
  }} catch (e) {{ r.subtle = 'error: ' + (e && e.message); }}
  try {{
    r.storagePrev = localStorage.getItem('laufey_e2e') || '';
    const v = 'run-' + Date.now();
    localStorage.setItem('laufey_e2e', v);
    r.storage = localStorage.getItem('laufey_e2e') === v ? 'ok' : 'read back mismatch';
  }} catch (e) {{ r.storage = 'error: ' + (e && e.message); }}
  try {{
    const res = await fetch('/stream');
    const reader = res.body.getReader();
    const dec = new TextDecoder();
    for (;;) {{
      const {{ value, done }} = await reader.read();
      if (done) break;
      r.streamChunks++;
      r.streamText += dec.decode(value, {{ stream: true }});
    }}
    r.streamText += dec.decode();
  }} catch (e) {{ r.streamText = 'error: ' + (e && e.message); }}
  const echoUrl = {echo_url:?};
  if (echoUrl) {{
    try {{
      const res = await fetch(echoUrl, {{ mode: 'cors' }});
      r.echoBody = await res.text();
    }} catch (e) {{ r.echoBody = 'error: ' + (e && e.message); }}
  }} else {{
    r.echoBody = 'skipped';
  }}
  await report(null);
}})().catch(e => report('script: ' + (e && e.message)));
</script></body></html>"#
  )
}

/// The page served at `app://e2e/`: proves the built-in scheme still works
/// next to the registered one, by reporting its origin through `appPing`.
const APP_PAGE_HTML: &str = r#"<!doctype html><html><head><meta charset="utf-8"><title>app</title></head><body>
<script>
(async () => {
  for (let i = 0; i < 100; i++) {
    if (typeof Laufey !== 'undefined' && typeof Laufey.appPing === 'function') {
      await Laufey.appPing(location.origin);
      return;
    }
    await new Promise(r => setTimeout(r, 50));
  }
})();
</script></body></html>"#;

fn respond(req: SchemeRequest, status: i32, content_type: &str, body: &[u8]) {
  let headers = vec![
    ("content-type".to_string(), content_type.to_string()),
    ("cache-control".to_string(), "no-store".to_string()),
  ];
  req.exchange.begin(status, &headers);
  if !body.is_empty() {
    req.exchange.write(body);
  }
  req.exchange.finish();
}

/// Scheme handler for both `laufey-e2e://app/...` and `app://e2e/...`. One
/// handler serves every registered scheme (the C ABI contract), so dispatch
/// on the URL. Every URL it sees is appended to `seen` so the negative checks
/// can prove a request never reached it. Runs on a backend thread; the
/// streamed route hops to its own thread so it never blocks the caller.
fn serve_scheme_request(
  req: SchemeRequest,
  echo_url: &str,
  seen: &Mutex<Vec<String>>,
) {
  let url = req.url.clone();
  seen.lock().unwrap().push(url.clone());
  // Strip an optional query/fragment; the routes below don't use them.
  let path = url.split(['?', '#']).next().unwrap_or("");
  match path {
    "laufey-e2e://app/" | "laufey-e2e://app" => respond(
      req,
      200,
      "text/html; charset=utf-8",
      scheme_page_html(echo_url).as_bytes(),
    ),
    "laufey-e2e://app/stream" => {
      std::thread::spawn(move || {
        let headers = vec![(
          "content-type".to_string(),
          "text/plain; charset=utf-8".to_string(),
        )];
        req.exchange.begin(200, &headers);
        for (i, chunk) in STREAM_CHUNKS.iter().enumerate() {
          if i > 0 {
            std::thread::sleep(std::time::Duration::from_millis(120));
          }
          if req.exchange.write(chunk.as_bytes()) < 0 {
            break; // consumer went away
          }
        }
        req.exchange.finish();
      });
    }
    "app://e2e/" | "app://e2e" => respond(
      req,
      200,
      "text/html; charset=utf-8",
      APP_PAGE_HTML.as_bytes(),
    ),
    // Target of the negative probes. CORS-open so that, were the engine to
    // serve the (cross-origin) request, the page's fetch would resolve
    // rather than fail on CORS — only a real refusal reads as "rejected".
    p if p.ends_with("://app/probe") => {
      let headers = vec![
        ("content-type".to_string(), "text/plain".to_string()),
        ("access-control-allow-origin".to_string(), "*".to_string()),
      ];
      req.exchange.begin(200, &headers);
      req.exchange.write(b"probe");
      req.exchange.finish();
    }
    _ => respond(req, 404, "text/plain", b"not found"),
  }
}

/// Script run in the custom-scheme window: fetch `url` and report through the
/// `schemeProbe` binding whether the engine served it (`served:<status>`) or
/// rejected the request (`rejected`).
fn fetch_probe_js(label: &str, url: &str) -> String {
  format!(
    r#"(async () => {{
  let out;
  try {{
    const res = await fetch({url:?});
    out = 'served:' + res.status;
  }} catch (e) {{
    out = 'rejected';
  }}
  await Laufey.schemeProbe({label:?}, out);
}})();"#
  )
}

/// A minimal loopback HTTP server that echoes the request's `Origin` header
/// (`origin=<value>` or `origin=none`) with `Access-Control-Allow-Origin: *`,
/// so a cross-origin fetch from the custom-scheme page proves which origin
/// the engine sends. Returns the URL to fetch, or `None` if no socket could
/// be bound (the check is then N/A).
fn start_origin_echo_server() -> Option<String> {
  let listener = std::net::TcpListener::bind("127.0.0.1:0").ok()?;
  let port = listener.local_addr().ok()?.port();
  std::thread::spawn(move || {
    for stream in listener.incoming() {
      let Ok(mut stream) = stream else { continue };
      let _ = stream.set_read_timeout(Some(std::time::Duration::from_secs(5)));
      let mut buf = Vec::new();
      let mut chunk = [0u8; 4096];
      // Read headers only (GET, no body).
      while !buf.windows(4).any(|w| w == b"\r\n\r\n") {
        match stream.read(&mut chunk) {
          Ok(0) | Err(_) => break,
          Ok(n) => buf.extend_from_slice(&chunk[..n]),
        }
      }
      let text = String::from_utf8_lossy(&buf);
      let origin = text
        .lines()
        .find_map(|l| {
          let (name, value) = l.split_once(':')?;
          name
            .trim()
            .eq_ignore_ascii_case("origin")
            .then(|| value.trim().to_string())
        })
        .unwrap_or_else(|| "none".to_string());
      let body = format!("origin={origin}");
      let response = format!(
        "HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\n\
         access-control-allow-origin: *\r\ncontent-length: {}\r\n\
         connection: close\r\n\r\n{}",
        body.len(),
        body
      );
      let _ = stream.write_all(response.as_bytes());
    }
  });
  Some(format!("http://127.0.0.1:{port}/echo"))
}

/// Request-body round trip over `app://` (see body_echo.rs). Opens its own
/// window and returns it so the caller keeps it alive until exit. The page
/// and echo routes are served by the battery's shared scheme handler.
async fn body_round_trip(received: &body_echo::Received) -> Option<Window> {
  if !laufey::scheme_handlers_supported() {
    na("custom-scheme request bodies (backend has no scheme handler support)");
    return None;
  }
  let reports: body_echo::Reports = Arc::new(Mutex::new(HashMap::new()));
  let win = Window::new(320, 240)
    .title("native-e2e-body")
    .bind("bodyReport", {
      let reports = reports.clone();
      move |call| {
        let a = &call.args;
        let len = match a.get(2) {
          Some(Value::Int(n)) => *n as i64,
          Some(Value::Double(d)) => *d as i64,
          _ => -1,
        };
        reports
          .lock()
          .unwrap()
          .insert(arg_string(a, 0), (arg_bool(a, 1), len, arg_string(a, 3)));
        call.resolve(Value::Bool(true));
      }
    })
    .load(body_echo::PAGE_URL);

  let cases = body_echo::cases();
  let done = wait_for(
    || {
      let r = reports.lock().unwrap();
      r.contains_key("script")
        || (cases.iter().all(|c| r.contains_key(c.label))
          && body_echo::MIME_CHECKS
            .iter()
            .all(|(label, _)| r.contains_key(*label)))
    },
    600,
    100,
  )
  .await;
  check("request-body page reported every case", done);
  let reports = reports.lock().unwrap().clone();
  if let Some((_, _, detail)) = reports.get("script") {
    check(&format!("request-body page script ran ({detail})"), false);
  }
  let received = received.lock().unwrap().clone();
  for c in &cases {
    let (method, body) = received
      .get(c.label)
      .cloned()
      .unwrap_or_else(|| (String::new(), Vec::new()));
    check(
      &format!(
        "{} {} body reaches the scheme handler intact ({} bytes sent, {} received, method {:?})",
        c.method,
        c.label,
        c.body.len(),
        body.len(),
        method
      ),
      received.contains_key(c.label) && method == c.method && body == c.body,
    );
    let (same, len, detail) = reports.get(c.label).cloned().unwrap_or((
      false,
      -1,
      "no report".to_string(),
    ));
    check(
      &format!(
        "{} echo is byte-identical in the page ({len} bytes, {detail})",
        c.label
      ),
      same && len == c.body.len() as i64,
    );
  }
  for (label, what) in body_echo::MIME_CHECKS {
    let (ok, _, detail) = reports.get(*label).cloned().unwrap_or((
      false,
      -1,
      "no report".to_string(),
    ));
    check(&format!("custom scheme: {what} ({detail})"), ok);
  }
  Some(win)
}

fn e2e_main() {
  let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
  rt.block_on(async move {
    // Pump the laufey event loop (JS-call dispatch, timers).
    tokio::spawn(async { laufey::run().await });

    // Windows: started by COM for a click on one of the app's toasts while it
    // wasn't running (scripts/notification-coldstart-e2e.ps1).
    if std::env::args().any(|a| a.eq_ignore_ascii_case("-ToastActivated")) {
      menu_notification_checks::cold_start().await;
    }

    // ---- C. custom scheme registration -----------------------------------
    // Registered BEFORE the first window: the engines read their scheme
    // tables when a web view is created (WebView2 fixes the set for the whole
    // process at its first environment), so this ordering is the documented
    // contract, not a convenience. "app" is served by the same handler.
    let scheme_supported = laufey::scheme_handlers_supported();
    let echo_url = start_origin_echo_server();
    let seen_urls: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
    // What the request-body round trip's echo routes received (body_echo.rs).
    let body_received: body_echo::Received =
      Arc::new(Mutex::new(HashMap::new()));
    // What the incremental-response checks observed (stream_checks.rs).
    let stream_state = stream_checks::State::default();
    let make_handler = {
      let echo = echo_url.clone().unwrap_or_default();
      let seen = seen_urls.clone();
      let received = body_received.clone();
      let streams = stream_state.clone();
      move || {
        let (echo, seen, received, streams) =
          (echo.clone(), seen.clone(), received.clone(), streams.clone());
        move |req: SchemeRequest| {
          let Some(req) = body_echo::serve(req, &received) else {
            return;
          };
          if let Some(req) = stream_checks::serve(req, &streams) {
            serve_scheme_request(req, &echo, &seen)
          }
        }
      }
    };
    laufey::register_scheme_handler(E2E_SCHEME, make_handler());

    // LAUFEY_E2E_ONLY=scheme-body runs just the request-body round trip.
    // Used where the full battery can't run (webview/linux in CI).
    if std::env::var("LAUFEY_E2E_ONLY").as_deref() == Ok("scheme-body") {
      let body_win = body_round_trip(&body_received).await;
      let stream_win = stream_checks::run(&stream_state).await;
      let _ = &stream_win;
      // Passkeys answer without the engine (not_supported on Linux), so they
      // ride along where the full battery can't run.
      passkey_checks(body_win.as_ref().map(|w| w.id()).unwrap_or(0)).await;
      let _ = &body_win;
      finish();
    }
    // LAUFEY_E2E_ONLY=lifetime: keep-alive with no window, then quit()
    // (both end the process, so they can't share the main battery's run).
    if std::env::var("LAUFEY_E2E_ONLY").as_deref() == Ok("lifetime") {
      lifetime_checks().await;
    }
    // LAUFEY_E2E_ONLY=io: drag and drop, file dialogs and the rich clipboard
    // (API 39); runs on every backend, webview/linux included.
    if std::env::var("LAUFEY_E2E_ONLY").as_deref() == Ok("io") {
      io_checks::run().await;
      finish();
    }
    // LAUFEY_E2E_ONLY=system: global shortcuts, launch at login and DevTools
    // (API 40); devtools-off: the same DevTools checks under
    // LAUFEY_INSPECTABLE=0; shortcut-holder: the second process the
    // shortcut conflict check starts.
    match std::env::var("LAUFEY_E2E_ONLY").as_deref() {
      Ok("system") => {
        system_checks::run().await;
        finish();
      }
      Ok("devtools-off") => {
        system_checks::devtools_off().await;
        finish();
      }
      // Menus and notifications (API 41).
      Ok("menus-notifications") => {
        menu_notification_checks::run().await;
        finish();
      }
      // UI-thread tasks and auth sessions (API 42). Ends with quit().
      Ok("auth-thread") => auth_thread_checks::run().await,
      Ok("shortcut-holder") => {
        // Not a battery: no OVERALL line, so its output can't be mistaken
        // for the main run's result.
        system_checks::hold_shortcut().await;
        eprintln!("[e2e-holder] done");
        let _ = std::io::Write::flush(&mut std::io::stderr());
        unsafe { libc_exit(0) };
      }
      _ => {}
    }
    // LAUFEY_E2E_ONLY=window-api: only the API 38 window checks (e.g. under a
    // real window manager, where the rest of the battery assumes none).
    if std::env::var("LAUFEY_E2E_ONLY").as_deref() == Ok("window-api") {
      window_api_checks().await;
      finish();
    }
    let scheme_report: Arc<Mutex<Option<SchemeReport>>> =
      Arc::new(Mutex::new(None));
    let scheme_probes: Arc<Mutex<HashMap<String, String>>> =
      Arc::new(Mutex::new(HashMap::new()));

    // ---- window creation + event-callback wiring -------------------------
    // Resize / move / focus handlers record the last event so we can drive the
    // setter and assert the callback round-trips (technique B).
    let resize_w = Arc::new(AtomicI32::new(0));
    let resize_h = Arc::new(AtomicI32::new(0));
    let moved = Arc::new(AtomicBool::new(false));
    let focused_seen = Arc::new(AtomicBool::new(false));
    let page_loaded = Arc::new(AtomicBool::new(false));

    let (rw, rh) = (resize_w.clone(), resize_h.clone());
    let mv = moved.clone();
    let fc = focused_seen.clone();
    let pl = page_loaded.clone();

    let win = Window::new(800, 600)
      .title("native-e2e")
      .on_resize(move |e| {
        rw.store(e.width, Ordering::SeqCst);
        rh.store(e.height, Ordering::SeqCst);
      })
      .on_move(move |_e| {
        mv.store(true, Ordering::SeqCst);
      })
      .on_focused(move |e| {
        if e.focused {
          fc.store(true, Ordering::SeqCst);
        }
      })
      // Readiness signal for the print_to_pdf test below; engine-less
      // backends never fire it, which that test treats as best-effort.
      .on_page_load(move |_e| {
        pl.store(true, Ordering::SeqCst);
      })
      // The custom-scheme page reports what it observed (technique C).
      .bind("schemeReport", {
        let report = scheme_report.clone();
        move |call| {
          let a = &call.args;
          *report.lock().unwrap() = Some(SchemeReport {
            origin: arg_string(a, 0),
            secure: arg_bool(a, 1),
            subtle: arg_string(a, 2),
            digest_bytes: arg_int(a, 3),
            storage: arg_string(a, 4),
            storage_prev: arg_string(a, 5),
            stream_text: arg_string(a, 6),
            stream_chunks: arg_int(a, 7),
            echo_body: arg_string(a, 8),
          });
          call.resolve(Value::Bool(true));
        }
      })
      // Negative probes (unregistered / late scheme) report here.
      .bind("schemeProbe", {
        let probes = scheme_probes.clone();
        move |call| {
          probes
            .lock()
            .unwrap()
            .insert(arg_string(&call.args, 0), arg_string(&call.args, 1));
          call.resolve(Value::Bool(true));
        }
      })
      // Serve the main page over the battery's own scheme so the origin
      // assertions below have something to measure. A no-op on engine-less
      // backends (Winit navigate is None).
      .load("laufey-e2e://app/");

    check("window id is nonzero", win.id() != 0);

    // Constructor-time getters must not wait for the OS window: JS can
    // read them from the constructor, and the winit backend creates the
    // NSWindow / HWND asynchronously. Seeded DPR used to stay 1.0 until
    // the first Resized; inner position used to be the frame origin.
    let early_scale = win.get_scale_factor();
    check("early scale factor is positive", early_scale > 0.0);
    let (early_w, early_h) = win.get_size();
    if (early_w, early_h) == (0, 0)
      && cfg!(target_os = "windows")
      && std::env::var("LAUFEY_E2E_BACKEND").as_deref() == Ok("webview")
    {
      // WebView2 creates the HWND on its UI thread after create_window
      // returns; until then there is no size to read.
      na("constructor size is readable immediately (WebView2 creates the window asynchronously)");
    } else {
      check(
        "constructor size is readable immediately",
        (early_w - 800).abs() <= 2 && (early_h - 600).abs() <= 2,
      );
    }

    let decorated = Window::new(400, 300).title("native-e2e-chrome");
    decorated.set_position(240, 160);
    check_inner_vs_frame(
      "decorated inner is offset by the title bar",
      decorated.get_position(),
      decorated.get_inner_position(),
      expect_title_bar_offset(),
    );

    let frameless = Window::new_with_options(
      200,
      150,
      WindowOptions {
        frameless: true,
        ..WindowOptions::default()
      },
    )
    .title("native-e2e-frameless");
    frameless.set_position(300, 200);
    check_inner_vs_frame(
      "frameless inner matches the frame origin",
      frameless.get_position(),
      frameless.get_inner_position(),
      false,
    );
    check_outer_vs_inner(
      "decorated outer is larger than the content",
      decorated.get_size(),
      decorated.get_outer_size(),
      expect_title_bar_offset(),
    );
    check_outer_vs_inner(
      "frameless outer matches the content",
      frameless.get_size(),
      frameless.get_outer_size(),
      false,
    );

    // ---- race probe: native handle immediately after creation ------------
    // Regression guard for denoland/deno#35785. The winit/raw backend creates
    // the OS window asynchronously, so reading the handle *right now* — with no
    // wait — is exactly the race that returned an uninitialized type 0
    // ("unknown Laufey window handle type: 0"). The getter must block until the
    // window exists and hand back the real type. Capability-probed: backends
    // that don't expose native handles legitimately return null/UNKNOWN.
    let early_handle = win.get_window_handle();
    if early_handle.is_null() {
      na("early window handle (backend doesn't expose native handles)");
    } else {
      let (label, accepted) = expected_handle_type();
      let ht = win.get_window_handle_type();
      check(
        &format!("early handle type resolved without racing (want {label}, got {ht})"),
        accepted.contains(&ht),
      );
    }

    // Give the backend a moment to realize the windows on screen, then wait
    // until the last one created has a size. Backends that create windows on
    // their UI thread do it in order, and WebView2 blocks that thread while
    // it creates its first environment: from under a second to over four on
    // a cold CI runner (CreateCoreWebView2EnvironmentWithOptions completing
    // inside the call). Every setter below queues behind that, so a round
    // trip timed from here would measure the engine's start-up instead.
    tokio::time::sleep(std::time::Duration::from_millis(300)).await;
    let realize_start = std::time::Instant::now();
    let realized = wait_for(|| frameless.get_size() != (0, 0), 600, 50).await;
    eprintln!(
      "[e2e] INFO windows realized: {realized} ({} ms after the first 300 ms)",
      realize_start.elapsed().as_millis()
    );

    let settled_scale = win.get_scale_factor();
    check("scale factor is still positive after realize", settled_scale > 0.0);
    check(
      "scale factor is stable from the constructor",
      (early_scale - settled_scale).abs() < 0.01,
    );
    let (settled_w, settled_h) = win.get_size();
    check(
      "constructor size survives realize",
      (settled_w - 800).abs() <= 2 && (settled_h - 600).abs() <= 2,
    );

    // After realize, WebView / CEF can still compare inner vs frame from
    // the live window. Winit's get_position only reflects a pending set,
    // so a (0, 0) frame with a nonzero inner is N/A rather than a fail.
    check_inner_vs_frame(
      "settled decorated inner is offset by the title bar",
      decorated.get_position(),
      decorated.get_inner_position(),
      expect_title_bar_offset(),
    );
    check_inner_vs_frame(
      "settled frameless inner matches the frame origin",
      frameless.get_position(),
      frameless.get_inner_position(),
      false,
    );
    check_outer_vs_inner(
      "settled decorated outer is larger than the content",
      decorated.get_size(),
      decorated.get_outer_size(),
      expect_title_bar_offset(),
    );
    check_outer_vs_inner(
      "settled frameless outer matches the content",
      frameless.get_size(),
      frameless.get_outer_size(),
      false,
    );

    // ---- test_inject_input ----------------------------------------------
    let last_key = Arc::new(Mutex::new(None::<KeyboardEvent>));
    let last_click = Arc::new(Mutex::new(None::<MouseClickEvent>));
    let last_move = Arc::new(Mutex::new(None::<MouseMoveEvent>));
    let last_wheel = Arc::new(Mutex::new(None::<WheelEvent>));
    let last_enter = Arc::new(Mutex::new(None::<CursorEnterLeaveEvent>));
    let input_win = {
      let lk = last_key.clone();
      let lc = last_click.clone();
      let lm = last_move.clone();
      let lw = last_wheel.clone();
      let le = last_enter.clone();
      Window::new(200, 150)
        .title("native-e2e-input")
        .on_keyboard_event(move |e| *lk.lock().unwrap() = Some(e))
        .on_mouse_click(move |e| *lc.lock().unwrap() = Some(e))
        .on_mouse_move(move |e| *lm.lock().unwrap() = Some(e))
        .on_wheel(move |e| *lw.lock().unwrap() = Some(e))
        .on_cursor_enter_leave(move |e| *le.lock().unwrap() = Some(e))
    };
    let input_id = input_win.id();

    if !laufey::test_inject_input(
      input_id,
      &TestInput::MouseMove {
        x: 10.0,
        y: 10.0,
        modifiers: 0,
      },
    ) {
      na("test_inject_input (backend has no hook)");
    } else {
      check(
        "inject mousemove reaches on_mouse_move",
        last_move
          .lock()
          .unwrap()
          .as_ref()
          .is_some_and(|e| (e.x - 10.0).abs() < 0.5 && (e.y - 10.0).abs() < 0.5),
      );

      // Reset hover so the next enter is pending until a real coordinate.
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::CursorLeave {
          x: 10.0,
          y: 10.0,
          modifiers: 0,
        },
      );
      *last_enter.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::CursorEnter {
          x: 0.0,
          y: 0.0,
          modifiers: 0,
        },
      );
      let enter_before_move = *last_enter.lock().unwrap();
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::MouseMove {
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      let enter_after_move = *last_enter.lock().unwrap();
      match (enter_before_move, enter_after_move) {
        (None, Some(e)) if e.entered && (e.x - 40.0).abs() < 0.5 && (e.y - 50.0).abs() < 0.5 => {
          check("mouseenter waits for the first move", true);
        }
        (Some(e), _) if e.entered && e.x == 0.0 && e.y == 0.0 => {
          na("mouseenter waits for the first move (backend injects enter immediately)");
        }
        _ => check("mouseenter waits for the first move", false),
      }

      *last_click.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::MouseButton {
          button: LAUFEY_MOUSE_BUTTON_LEFT,
          pressed: true,
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::MouseButton {
          button: LAUFEY_MOUSE_BUTTON_LEFT,
          pressed: false,
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      let first = last_click.lock().unwrap().clone();
      check(
        "inject click.detail is 1",
        first.as_ref().is_some_and(|e| {
          e.state == MouseButtonState::Released
            && e.button == MouseButton::Left
            && e.click_count == 1
        }),
      );

      *last_click.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::MouseButton {
          button: LAUFEY_MOUSE_BUTTON_LEFT,
          pressed: true,
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      match last_click.lock().unwrap().as_ref().map(|e| e.click_count) {
        Some(2) => check("inject second nearby press is click.detail 2", true),
        Some(1) => na(
          "inject second nearby press is click.detail 2 (backend inject does not track multi-click)",
        ),
        _ => check("inject second nearby press is click.detail 2", false),
      }

      *last_wheel.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::Wheel {
          delta_x: 0.0,
          delta_y: -1.0,
          delta_mode: LAUFEY_WHEEL_DELTA_LINE,
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      check(
        "inject line-scroll up is DOM-negative deltaY",
        last_wheel.lock().unwrap().as_ref().is_some_and(|e| {
          e.delta_y < 0.0 && e.delta_mode == WheelDeltaMode::Line
        }),
      );

      *last_key.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::Key {
          key: "a".into(),
          code: "KeyA".into(),
          pressed: true,
          repeat: false,
          modifiers: 0,
        },
      );
      check(
        "inject KeyA reaches on_keyboard_event",
        last_key.lock().unwrap().as_ref().is_some_and(|e| {
          e.state == KeyState::Pressed && e.key == "a" && e.code == "KeyA"
        }),
      );

      *last_key.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::Modifiers {
          modifiers: LAUFEY_MOD_SHIFT,
        },
      );
      check(
        "inject Shift modifiers emit keydown Shift/ShiftLeft",
        last_key.lock().unwrap().as_ref().is_some_and(|e| {
          e.state == KeyState::Pressed
            && e.key == "Shift"
            && e.code == "ShiftLeft"
            && e.modifiers.shift
        }),
      );
      *last_key.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::Modifiers { modifiers: 0 },
      );
      check(
        "inject Shift release emits keyup",
        last_key.lock().unwrap().as_ref().is_some_and(|e| {
          e.state == KeyState::Released
            && e.key == "Shift"
            && !e.modifiers.shift
        }),
      );
    }
    let _ = &input_win;

    // ---- A. direct state readback ---------------------------------------
    win.set_size(640, 480);
    let sized = wait_for(
      || {
        let (w, h) = win.get_size();
        (w - 640).abs() <= 2 && (h - 480).abs() <= 2
      },
      50,
      40,
    )
    .await;
    if !sized {
      // Say what was read and whether the size only arrived late, so a
      // failure tells "slow" apart from "wrong".
      let t0 = std::time::Instant::now();
      let late = wait_for(
        || {
          let (w, h) = win.get_size();
          (w - 640).abs() <= 2 && (h - 480).abs() <= 2
        },
        200,
        50,
      )
      .await;
      eprintln!(
        "[e2e] INFO set_size(640, 480): get_size {:?}; {}",
        win.get_size(),
        if late {
          format!("converged {} ms after the 2 s wait", t0.elapsed().as_millis())
        } else {
          "never converged within 10 s more".to_string()
        }
      );
    }
    check("set_size -> get_size round-trips", sized);
    check_page_size("set_size sizes the page area", &win, (640, 480)).await;

    // Position is advisory: window managers may constrain it. Assert loosely.
    win.set_position(150, 170);
    tokio::time::sleep(std::time::Duration::from_millis(200)).await;
    let (px, py) = win.get_position();
    if (px - 150).abs() <= 40 && (py - 170).abs() <= 40 {
      check("set_position -> get_position round-trips", true);
    } else {
      na("set_position (window manager constrained placement)");
    }

    win.set_resizable(false);
    let not_resizable = wait_for(|| !win.get_resizable(), 25, 20).await;
    win.set_resizable(true);
    let resizable_again = wait_for(|| win.get_resizable(), 25, 20).await;
    check(
      "set_resizable false/true round-trips",
      not_resizable && resizable_again,
    );

    // always-on-top and opacity are advisory: several backends / window
    // managers don't honor a runtime toggle or don't reflect it in the getter
    // (GTK/X11, some WMs). Treat "didn't round-trip" as N/A, not a failure —
    // this is a capability probe, not a WM-conformance test.
    win.set_always_on_top(true);
    let aot = wait_for(|| win.get_always_on_top(), 25, 20).await;
    win.set_always_on_top(false);
    if aot {
      check("set_always_on_top round-trips", true);
    } else {
      na("set_always_on_top (backend/WM doesn't reflect the toggle)");
    }

    win.set_opacity(0.6);
    tokio::time::sleep(std::time::Duration::from_millis(150)).await;
    if (win.get_opacity() - 0.6).abs() < 0.05 {
      check("set_opacity -> get_opacity round-trips", true);
    } else {
      na("set_opacity (backend doesn't support a runtime opacity toggle)");
    }
    win.set_opacity(1.0);

    win.set_click_passthrough(true);
    let passthrough = wait_for(|| win.get_click_passthrough(), 25, 20).await;
    win.set_click_passthrough(false);
    if passthrough {
      check("set_click_passthrough round-trips", true);
    } else {
      na("set_click_passthrough (backend doesn't reflect the toggle)");
    }

    // Forwarding is macOS-only for now; elsewhere the setter is a no-op and
    // the getter reports false, which this treats as N/A. Whether forwarded
    // events actually arrive needs real OS input and is not probed here.
    win.set_click_passthrough_forward(true);
    let forward = wait_for(|| win.get_click_passthrough_forward(), 25, 20).await;
    win.set_click_passthrough_forward(false);
    if forward {
      check("set_click_passthrough_forward round-trips", true);
    } else {
      na("set_click_passthrough_forward (platform doesn't support forwarding)");
    }

    // Visibility.
    win.show();
    let visible = wait_for(|| win.get_visible(), 25, 20).await;
    check("show -> get_visible true", visible);

    // ---- window handle (capability-probed) -------------------------------
    // Some backends (e.g. the system WebView) intentionally don't expose native
    // window handles and return null / UNKNOWN — that's N/A, not a failure.
    let handle = win.get_window_handle();
    if handle.is_null() {
      na("window handle (backend doesn't expose native handles)");
    } else {
      check("get_window_handle non-null", true);
      let (label, accepted) = expected_handle_type();
      let ht = win.get_window_handle_type();
      check(
        &format!("window handle type is {label} (got {ht})"),
        accepted.contains(&ht),
      );
    }

    // ---- B. event-callback round-trips ----------------------------------
    win.set_size(720, 540);
    let resize_fired = wait_for(
      || {
        (resize_w.load(Ordering::SeqCst) - 720).abs() <= 4
          && (resize_h.load(Ordering::SeqCst) - 540).abs() <= 4
      },
      60,
      40,
    )
    .await;
    if resize_fired {
      check("on_resize callback round-trips", true);
    } else if resize_w.load(Ordering::SeqCst) != 0 {
      // Fired but with different dims (HiDPI scaling etc.) — still a round-trip.
      check("on_resize callback fired", true);
    } else {
      na("on_resize (backend emits no resize events)");
    }

    win.set_position(220, 240);
    let move_fired = wait_for(|| moved.load(Ordering::SeqCst), 40, 40).await;
    if move_fired {
      check("on_move callback fires", true);
    } else {
      na("on_move (backend emits no move events / WM ignored)");
    }

    win.focus();
    let focus_fired =
      wait_for(|| focused_seen.load(Ordering::SeqCst), 40, 40).await;
    if focus_fired {
      check("on_focused callback fires", true);
    } else {
      na("on_focused (no focus event in headless session)");
    }

    // ---- system integration: clipboard round-trip -----------------------
    let nonce = std::process::id();
    let payload = format!("laufey-e2e-{nonce}");
    laufey::write_clipboard_text(&payload);
    match laufey::read_clipboard_text() {
      Some(got) if got == payload => {
        check("clipboard write/read round-trips", true)
      }
      Some(_) => check("clipboard write/read round-trips", false),
      None => na("clipboard (backend has no clipboard support)"),
    }

    // ---- native chrome: menu + tray click round-trips -------------------
    // Build an app menu and a tray menu with distinct item ids, then use the
    // test hook (test_click_menu_item) to synthesize a click and assert the
    // registered on_click handler fires with the right id. This exercises the
    // backend's template -> id-registration -> dispatch plumbing without OS
    // input. On backends without the hook (API < 30) the synth returns false
    // -> N/A.
    let app_click = Arc::new(std::sync::Mutex::new(None::<String>));
    let ac = app_click.clone();
    win.set_menu(
      &[MenuItem::Submenu {
        label: "E2E".into(),
        items: vec![
          MenuItem::Item {
            label: "Ping".into(),
            id: Some("app_ping".into()),
            accelerator: None,
            enabled: true,
            checked: false,
            icon: None,
            tooltip: None,
          },
          MenuItem::Separator,
          MenuItem::Role {
            role: "quit".into(),
          },
        ],
      }],
      move |id| *ac.lock().unwrap() = Some(id.to_string()),
    );
    tokio::time::sleep(std::time::Duration::from_millis(200)).await;
    if laufey::test_click_menu_item("app_ping") {
      check(
        "app menu click round-trips to on_click with id 'app_ping'",
        app_click.lock().unwrap().as_deref() == Some("app_ping"),
      );
    } else {
      na("app menu click round-trip (backend has no test_click hook)");
    }

    // Tray: id == 0 means the backend can't create tray icons here. Keep the
    // binding alive past this block (it destroys the native icon on drop) so
    // the Layer-1 D-Bus observer can introspect it during the hold below.
    //
    // On Linux the cef/webview tray is dlopen-gated on an appindicator
    // runtime library, so id 0 normally reads as N/A. A CI leg that installs
    // the library sets LAUFEY_E2E_REQUIRE_TRAY so a stub tray FAILS instead
    // of passing silently (issue #63).
    let require_tray =
      std::env::var("LAUFEY_E2E_REQUIRE_TRAY").is_ok_and(|v| !v.is_empty());
    let tray = TrayIcon::new();
    // Every tray menu click, counted for the Layer-1 hold below (the D-Bus
    // observer's dbusmenu Event must arrive here too).
    let tray_clicks = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let mut own_tray_clicks = 0usize;
    if tray.id() == 0 {
      if require_tray {
        check(
          "create_tray_icon returned nonzero id (LAUFEY_E2E_REQUIRE_TRAY)",
          false,
        );
      } else {
        na("tray (backend has no tray support on this platform)");
      }
    } else {
      check("create_tray_icon returned nonzero id", true);
      tray.set_icon(TINY_PNG);
      let tray_click = Arc::new(std::sync::Mutex::new(None::<String>));
      let tc = tray_click.clone();
      let counted = tray_clicks.clone();
      tray.set_menu(
        &[
          MenuItem::Item {
            label: "Ping".into(),
            id: Some("tray_ping".into()),
            accelerator: None,
            enabled: true,
            checked: false,
            icon: None,
            tooltip: None,
          },
          MenuItem::Role {
            role: "quit".into(),
          },
        ],
        move |id| {
          let n = counted.fetch_add(1, Ordering::SeqCst) + 1;
          eprintln!("[e2e] tray menu click #{n}: {id}");
          *tc.lock().unwrap() = Some(id.to_string());
        },
      );
      tokio::time::sleep(std::time::Duration::from_millis(200)).await;
      if laufey::test_click_menu_item("tray_ping") {
        own_tray_clicks = 1;
        check(
          "tray menu click round-trips to on_click with id 'tray_ping'",
          tray_click.lock().unwrap().as_deref() == Some("tray_ping"),
        );
      } else {
        na("tray menu click round-trip (backend has no test_click hook)");
      }
    }

    // Verifying menu/tray *structure* against the OS (that the widget was
    // really registered, not just that set_menu was accepted) needs main-thread
    // UI access. Linux is covered out-of-process by the D-Bus driver (Layer 1).
    // macOS self-AX is proven (docs/e2e-testing.md §7.2) but must run on the
    // backend's main thread — which this worker-thread runtime can't reach
    // (a dispatch_sync to the main queue deadlocks against the backend event
    // loop). It belongs behind a backend test hook; tracked as a follow-up.
    na("menu/tray OS-structure check (Linux: D-Bus driver; macOS/Windows: pending backend hook)");

    // ---- print_to_pdf smoke test (API >= 32) -----------------------------
    // Render the loaded page to a PDF and assert real PDF bytes come back
    // (`%PDF-` magic), exercising each backend's actual render path (WKWebView
    // createPDFWithConfiguration, WebView2 PrintToPdfStream, WebKitGTK
    // print-to-file, CEF DevTools Page.printToPDF). Winit has no web engine
    // and reports "unsupported" through the callback -> N/A. A backend whose
    // completion handler never fires shows up here as a FAIL on the delivery
    // assertion.
    //
    // The render races page load: WKWebView's createPDFWithConfiguration
    // fails with a generic NSError while the page is still loading, so wait
    // for on_page_load first (best effort -- engine-less backends never fire
    // it) and retry a transient error before judging; only a persistent error
    // is a real failure.
    let _ = wait_for(|| page_loaded.load(Ordering::SeqCst), 50, 100).await;
    let mut outcome: Option<Result<Vec<u8>, String>> = None;
    for attempt in 0..3u32 {
      if attempt > 0 {
        tokio::time::sleep(std::time::Duration::from_millis(700)).await;
      }
      let pdf_result =
        Arc::new(std::sync::Mutex::new(None::<Result<Vec<u8>, String>>));
      let pr = pdf_result.clone();
      win.print_to_pdf(None, move |r| *pr.lock().unwrap() = Some(r));
      if !wait_for(|| pdf_result.lock().unwrap().is_some(), 150, 100).await {
        continue; // nothing delivered this attempt; retry
      }
      let result = pdf_result.lock().unwrap().take().unwrap();
      let transient = matches!(&result, Err(e) if !e.contains("not supported"));
      outcome = Some(result);
      if !transient {
        break;
      }
    }
    match outcome {
      None => check("print_to_pdf delivers a result within 15s", false),
      Some(Ok(bytes)) => check(
        &format!("print_to_pdf returns real PDF bytes ({} bytes)", bytes.len()),
        bytes.starts_with(b"%PDF-"),
      ),
      Some(Err(e)) if e.contains("not supported") => {
        na("print_to_pdf (backend has no web engine / PDF support)")
      }
      Some(Err(e)) => {
        check(&format!("print_to_pdf succeeds (got: {e})"), false)
      }
    }

    // ---- C. custom scheme is a real origin --------------------------------
    // The page at laufey-e2e://app/ (loaded above) reports back through the
    // schemeReport binding. Engine-less backends leave register_scheme_handler
    // NULL -> N/A. On a web-engine backend a missing report is a FAIL: the
    // scheme was registered before the window, so the page must load.
    if !scheme_supported {
      na("custom scheme origin (backend has no scheme handler support)");
    } else {
      let reported =
        wait_for(|| scheme_report.lock().unwrap().is_some(), 200, 100).await;
      check("custom-scheme page loaded and reported back", reported);
      if let Some(r) = scheme_report.lock().unwrap().clone() {
        check(
          &format!("location.origin is {E2E_ORIGIN} (got {:?})", r.origin),
          r.origin == E2E_ORIGIN,
        );
        check("custom-scheme page is a secure context", r.secure);
        check(
          &format!(
            "crypto.subtle works on the custom scheme (typeof {}, digest {} bytes)",
            r.subtle, r.digest_bytes
          ),
          r.subtle == "object" && r.digest_bytes == 32,
        );
        check(
          &format!("localStorage works on the custom-scheme origin ({})", r.storage),
          r.storage == "ok",
        );
        // Persistence across launches is only observable on the second run;
        // report it so a manual two-run check has the evidence.
        eprintln!(
          "[e2e] INFO localStorage value from a previous run: {:?}",
          r.storage_prev
        );
        let expected_stream: String = STREAM_CHUNKS.concat();
        check(
          &format!(
            "same-origin fetch over the custom scheme delivers the streamed body (got {:?})",
            r.stream_text
          ),
          r.stream_text == expected_stream,
        );
        // Chunk arrival is engine-dependent (WebView2 buffers the whole
        // response before answering); informational only.
        eprintln!(
          "[e2e] INFO streamed response arrived in {} read(s) ({} writes)",
          r.stream_chunks,
          STREAM_CHUNKS.len()
        );
        match &echo_url {
          Some(_) => check(
            &format!(
              "cross-origin fetch from the custom scheme sends Origin: {E2E_ORIGIN} (got {:?})",
              r.echo_body
            ),
            r.echo_body == format!("origin={E2E_ORIGIN}"),
          ),
          None => na("cross-origin Origin header (could not bind a loopback echo server)"),
        }
      }

      // Negative: a scheme nobody registered is not served — the fetch
      // fails and the request never reaches the handler.
      let probe = |label: &'static str, url: String| {
        let probes = scheme_probes.clone();
        win.execute_js(&fetch_probe_js(label, &url), None::<fn(_)>);
        async move {
          wait_for(|| probes.lock().unwrap().contains_key(label), 100, 100)
            .await;
          probes.lock().unwrap().get(label).cloned().unwrap_or_default()
        }
      };
      let reached = |scheme: &str| {
        let prefix = format!("{scheme}:");
        seen_urls
          .lock()
          .unwrap()
          .iter()
          .any(|u| u.starts_with(&prefix))
      };
      let got = probe(
        "unregistered",
        format!("{UNREGISTERED_SCHEME}://app/probe"),
      )
      .await;
      check(
        &format!("an unregistered scheme is not served (fetch: {got:?})"),
        got == "rejected" && !reached(UNREGISTERED_SCHEME),
      );

      // Negative: registering a scheme after the window exists breaks the
      // ordering contract, so that window must not serve it (the backend
      // logs a warning on stderr). WebKitGTK is the exception: its schemes
      // live on the shared web context, which applies a late registration to
      // existing web views too — reported, not asserted, there.
      laufey::register_scheme_handler(LATE_SCHEME, make_handler());
      tokio::time::sleep(std::time::Duration::from_millis(300)).await;
      let got = probe("late", format!("{LATE_SCHEME}://app/probe")).await;
      let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
      if cfg!(target_os = "linux") && backend == "webview" {
        eprintln!(
          "[e2e] INFO WebKitGTK: scheme registered after the first window, \
           fetched from it: {got:?}"
        );
      } else {
        check(
          &format!(
            "a scheme registered after the first window is not served by it (fetch: {got:?})"
          ),
          got == "rejected" && !reached(LATE_SCHEME),
        );
      }
    }

    // ---- request body over the custom scheme -----------------------------
    let body_win = body_round_trip(&body_received).await;
    let stream_win = stream_checks::run(&stream_state).await;

    // ---- close-requested handler round-trip --------------------------------
    // A second window (kept separate from `win`, which must survive to
    // shutdown). Registers an on_close_requested handler that *stashes* the
    // window_id instead of resolving inline — proving resolution genuinely
    // works from outside the handler (any thread, any time), not just
    // synchronously inline. Synthesizes the close via
    // test_trigger_close_requested (API >= 31, implemented by every in-tree
    // backend — init_api's version check guarantees it's present, so a
    // failure here is a regression, never a missing hook).
    //
    // Its page is served over the built-in "app" scheme by the same handler
    // as laufey-e2e://, proving "app" keeps working next to a registered
    // scheme (technique C, backward compatibility).
    let app_origin: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let close_win = Window::new(200, 150)
      .title("native-e2e-close")
      .bind("appPing", {
        let origin = app_origin.clone();
        move |call| {
          *origin.lock().unwrap() = Some(arg_string(&call.args, 0));
          call.resolve(Value::Bool(true));
        }
      })
      .load("app://e2e/");
    let close_win_id = close_win.id();
    if scheme_supported {
      let pinged =
        wait_for(|| app_origin.lock().unwrap().is_some(), 150, 100).await;
      let got = app_origin.lock().unwrap().clone().unwrap_or_default();
      check(
        &format!("built-in app:// scheme still served next to the registered one (origin {got:?})"),
        pinged && got == "app://e2e",
      );
    }
    let baseline_sized = wait_for(
      || {
        let (w, _h) = close_win.get_size();
        w != 0
      },
      50,
      40,
    )
    .await;

    let close_handler_fired = Arc::new(AtomicBool::new(false));
    let pending_close: Arc<std::sync::Mutex<Option<u32>>> =
      Arc::new(std::sync::Mutex::new(None));
    let close_win = {
      let fired = close_handler_fired.clone();
      let pending = pending_close.clone();
      close_win.on_close_requested(move |event| {
        fired.store(true, Ordering::SeqCst);
        *pending.lock().unwrap() = Some(event.window_id);
      })
    };

    let still_open_after_defer =
      laufey::test_trigger_close_requested(close_win_id);
    if !baseline_sized {
      // Precondition, not a close-dispatch check: without a sized baseline
      // the stays-open/closed probes below are meaningless.
      na("close handler (precondition failed: close_win never reported a nonzero size)");
    } else {
      // The hook itself is guaranteed present: init_api rejects any backend
      // whose API version differs from the current one, and every in-tree
      // backend implements test_trigger_close_requested. Dispatch is synchronous,
      // so a handler that didn't fire is a real regression — fail, don't
      // N/A.
      check(
        "on_close_requested handler fired for synthesized close",
        close_handler_fired.load(Ordering::SeqCst),
      );
      check(
        "close handler defers — window stays open",
        still_open_after_defer,
      );
      let (w, _h) = close_win.get_size();
      check("window state still readable after deferred close", w != 0);

      // Resolve the stashed window_id — from here, not from inside the
      // handler — and confirm the window actually closes.
      let pending_id = pending_close.lock().unwrap().take();
      if let Some(id) = pending_id {
        Window::from_id(id).close();
      }
      let closed = wait_for(
        || {
          let (w, h) = close_win.get_size();
          w == 0 && h == 0
        },
        50,
        40,
      )
      .await;
      check(
        "closing from outside the on_close_requested handler actually closes the window",
        closed,
      );
    }

    // ---- deep-link (open-url) round-trip -----------------------------------
    // Capability-probed: the hook is NULL on every non-macOS backend, where
    // the OS hands deep links to a new process as argv instead of to the
    // running one (see set_open_url_handler in laufey.h).
    //
    // Covers both halves of the delivery contract. The first trigger runs
    // *before* any handler is registered — the position every cold-start
    // launch URL is in, since the runtime is still coming up when the OS
    // routes it — so it must be buffered and replayed on registration, not
    // dropped.
    const COLD_URL: &str = "laufeytest://open/document/42";
    const LIVE_URL: &str = "laufeytest://open/document/43?q=1";

    let seen_urls: Arc<std::sync::Mutex<Vec<String>>> =
      Arc::new(std::sync::Mutex::new(Vec::new()));
    let buffered_not_delivered = !laufey::test_trigger_open_url(COLD_URL);
    {
      let seen = seen_urls.clone();
      laufey::on_open_url(move |url| {
        seen.lock().unwrap().push(url.to_string());
      });
    }
    // Both the flush above and this trigger dispatch synchronously on this
    // thread, so the assertions below need no waiting.
    let delivered_live = laufey::test_trigger_open_url(LIVE_URL);
    let urls = seen_urls.lock().unwrap().clone();

    if !delivered_live && urls.is_empty() {
      // A registered handler that receives nothing means the hook is absent
      // (non-macOS backend), not that dispatch is broken.
      na("deep links (test_trigger_open_url unavailable on this backend)");
    } else {
      check(
        "URL arriving before any handler is buffered, not delivered",
        buffered_not_delivered,
      );
      check(
        "buffered cold-start URL is replayed on registration",
        urls.first().map(String::as_str) == Some(COLD_URL),
      );
      check(
        "URL arriving with a handler registered is delivered live",
        delivered_live && urls.iter().any(|u| u == LIVE_URL),
      );
      check(
        "no URL is delivered twice or lost",
        urls.len() == 2,
      );
    }

    // ---- Window state, constraints, screens, chrome (API >= 38) ----------
    window_api_checks().await;

    // ---- Passkeys (API >= 37) --------------------------------------------
    passkey_checks(win.id()).await;

    // ---- Layer-1 hold ----------------------------------------------------
    // When driven by the D-Bus observer (native_e2e_driver), stay alive with
    // the tray + menu registered so it can read the StatusNotifierItem, walk
    // the dbusmenu layout, and fire a menu Event that round-trips to the
    // `on_click` above. The driver kills us when it's done.
    // The battery's own test_click_menu_item made one click; the observer's
    // Event is the next, and it may already have arrived.
    if std::env::var_os("LAUFEY_E2E_HOLD").is_some() {
      eprintln!("[e2e] holding for Layer-1 observer");
      let reached = wait_for(
        || tray_clicks.load(Ordering::SeqCst) > own_tray_clicks,
        600,
        50,
      )
      .await;
      check(
        "Layer 1: the observer's dbusmenu Event reaches the tray's on_click",
        reached,
      );
    }

    // A menu-bar-only macOS app uses the Accessory activation policy. Closing
    // its last transient window must not terminate the process: it still owns
    // the status item and needs to be able to show a window later. This check
    // deliberately closes `win`, the final remaining window, then proves this
    // runtime is still alive on the next turn of the async executor.
    #[cfg(target_os = "macos")]
    {
      laufey::set_dock_visible(false);
      tokio::time::sleep(std::time::Duration::from_millis(250)).await;
      win.close();
      tokio::time::sleep(std::time::Duration::from_millis(250)).await;
      check("accessory app survives closing its last window", true);
    }

    // ---- shutdown --------------------------------------------------------
    // Decide the result and terminate immediately with a deterministic exit
    // code. We deliberately skip close()/quit(): tearing the window/webview
    // down on the backend's main thread can crash or race and clobber the exit
    // code (e.g. SIGTRAP -> 133), which would corrupt the CI signal. The OS
    // reclaims everything on exit. `_ = &win;` keeps the window alive to here.
    let _ = (&win, &body_win, &stream_win);
    finish();
  });
}

// ---- Window state, constraints, screens, chrome (API >= 38) ---------------

fn rect_inside(inner: &laufey::Rect, outer: &laufey::Rect) -> bool {
  inner.x >= outer.x
    && inner.y >= outer.y
    && inner.x + inner.width <= outer.x + outer.width
    && inner.y + inner.height <= outer.y + outer.height
}

async fn wait_state<F: Fn(laufey::WindowState) -> bool>(
  w: &Window,
  f: F,
  timeout_ms: u64,
) -> bool {
  wait_for(|| f(w.get_state()), (timeout_ms / 50) as u32, 50).await
}

/// Every check is capability-probed: a backend that reports a capability must
/// honour it, and one that doesn't must refuse honestly (setters return
/// false). Window-manager-dependent state changes (Linux under a bare X
/// server has no window manager to apply them) are N/A when nothing changed
/// at all, never when the state changed wrongly.
async fn window_api_checks() {
  let caps = laufey::window_capabilities();
  eprintln!("[e2e] window capabilities = {:#x}", caps.bits);
  check("window capabilities are reported (API 38)", caps.bits != 0);

  if std::env::var_os("LAUFEY_E2E_EARLY_RESIZE").is_some() {
    // Experiment: set_size at various delays after creating a window; does
    // the page follow?
    let mut lost = 0;
    for round in 0..3 {
      for delay in [0u64, 20, 50, 100, 200, 300, 400, 600] {
        let x = Window::new(520, 420).title("early-resize");
        x.show();
        tokio::time::sleep(std::time::Duration::from_millis(delay)).await;
        x.set_size(600, 400);
        let mut got = None;
        for _ in 0..40 {
          got = page_inner_size(&x).await;
          if got == Some((600, 400)) {
            break;
          }
          tokio::time::sleep(std::time::Duration::from_millis(100)).await;
        }
        if got != Some((600, 400)) {
          lost += 1;
        }
        eprintln!(
          "[e2e] EARLY round {round} delay {delay} ms: page {got:?} size {:?}",
          x.get_size()
        );
        x.close();
        tokio::time::sleep(std::time::Duration::from_millis(200)).await;
      }
    }
    eprintln!("[e2e] EARLY lost {lost} of 24");
  }
  const TITLE: &str = "native-e2e-window-api";
  let w = Window::new(520, 420).title(TITLE);
  let id = w.id();
  w.show();
  let sized = wait_for(|| w.get_size().0 != 0, 100, 50).await;
  if !sized {
    check("window-api window reports a size", false);
    return;
  }
  // --hidpi: the rest of this battery then runs at that scale too.
  if let Some(scale) = std::env::var("LAUFEY_E2E_EXPECT_SCALE")
    .ok()
    .and_then(|v| v.parse::<f64>().ok())
  {
    hidpi_checks(&w, TITLE, scale).await;
  }

  // -- screens --------------------------------------------------------------
  if caps.screens() {
    let screens = laufey::screens();
    check("screens(): at least one display", !screens.is_empty());
    check(
      "screens(): exactly one primary, listed first",
      screens.iter().filter(|s| s.is_primary).count() == 1
        && screens.first().map(|s| s.is_primary).unwrap_or(false),
    );
    let sane = screens.iter().all(|s| {
      s.id != 0
        && s.id < (1i64 << 53)
        && s.bounds.width > 0
        && s.bounds.height > 0
        && s.work_area.width > 0
        && s.work_area.height > 0
        && rect_inside(&s.work_area, &s.bounds)
        && s.scale_factor >= 0.5
        && s.scale_factor <= 8.0
    });
    check(
      "screens(): nonzero ids, positive bounds, work area inside bounds, sane scale",
      sane,
    );
    let mut ids: Vec<i64> = screens.iter().map(|s| s.id).collect();
    ids.sort_unstable();
    ids.dedup();
    check("screens(): ids are distinct", ids.len() == screens.len());
    match w.get_screen_id() {
      Some(sid) => check(
        "window screen id is one of screens()",
        screens.iter().any(|s| s.id == sid),
      ),
      None => na("window screen (backend can't tell which display yet)"),
    }
    for s in &screens {
      eprintln!("[e2e]   screen {s:?}");
    }
  } else {
    check(
      "screens() is empty without the capability",
      laufey::screens().is_empty(),
    );
  }
  if caps.display_events() {
    // A display change can't be synthesized; registering must be harmless.
    laufey::on_display_changed(|| eprintln!("[e2e] display changed"));
    check("on_display_changed registers", true);
  } else {
    na("display-changed events (not reported by this backend)");
  }

  // -- size constraints -----------------------------------------------------
  if caps.size_constraints() {
    let range = laufey::SizeConstraints {
      min_width: 400,
      min_height: 300,
      max_width: 900,
      max_height: 700,
    };
    w.set_size_constraints(range);
    check(
      "size constraints round-trip",
      w.get_size_constraints() == range,
    );
    check(
      "get_min_size / get_max_size",
      w.get_min_size() == (400, 300) && w.get_max_size() == (900, 700),
    );
    w.set_size(120, 90);
    let clamped_up = wait_for(
      || {
        let (sw, sh) = w.get_size();
        (sw - 400).abs() <= 4 && (sh - 300).abs() <= 4
      },
      60,
      50,
    )
    .await;
    let (sw, sh) = w.get_size();
    check(
      &format!("set_size below the minimum is clamped to it (got {sw}x{sh})"),
      clamped_up,
    );
    check_page_size("the minimum size is the page area", &w, (400, 300)).await;
    w.set_size(3000, 3000);
    let clamped_down = wait_for(
      || {
        let (sw, sh) = w.get_size();
        (sw - 900).abs() <= 4 && (sh - 700).abs() <= 4
      },
      60,
      50,
    )
    .await;
    let (sw, sh) = w.get_size();
    check(
      &format!("set_size above the maximum is clamped to it (got {sw}x{sh})"),
      clamped_down,
    );
    check_page_size("the maximum size is the page area", &w, (900, 700)).await;
    // A window outside a new range is resized into it.
    w.set_size(800, 600);
    let _ = wait_for(|| (w.get_size().0 - 800).abs() <= 4, 40, 50).await;
    w.set_max_size(600, 500);
    let pulled_in = wait_for(
      || {
        let (sw, sh) = w.get_size();
        sw <= 604 && sh <= 504
      },
      60,
      50,
    )
    .await;
    let (sw, sh) = w.get_size();
    check(
      &format!("a tighter maximum resizes the window into it (got {sw}x{sh})"),
      pulled_in,
    );
    // A maximum below the minimum is raised to it.
    w.set_size_constraints(laufey::SizeConstraints {
      min_width: 500,
      min_height: 0,
      max_width: 300,
      max_height: 0,
    });
    check(
      "a maximum below the minimum is raised to it",
      w.get_size_constraints().max_width == 500,
    );
    w.set_size_constraints(laufey::SizeConstraints::default());
    check(
      "constraints clear to none",
      w.get_size_constraints() == laufey::SizeConstraints::default(),
    );
    w.set_size(520, 420);
    let _ = wait_for(|| (w.get_size().0 - 520).abs() <= 4, 40, 50).await;
  } else {
    na("size constraints (not supported by this backend)");
  }

  // -- state round trips ------------------------------------------------------
  if caps.state() {
    let events: Arc<Mutex<Vec<laufey::WindowStateEvent>>> = Arc::default();
    {
      let events = events.clone();
      laufey::on_window_state_change(id, move |ev| {
        events.lock().unwrap().push(ev);
      });
    }
    let saw = |pred: &dyn Fn(&laufey::WindowStateEvent) -> bool| {
      events.lock().unwrap().iter().any(pred)
    };
    check(
      "a new window is in the normal state",
      w.get_state().is_normal(),
    );

    w.set_position(80, 90);
    tokio::time::sleep(std::time::Duration::from_millis(400)).await;
    let before_pos = w.get_position();
    let before_size = w.get_size();
    // Bounds become the normal bounds once they have rested
    // (kNormalBoundsSettleMs, timed from when the OS reports them). On a busy
    // runner the move can be reported late enough that the sleep above isn't
    // a rest yet: wait until the tracker has settled on these bounds, so the
    // checks below test maximize / fullscreen, not the runner's load.
    if caps.normal_bounds() {
      let _ = wait_for(
        || {
          w.get_normal_bounds().is_some_and(|r| {
            (r.x - before_pos.0).abs() <= 4
              && (r.y - before_pos.1).abs() <= 4
              && (r.width - before_size.0).abs() <= 4
              && (r.height - before_size.1).abs() <= 4
          })
        },
        60,
        50,
      )
      .await;
    }

    // maximize / unmaximize
    w.maximize();
    if wait_state(&w, |s| s.maximized, 6000).await {
      check("maximize -> is_maximized", true);
      let evented = wait_for(
        || saw(&|e| e.state.maximized && !e.previous.maximized),
        60,
        50,
      )
      .await;
      check(
        "maximize fires a state event (previous not maximized)",
        evented,
      );
      if caps.normal_bounds() {
        tokio::time::sleep(std::time::Duration::from_millis(400)).await;
        match w.get_normal_bounds() {
          Some(r) => {
            let size_ok = (r.width - before_size.0).abs() <= 4
              && (r.height - before_size.1).abs() <= 4;
            let pos_ok = !caps.set_position()
              || ((r.x - before_pos.0).abs() <= 40
                && (r.y - before_pos.1).abs() <= 40);
            check(
              &format!(
                "normal bounds while maximized are the pre-maximize bounds (got {r:?}, before {before_pos:?} {before_size:?})"
              ),
              size_ok && pos_ok,
            );
          }
          None => check("normal bounds available while maximized", false),
        }
      }
      w.unmaximize();
      check(
        "unmaximize -> !is_maximized",
        wait_state(&w, |s| !s.maximized, 6000).await,
      );
      check(
        "unmaximize fires a state event",
        wait_for(
          || saw(&|e| !e.state.maximized && e.previous.maximized),
          60,
          50,
        )
        .await,
      );
    } else {
      check(
        "maximize did not report a wrong state",
        w.get_state().is_normal(),
      );
      wm_na("maximize (no window manager applied it)");
    }

    // minimize / restore
    w.minimize();
    if wait_state(&w, |s| s.minimized, 6000).await {
      check("minimize -> is_minimized", true);
      check(
        "minimize fires a state event",
        wait_for(
          || saw(&|e| e.state.minimized && !e.previous.minimized),
          60,
          50,
        )
        .await,
      );
      w.restore();
      check(
        "restore -> !is_minimized",
        wait_state(&w, |s| !s.minimized, 6000).await,
      );
      check(
        "restore fires a state event",
        wait_for(
          || saw(&|e| !e.state.minimized && e.previous.minimized),
          60,
          50,
        )
        .await,
      );
    } else {
      check(
        "minimize did not report a wrong state",
        !w.get_state().maximized && !w.get_state().fullscreen,
      );
      wm_na("minimize (no window manager applied it)");
      w.restore();
    }
    tokio::time::sleep(std::time::Duration::from_millis(500)).await;

    // fullscreen
    let fs_before_pos = w.get_position();
    let fs_before_size = w.get_size();
    w.set_fullscreen(true);
    if wait_state(&w, |s| s.fullscreen, 10000).await {
      check("set_fullscreen(true) -> is_fullscreen", true);
      check(
        "entering fullscreen fires a state event",
        wait_for(
          || saw(&|e| e.state.fullscreen && !e.previous.fullscreen),
          60,
          50,
        )
        .await,
      );
      if caps.normal_bounds() {
        // Long enough for the transition's last frame to have "settled"
        // (laufey_window.h kNormalBoundsSettleMs), which is how the
        // fullscreen frame used to become the normal bounds on macOS.
        tokio::time::sleep(std::time::Duration::from_millis(800)).await;
        match w.get_normal_bounds() {
          Some(r) => {
            let size_ok = (r.width - fs_before_size.0).abs() <= 4
              && (r.height - fs_before_size.1).abs() <= 4;
            let pos_ok = !caps.set_position()
              || ((r.x - fs_before_pos.0).abs() <= 40
                && (r.y - fs_before_pos.1).abs() <= 40);
            check(
              &format!(
                "normal bounds while fullscreen are the pre-fullscreen bounds (got {r:?}, before {fs_before_pos:?} {fs_before_size:?})"
              ),
              size_ok && pos_ok,
            );
          }
          None => check("normal bounds available while fullscreen", false),
        }
      }
      w.set_fullscreen(false);
      check(
        "set_fullscreen(false) -> !is_fullscreen",
        wait_state(&w, |s| !s.fullscreen, 10000).await,
      );
      check(
        "leaving fullscreen fires a state event",
        wait_for(
          || saw(&|e| !e.state.fullscreen && e.previous.fullscreen),
          60,
          50,
        )
        .await,
      );
      // macOS animates the exit; let it settle before the next checks.
      tokio::time::sleep(std::time::Duration::from_millis(1000)).await;
    } else {
      check(
        "fullscreen did not report a wrong state",
        !w.get_state().maximized,
      );
      wm_na("fullscreen (no window manager applied it)");
    }
    let events = events.lock().unwrap().clone();
    check(
      "state events carry this window's id",
      events.iter().all(|e| e.window_id == id),
    );
    check(
      "no state event repeats its previous state",
      events.iter().all(|e| e.state != e.previous),
    );
  } else {
    na("window state (not supported by this backend)");
  }

  // -- title bar / traffic lights ----------------------------------------------
  let hidden = w.set_titlebar_style(laufey::TitlebarStyle::Hidden);
  check(
    "set_titlebar_style(Hidden) succeeds exactly when reported",
    hidden == caps.titlebar_hidden(),
  );
  if hidden {
    // The content now starts at the top of the frame.
    let under = wait_for(
      || {
        let frame = w.get_position();
        let inner = w.get_inner_position();
        inner.1 == frame.1
      },
      40,
      50,
    )
    .await;
    check("hidden title bar: content extends under it", under);
    let inset = w.set_titlebar_style(laufey::TitlebarStyle::HiddenInset);
    check(
      "set_titlebar_style(HiddenInset) succeeds exactly when reported",
      inset == caps.titlebar_hidden_inset(),
    );
    let traffic = w.set_traffic_light_position(Some((20, 18)));
    check(
      "set_traffic_light_position succeeds exactly when reported",
      traffic == caps.traffic_light_position(),
    );
    w.set_traffic_light_position(None);
    check(
      "set_titlebar_style(Default) restores the title bar",
      w.set_titlebar_style(laufey::TitlebarStyle::Default),
    );
    let back = wait_for(
      || {
        let frame = w.get_position();
        let inner = w.get_inner_position();
        inner.1 > frame.1
      },
      40,
      50,
    )
    .await;
    check("default title bar: content below it again", back);
  } else {
    check(
      "traffic lights refused without the capability",
      !w.set_traffic_light_position(Some((20, 18)))
        || caps.traffic_light_position(),
    );
    na("title bar styles (not supported by this backend)");
  }

  // -- backdrops ----------------------------------------------------------------
  for (name, backdrop, cap) in [
    ("mica", laufey::Backdrop::Mica, caps.mica()),
    ("acrylic", laufey::Backdrop::Acrylic, caps.acrylic()),
    ("mica-alt", laufey::Backdrop::MicaAlt, caps.mica_alt()),
    (
      "vibrancy",
      laufey::Backdrop::Vibrancy(laufey::VibrancyMaterial::Sidebar),
      caps.vibrancy(),
    ),
  ] {
    let applied = w.set_backdrop(backdrop);
    check(
      &format!(
        "set_backdrop({name}) succeeds exactly when reported (got {applied})"
      ),
      applied == cap,
    );
    if applied {
      check(
        &format!("set_backdrop(none) after {name}"),
        w.set_backdrop(laufey::Backdrop::None),
      );
    }
  }

  w.close();
}

/// LAUFEY_E2E_EXPECT_SCALE (scripts/native-e2e-run.sh --hidpi): the display
/// runs at a device scale factor of `scale`. laufey's sizes and positions
/// stay in DIPs (CSS pixels), the page's devicePixelRatio and the reported
/// scales are `scale`, and the window system really has the window at
/// `scale` times its DIP size and origin. (The WebView2 / CEF DIP handling
/// was only verified at 1x before.)
async fn hidpi_checks(w: &Window, title: &str, scale: f64) {
  let near = |got: f64| (got - scale).abs() < 0.01;
  let ws = w.get_scale_factor();
  check(
    &format!("HiDPI: the window's scale factor is {scale} (got {ws})"),
    near(ws),
  );
  if laufey::window_capabilities().screens() {
    let primary = laufey::screens().into_iter().find(|s| s.is_primary);
    let got = primary.map(|s| s.scale_factor).unwrap_or(0.0);
    check(
      &format!(
        "HiDPI: the primary screen's scale factor is {scale} (got {got})"
      ),
      near(got),
    );
  }
  match page_number(w, "window.devicePixelRatio").await {
    Some(dpr) => check(
      &format!("HiDPI: the page's devicePixelRatio is {scale} (got {dpr})"),
      near(dpr),
    ),
    None => na("HiDPI: devicePixelRatio (no page on this backend)"),
  }

  // Sizes are DIPs: the page is exactly that many CSS pixels, and the
  // window system has `scale` times as many physical pixels.
  let want = (600, 400);
  w.set_size(want.0, want.1);
  let sized = wait_for(
    || {
      let (sw, sh) = w.get_size();
      (sw - want.0).abs() <= 1 && (sh - want.1).abs() <= 1
    },
    60,
    50,
  )
  .await;
  let (sw, sh) = w.get_size();
  check(
    &format!("HiDPI: get_size is the DIP size set (got {sw}x{sh})"),
    sized,
  );
  check_page_size("HiDPI: the page area is the DIP size", w, want).await;
  if std::env::var_os("LAUFEY_E2E_HIDPI_DIAG").is_some() {
    // Experiment: is a lost early resize slow or gone?
    let t0 = std::time::Instant::now();
    let mut last = None;
    while t0.elapsed() < std::time::Duration::from_secs(10) {
      let got = page_inner_size(w).await;
      if got != last {
        eprintln!(
          "[e2e] DIAG {} ms: page {got:?} size {:?}",
          t0.elapsed().as_millis(),
          w.get_size()
        );
        last = got;
      }
      tokio::time::sleep(std::time::Duration::from_millis(100)).await;
    }
    w.set_size(601, 401);
    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
    eprintln!(
      "[e2e] DIAG after 601x401: page {:?}",
      page_inner_size(w).await
    );
    w.set_size(600, 400);
    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
    eprintln!(
      "[e2e] DIAG after 600x400: page {:?}",
      page_inner_size(w).await
    );
  }

  // Positions are DIPs too.
  w.set_position(100, 80);
  let placed = wait_for(
    || {
      let (x, y) = w.get_position();
      (x - 100).abs() <= 2 && (y - 80).abs() <= 2
    },
    60,
    50,
  )
  .await;
  let (px, py) = w.get_position();
  if placed {
    check("HiDPI: get_position is the DIP position set", true);
  } else if cfg!(target_os = "linux") {
    // The window manager may place the frame elsewhere; the content-origin
    // check below still compares laufey's DIPs with the real pixels.
    na(&format!(
      "HiDPI: get_position is the DIP position set (WM placed it at {px},{py})"
    ));
  } else {
    check(
      &format!("HiDPI: get_position is the DIP position set (got {px},{py})"),
      false,
    );
  }
  tokio::time::sleep(std::time::Duration::from_millis(300)).await;

  let can_measure = cfg!(windows) || os_view::xdotool().is_some();
  match os_view::content_rect(title) {
    Some((x, y, pw, ph)) => {
      let (sw, sh) = w.get_size();
      let (ew, eh) = (sw as f64 * scale, sh as f64 * scale);
      check(
        &format!(
          "HiDPI: the window system has the content at {ew}x{eh} physical pixels (got {pw}x{ph})"
        ),
        (pw as f64 - ew).abs() <= scale + 1.0
          && (ph as f64 - eh).abs() <= scale + 1.0,
      );
      let (ix, iy) = w.get_inner_position();
      let (ex, ey) = (ix as f64 * scale, iy as f64 * scale);
      check(
        &format!(
          "HiDPI: the content origin {ix},{iy} (DIP) is at {ex},{ey} physical (got {x},{y})"
        ),
        (x as f64 - ex).abs() <= 2.0 * scale + 1.0
          && (y as f64 - ey).abs() <= 2.0 * scale + 1.0,
      );
    }
    None if can_measure => check(
      "HiDPI: the window system reports the window's geometry",
      false,
    ),
    None => {
      na("HiDPI: physical geometry (no way to ask the window system here)")
    }
  }
  w.set_size(520, 420);
  let _ = wait_for(|| (w.get_size().0 - 520).abs() <= 4, 40, 50).await;
}

/// A tray-only app: with no window at all, a click on the tray icon still
/// reaches the app (denoland/deno#36778 reported dead tray clicks once the
/// host window was hidden on WebView2). The click is posted to the tray's
/// message-only window exactly as Shell_NotifyIcon delivers it, so the
/// message path under test is the shipping one; only the OS-side click is
/// synthesized. Windows WebView2 / CEF only (their shared tray_win.cc).
#[cfg(target_os = "windows")]
async fn tray_click_with_no_window() {
  #[link(name = "user32")]
  extern "system" {
    fn FindWindowExW(
      parent: isize,
      child_after: isize,
      class: *const u16,
      window: *const u16,
    ) -> isize;
    fn PostMessageW(hwnd: isize, msg: u32, wparam: usize, lparam: isize)
      -> i32;
  }
  const HWND_MESSAGE: isize = -3;
  const WM_APP: u32 = 0x8000;
  const WM_LBUTTONUP: isize = 0x0202;
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  if backend != "webview" && backend != "cef" {
    na("tray click with no window (Windows WebView2 / CEF tray only)");
    return;
  }
  let clicked = Arc::new(AtomicBool::new(false));
  let tray = {
    let clicked = clicked.clone();
    TrayIcon::new().icon(TINY_PNG).on_click(move || {
      clicked.store(true, Ordering::SeqCst);
    })
  };
  if tray.id() == 0 {
    check("tray created with no window open", false);
    return;
  }
  let class: Vec<u16> = "LaufeyCommonTrayWindow\0".encode_utf16().collect();
  let find = || unsafe {
    FindWindowExW(HWND_MESSAGE, 0, class.as_ptr(), std::ptr::null())
  };
  // CEF creates it on its UI thread after create_tray_icon returns.
  let _ = wait_for(|| find() != 0, 100, 50).await;
  let hwnd = find();
  check("tray message window exists with no window open", hwnd != 0);
  if hwnd == 0 {
    return;
  }
  // WM_LAUFEY_COMMON_TRAYICON (tray_win.cc): wParam = tray id, LOWORD(lParam)
  // = the mouse message. The click handler is installed on the UI thread
  // asynchronously (CEF posts it as a task, which may run after a window
  // message posted now), so re-post until it lands.
  let mut posted = false;
  let mut reached = false;
  for _ in 0..20 {
    posted |= unsafe {
      PostMessageW(hwnd, WM_APP + 65, tray.id() as usize, WM_LBUTTONUP)
    } != 0;
    if wait_for(|| clicked.load(Ordering::SeqCst), 10, 50).await {
      reached = true;
      break;
    }
  }
  check("tray click posted", posted);
  check("a tray click reaches the app with no window open", reached);
}

#[cfg(not(target_os = "windows"))]
async fn tray_click_with_no_window() {
  na("tray click with no window (posted-message check is Windows-only)");
}

/// LAUFEY_E2E_ONLY=lifetime. Ends the process: keep-alive with no window,
/// then quit(), which must end the event loop (the backend then calls the
/// runtime's shutdown, observed here through `should_shutdown`).
async fn lifetime_checks() -> ! {
  let caps = laufey::window_capabilities();
  if caps.keep_alive() {
    laufey::set_quit_on_last_window_closed(false);
    let w = Window::new(300, 200).title("native-e2e-lifetime");
    let _ = wait_for(|| w.get_size().0 != 0, 100, 50).await;
    w.close();
    let closed = wait_for(|| w.get_size() == (0, 0), 100, 50).await;
    check("the last window closed", closed);
    // Still here, and the backend hasn't begun shutting down.
    tokio::time::sleep(std::time::Duration::from_millis(1500)).await;
    check(
      "keep-alive: the event loop survives its last window",
      !laufey::should_shutdown(),
    );
    // A window can be opened again (a tray app reopening its UI).
    let again = Window::new(300, 200).title("native-e2e-lifetime-2");
    check(
      "a window opens after the last one closed",
      wait_for(|| again.get_size().0 != 0, 100, 50).await,
    );
    again.close();
    let _ = wait_for(|| again.get_size() == (0, 0), 100, 50).await;
    tray_click_with_no_window().await;
  } else {
    na("keep-alive (not supported by this backend)");
  }
  // Quit with a window open, the usual case. The runtime must still be told
  // before the process ends: macOS WKWebView used to return from main under
  // it, unless AppKit's terminate-after-last-window check happened to run
  // first (which a closed last window above can schedule).
  let open_at_quit = Window::new(300, 200).title("native-e2e-lifetime-3");
  let _ = wait_for(|| open_at_quit.get_size().0 != 0, 100, 50).await;
  laufey::quit();
  let ended = wait_for(laufey::should_shutdown, 300, 50).await;
  check(
    "quit() ends the event loop (runtime shutdown begins)",
    ended,
  );
  // Report before the backend's teardown can race the exit code.
  finish();
}

/// The `code` of an error envelope, or "ok" for a credential.
fn passkey_code(envelope: &str) -> String {
  if envelope.starts_with(r#"{"ok":true,"#) {
    return "ok".into();
  }
  let key = r#""code":""#;
  match envelope.find(key) {
    Some(i) => envelope[i + key.len()..]
      .split('"')
      .next()
      .unwrap_or("")
      .to_string(),
    None => format!("malformed envelope: {envelope}"),
  }
}

fn passkey_get_options(rp_id: &str, timeout_ms: u32) -> String {
  format!(
    r#"{{"challenge":"AAECAwQFBgcICQoLDA0ODw","rpId":"{rp_id}","timeout":{timeout_ms},"userVerification":"preferred","allowCredentials":[]}}"#
  )
}

/// A request's envelope, or a FAIL when none arrives in time.
async fn passkey_answer(
  name: &str,
  fut: impl std::future::Future<Output = String>,
) -> String {
  match tokio::time::timeout(std::time::Duration::from_secs(30), fut).await {
    Ok(envelope) => envelope,
    Err(_) => {
      check(&format!("{name}: an answer within 30s"), false);
      String::new()
    }
  }
}

/// Passkeys (API >= 37). A real ceremony needs a person at the machine (see
/// docs/passkeys.md), so this covers what answers without one: the
/// capabilities per OS, the parser's refusals, an unknown window, one ceremony
/// at a time, and a real OS round trip — on macOS an unsigned host is refused
/// by the OS at once (`invalid_rp`); on Windows the request shows the system
/// dialog and laufey's own timeout ends it (`timeout`;
/// LAUFEY_E2E_PASSKEY_EXPECT overrides the expected code for a machine where
/// the dialog ends differently). Exactly-once delivery is covered by
/// backend-common's passkey_test (the Rust future can't observe a second
/// call).
async fn passkey_checks(window_id: u32) {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  let caps = laufey::passkey_capabilities();
  eprintln!(
    "[e2e] passkey capabilities: platform={} security_keys={}",
    caps.platform_authenticator, caps.security_keys
  );
  let supported = caps.platform_authenticator || caps.security_keys;

  if backend == "winit" || cfg!(target_os = "linux") {
    check("passkey capabilities: none here", !supported);
    let env = passkey_answer(
      "passkey get",
      laufey::passkey_get(window_id, &passkey_get_options("example.com", 2000)),
    )
    .await;
    check(
      &format!(
        "passkey request -> not_supported (got {})",
        passkey_code(&env)
      ),
      passkey_code(&env) == "not_supported",
    );
    return;
  }
  if cfg!(target_os = "macos") {
    check(
      "passkey capabilities: platform + security keys (macOS 12+)",
      caps.platform_authenticator && caps.security_keys,
    );
  } else if !caps.security_keys {
    // Windows without webauthn.dll (an old build / Server SKU).
    check(
      "passkey capabilities: none without webauthn.dll",
      !caps.platform_authenticator,
    );
    let env = passkey_answer(
      "passkey get",
      laufey::passkey_get(window_id, &passkey_get_options("example.com", 2000)),
    )
    .await;
    check(
      "passkey request without webauthn.dll -> not_supported",
      passkey_code(&env) == "not_supported",
    );
    return;
  } else {
    check("passkey capabilities: security keys (webauthn.dll)", true);
  }

  // The parser refuses before the OS sees anything.
  let env = passkey_answer(
    "passkey create, malformed challenge",
    laufey::passkey_create(
      window_id,
      r#"{"rp":{"id":"example.com","name":"x"},"user":{"id":"dXNlcg","name":"u"},"challenge":"not base64!"}"#,
    ),
  )
  .await;
  check(
    &format!(
      "passkey create with a malformed challenge -> unknown (got {})",
      passkey_code(&env)
    ),
    passkey_code(&env) == "unknown" && env.contains("challenge"),
  );
  for rp in ["", "https://example.com", "Example.com", "127.0.0.1"] {
    let env = passkey_answer(
      "passkey get, bad rpId",
      laufey::passkey_get(window_id, &passkey_get_options(rp, 2000)),
    )
    .await;
    check(
      &format!(
        "passkey get with rpId {rp:?} -> invalid_rp (got {})",
        passkey_code(&env)
      ),
      passkey_code(&env) == "invalid_rp",
    );
  }
  let env = passkey_answer(
    "passkey get, unknown window",
    laufey::passkey_get(999_999, &passkey_get_options("example.com", 2000)),
  )
  .await;
  check(
    &format!(
      "passkey get on an unknown window -> unknown (got {})",
      passkey_code(&env)
    ),
    passkey_code(&env) == "unknown" && env.contains("not found"),
  );

  // A real OS request, and a second one while it is in progress.
  let options = passkey_get_options("example.com", 2000);
  let first = laufey::passkey_get(window_id, &options);
  let second = laufey::passkey_get(window_id, &options);
  let second = passkey_answer("second passkey get", second).await;
  check(
    &format!(
      "a second passkey request while one runs -> already in progress (got {})",
      passkey_code(&second)
    ),
    passkey_code(&second) == "unknown"
      && second.contains("already in progress"),
  );
  let first = passkey_answer("passkey get", first).await;
  let code = passkey_code(&first);
  let expected: Vec<String> = match std::env::var("LAUFEY_E2E_PASSKEY_EXPECT") {
    Ok(v) if !v.is_empty() => vec![v],
    _ if cfg!(target_os = "macos") => vec!["invalid_rp".into()],
    // Windows: webauthn.dll shows its dialog on the runner's interactive
    // desktop and waits there for a security key nobody inserts. Nothing
    // answers before laufey's own 2 s timer (StartTimeout), which aborts
    // the ceremony with `timeout` and cancels the OS operation. Every
    // windows-latest run since passkeys landed got exactly that (10 of 10
    // CEF and WebView2 runs of the integration branch, 2026-10-01/02);
    // `cancelled` would mean the OS gave up first, `not_supported` that
    // webauthn.dll is missing (handled above), and `unknown` a failure.
    _ => vec!["timeout".into()],
  };
  check(
    &format!("passkey get from the OS -> one of {expected:?} (got {code})"),
    expected.contains(&code),
  );

  // The slot frees once the OS has ended the operation.
  let mut freed = false;
  for _ in 0..10 {
    let env = passkey_answer(
      "passkey get after the first",
      laufey::passkey_get(window_id, &passkey_get_options("example.com", 1000)),
    )
    .await;
    if !env.contains("already in progress") {
      freed = true;
      break;
    }
    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
  }
  check("the passkey slot frees after a request ends", freed);
}

/// Report the overall result and exit immediately (see "shutdown" above).
fn finish() -> ! {
  let failed = FAILED.load(Ordering::SeqCst);
  eprintln!("[e2e] OVERALL {}", if failed { "FAIL" } else { "PASS" });
  let _ = std::io::Write::flush(&mut std::io::stderr());
  // _exit avoids running C++ static destructors / atexit handlers in the
  // backend, which is where the teardown crash lives.
  unsafe { libc_exit(if failed { 1 } else { 0 }) };
}

extern "C" {
  #[link_name = "_exit"]
  fn libc_exit(code: i32) -> !;
}

laufey::main!(e2e_main);
