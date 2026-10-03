//! Incremental responses over a custom scheme.
//!
//! A page at `app://e2e-stream/` reads responses that never end while it
//! reads them (fetch, EventSource and XMLHttpRequest), aborts one through an
//! AbortController, and reads a binary body larger than the WebView2
//! backend's credit window. Each never-ending route writes a little, waits,
//! writes more, then keeps writing heartbeats until a write fails, so a page
//! that sees its first chunks proves they arrived before the response ended,
//! and the failed write proves the engine's cancellation reached the
//! handler. See docs/custom-schemes.md ("Streaming responses").
//!
//! Two more scenarios check that a write never blocks the writer, as the
//! runtime's event loop writes every response: `slow` writes a body while
//! the UI thread is busy (a WebKitGTK pipe used to block once 64 KiB were
//! unread), from one "event loop" thread that must still serve another
//! request meanwhile, to a page that reads slowly; `cap` writes far more
//! than a page that doesn't read can hold, and the response must fail at
//! the backend's cap (64 MiB) instead of growing without bound.

use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use laufey::{SchemeRequest, Value, Window};

use crate::{arg_bool, arg_string, check, na, wait_for};

pub const PAGE_URL: &str = "app://e2e-stream/";
const PAGE_PREFIX: &str = "app://e2e-stream";

/// Length of the binary body: over the WebView2 credit window (4 MiB) and
/// not a multiple of any plausible chunk size.
const BIN_LEN: usize = 6 * 1024 * 1024 + 13;

/// Length of the `slow` body: far over a pipe's 64 KiB.
const SLOW_LEN: usize = 4 * 1024 * 1024 + 7;
/// How long the UI thread is kept busy while `slow` / `cap` write.
const UI_BUSY_MS: u64 = 1500;
/// Bytes `cap` tries to write: over the 64 MiB cap.
const CAP_LEN: usize = 80 * 1024 * 1024;
/// The backends' cap on bytes queued for a page that isn't reading.
const CAP_BYTES: usize = 64 * 1024 * 1024;
/// What WebView2 posts to a page ahead of its reads (kCreditWindow).
const CREDIT_WINDOW: usize = 4 * 1024 * 1024;

/// The scenarios the page runs; each reports once.
const LABELS: [&str; 9] = [
  "fetch", "abort", "sse", "xhr", "bin", "fast", "slow", "cap", "nohead",
];
/// Labels whose never-ending route must see its write fail (the engine
/// cancelled the request) once the page is done with it.
const CANCELLED: [&str; 4] = ["fetch", "abort", "sse", "xhr"];

/// Shared between the scheme handler and the checks.
#[derive(Clone, Default)]
pub struct State {
  /// Never-ending routes whose write failed, by label.
  cancelled: Arc<Mutex<HashSet<String>>>,
  /// Never-ending routes whose exchange reported the cancel
  /// (SchemeExchange::is_cancelled, the backend's on_cancel), by label.
  on_cancel: Arc<Mutex<HashSet<String>>>,
  /// `slow`: how long writing the whole body took (ms), and whether every
  /// write was accepted.
  slow_write: Arc<Mutex<Option<(u128, bool)>>>,
  /// `cap`: bytes accepted before a write failed (None: none failed).
  cap_write: Arc<Mutex<Option<Option<usize>>>>,
}

/// Whether this backend caps what it holds for a page that isn't reading
/// (WKWebView hands every write to WebKit, which takes it all).
fn backend_caps_queue() -> bool {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  !(cfg!(target_os = "macos") && backend == "webview")
}

/// The single "event loop" thread `slow`, `ping` and `cap` are served from,
/// as a runtime serves every response from its one event loop thread: if a
/// write blocks it, nothing else is served.
type LoopJob = Box<dyn FnOnce() + Send>;

fn event_loop() -> &'static Mutex<mpsc::Sender<LoopJob>> {
  static LOOP: OnceLock<Mutex<mpsc::Sender<LoopJob>>> = OnceLock::new();
  LOOP.get_or_init(|| {
    let (tx, rx) = mpsc::channel::<LoopJob>();
    std::thread::spawn(move || {
      for job in rx {
        job();
      }
    });
    Mutex::new(tx)
  })
}

fn on_event_loop(job: impl FnOnce() + Send + 'static) {
  let _ = event_loop().lock().unwrap().send(Box::new(job));
}

/// Keep the UI thread busy for `ms` (it can't read anything meanwhile).
/// Returns once the busy task has started, or false if it never did.
fn block_ui_thread(ms: u64) -> bool {
  let started = Arc::new(AtomicBool::new(false));
  let s = started.clone();
  // Dispatched now; the future isn't needed.
  drop(laufey::spawn_on_ui_thread(move || {
    s.store(true, Ordering::SeqCst);
    std::thread::sleep(Duration::from_millis(ms));
  }));
  let start = Instant::now();
  while !started.load(Ordering::SeqCst) {
    if start.elapsed() > Duration::from_secs(5) {
      return false;
    }
    std::thread::sleep(Duration::from_millis(5));
  }
  true
}

fn pattern(len: usize) -> Vec<u8> {
  (0..len)
    .map(|i| ((i % 251) ^ ((i / 251) & 0xff)) as u8)
    .collect()
}

fn page_html() -> String {
  format!(
    r#"<!doctype html><html><head><meta charset="utf-8"><title>stream</title></head><body>
<script>
async function waitForBinding(name) {{
  for (let i = 0; i < 200; i++) {{
    if (typeof Laufey !== 'undefined' && typeof Laufey[name] === 'function') return;
    await new Promise(r => setTimeout(r, 50));
  }}
  throw new Error('binding never appeared: ' + name);
}}
const sleep = ms => new Promise(r => setTimeout(r, ms));
// Rejects after `ms` so a scenario that hangs still reports.
function deadline(p, ms, what) {{
  return Promise.race([p, sleep(ms).then(() => {{ throw new Error('timed out: ' + what); }})]);
}}
async function report(label, ok, detail) {{
  await waitForBinding('streamReport');
  await Laufey.streamReport(label, !!ok, String(detail));
}}
// Reads until `want` has arrived; resolves with the reader still open.
async function readUntil(reader, want) {{
  const dec = new TextDecoder();
  let text = '', reads = 0;
  while (!text.includes(want)) {{
    const {{ value, done }} = await reader.read();
    if (done) throw new Error('ended early after ' + JSON.stringify(text));
    reads++;
    text += dec.decode(value, {{ stream: true }});
  }}
  return {{ text, reads }};
}}
const scenarios = {{
  // A response that never ends delivers its first chunks, with its status
  // and headers, and cancelling the reader stops it.
  async fetch() {{
    const res = await fetch('/open?k=fetch');
    const reader = res.body.getReader();
    const got = await readUntil(reader, 'a|b|');
    await reader.cancel();
    const head = res.status === 201 && res.headers.get('x-e2e') === 'yes' &&
      !res.headers.has('x-laufey-stream');
    return [head, 'status ' + res.status + ' ' + res.statusText + ', x-e2e ' +
      res.headers.get('x-e2e') + ', ' + got.reads + ' read(s)'];
  }},
  // Aborting mid-body fails the pending read with an AbortError.
  async abort() {{
    const ac = new AbortController();
    const res = await fetch('/open?k=abort', {{ signal: ac.signal }});
    const reader = res.body.getReader();
    await readUntil(reader, 'a|');
    ac.abort();
    try {{
      for (;;) {{ const {{ done }} = await reader.read(); if (done) return [false, 'ended instead of aborting']; }}
    }} catch (e) {{
      return [e && e.name === 'AbortError', 'read rejected with ' + (e && e.name)];
    }}
  }},
  // EventSource: a default and a named event, the event id, then close().
  async sse() {{
    const es = new EventSource('/sse');
    const seen = [];
    await new Promise((resolve, reject) => {{
      es.onmessage = e => {{ seen.push('message:' + e.data); }};
      es.addEventListener('tick', e => {{ seen.push('tick:' + e.data + '#' + e.lastEventId); resolve(); }});
      es.onerror = () => {{ if (es.readyState === EventSource.CLOSED) reject(new Error('closed')); }};
    }});
    const open = es.readyState === EventSource.OPEN;
    es.close();
    const want = 'message:one,tick:two#7';
    return [open && seen.join(',') === want && es.readyState === EventSource.CLOSED,
            seen.join(',') + ' readyState ' + es.readyState];
  }},
  // XMLHttpRequest reaches LOADING with the first chunks in responseText.
  async xhr() {{
    const xhr = new XMLHttpRequest();
    const states = [];
    const text = await new Promise((resolve, reject) => {{
      xhr.onreadystatechange = () => {{
        states.push(xhr.readyState);
        if (xhr.readyState === 3 && xhr.responseText.includes('a|b|')) resolve(xhr.responseText);
      }};
      xhr.onerror = () => reject(new Error('xhr error'));
      xhr.open('GET', '/open?k=xhr');
      xhr.send();
    }});
    const status = xhr.status;
    xhr.abort();
    return [status === 201 && text.startsWith('a|b|'),
            'status ' + status + ', states ' + states.join(',')];
  }},
  // A large binary body arrives intact, byte for byte.
  async bin() {{
    const res = await fetch('/bin');
    const got = new Uint8Array(await res.arrayBuffer());
    const n = {bin_len};
    let bad = got.length === n ? -1 : 0;
    for (let i = 0; bad < 0 && i < n; i++) {{
      if (got[i] !== ((i % 251) ^ (Math.floor(i / 251) & 255))) bad = i;
    }}
    return [bad < 0, 'length ' + got.length + (bad >= 0 ? ', first difference at ' + bad : '')];
  }},
  // A response that is complete at once is unaffected.
  async fast() {{
    const res = await fetch('/fast');
    const text = await res.text();
    return [res.status === 200 && text === 'fast body', 'status ' + res.status + ' ' + JSON.stringify(text)];
  }},
  // A big body written while the UI thread was busy arrives intact to a
  // page that reads it slowly, and the writer's event loop serves another
  // request in the middle of it.
  async slow() {{
    const res = await fetch('/slow');
    const reader = res.body.getReader();
    const n = {slow_len};
    let got = 0, bad = -1, reads = 0, ping = null;
    for (;;) {{
      const {{ value, done }} = await reader.read();
      if (done) break;
      for (let i = 0; bad < 0 && i < value.length; i++) {{
        const k = got + i;
        if (value[i] !== ((k % 251) ^ (Math.floor(k / 251) & 255))) bad = k;
      }}
      got += value.length;
      if (++reads === 1) {{
        ping = deadline(fetch('/ping').then(r => r.text()), 8000, 'ping');
      }}
      await sleep(2);
    }}
    let pong = '';
    try {{ pong = await ping; }} catch (e) {{ pong = 'error: ' + (e && e.message); }}
    return [got === n && bad < 0 && pong === 'pong',
            'length ' + got + ', ' + reads + ' reads' + (bad >= 0 ? ', first difference at ' + bad : '') +
            ', ping ' + JSON.stringify(pong)];
  }},
  // A handler that finishes without ever sending a head: there is no
  // response, so the request fails instead of staying pending forever.
  async nohead() {{
    try {{
      const res = await deadline(fetch('/nohead'), 8000, 'nohead');
      return [false, 'resolved with status ' + res.status];
    }} catch (e) {{
      return [e && e.name === 'TypeError', 'rejected with ' + (e && e.name) + ': ' + (e && e.message)];
    }}
  }},
  // A response the page doesn't read fails once the backend holds its cap,
  // and the page's read rejects instead of the response ending short.
  async cap() {{
    if (!{caps}) return [true, 'N/A: this backend hands every write to the engine'];
    let res;
    try {{
      res = await fetch('/cap');
    }} catch (e) {{
      // The cap was reached before the head reached the page (WebKitGTK
      // reads nothing while its thread is busy): the response failed.
      return [true, 'fetch rejected (' + (e && e.name) + ')'];
    }}
    const reader = res.body.getReader();
    await sleep({busy_ms} + 2500);
    let got = 0;
    try {{
      for (;;) {{
        const {{ value, done }} = await reader.read();
        if (done) return [false, 'ended normally after ' + got + ' bytes'];
        got += value.length;
      }}
    }} catch (e) {{
      return [got < {cap_len}, 'read rejected (' + (e && e.name) + ') after ' + got + ' bytes'];
    }}
  }},
}};
(async () => {{
  for (const [label, run] of Object.entries(scenarios)) {{
    try {{
      const [ok, detail] = await deadline(run(), 15000, label);
      await report(label, ok, detail);
    }} catch (e) {{
      await report(label, false, 'error: ' + (e && e.message));
    }}
  }}
}})().catch(e => report('script', false, String(e && e.message)));
</script></body></html>"#,
    bin_len = BIN_LEN,
    slow_len = SLOW_LEN,
    caps = backend_caps_queue(),
    busy_ms = UI_BUSY_MS,
    cap_len = CAP_LEN,
  )
}

fn begin(req: &SchemeRequest, status: i32, content_type: &str) {
  let mut headers = vec![
    ("content-type".to_string(), content_type.to_string()),
    ("cache-control".to_string(), "no-store".to_string()),
  ];
  if status == 201 {
    headers.push(("x-e2e".to_string(), "yes".to_string()));
  }
  req.exchange.begin(status, &headers);
}

/// Writes `first`, then `second` after a pause, then heartbeats until a write
/// fails (recorded under `label`) or a minute passes.
fn never_ending(
  req: SchemeRequest,
  state: State,
  label: String,
  first: &str,
  second: &str,
  heartbeat: &str,
) {
  let mut failed = req.exchange.write(first.as_bytes()) < 0;
  if !failed {
    std::thread::sleep(Duration::from_millis(300));
    failed = req.exchange.write(second.as_bytes()) < 0;
  }
  let start = Instant::now();
  while !failed && start.elapsed() < Duration::from_secs(60) {
    std::thread::sleep(Duration::from_millis(200));
    failed = req.exchange.write(heartbeat.as_bytes()) < 0;
  }
  if failed {
    state.cancelled.lock().unwrap().insert(label.clone());
    // The backend's on_cancel: reported by the time the write fails, or
    // just after (it runs on the engine's thread).
    let start = Instant::now();
    while !req.exchange.is_cancelled()
      && start.elapsed() < Duration::from_secs(2)
    {
      std::thread::sleep(Duration::from_millis(20));
    }
    if req.exchange.is_cancelled() {
      state.on_cancel.lock().unwrap().insert(label);
    }
  }
  req.exchange.finish();
}

/// Serve `req` if it belongs to these checks; otherwise hand it back. Every
/// route runs on its own thread: the handler must not block the backend
/// thread it is called on.
pub fn serve(req: SchemeRequest, state: &State) -> Option<SchemeRequest> {
  let url = req.url.clone();
  let path = url.split(['?', '#']).next().unwrap_or("").to_string();
  if path == PAGE_PREFIX || path == PAGE_URL {
    begin(&req, 200, "text/html");
    req.exchange.write(page_html().as_bytes());
    req.exchange.finish();
    return None;
  }
  let Some(route) = path.strip_prefix("app://e2e-stream/") else {
    return Some(req);
  };
  let route = route.to_string();
  let label = url
    .split_once("k=")
    .map(|(_, k)| k.to_string())
    .unwrap_or_default();
  let state = state.clone();
  std::thread::spawn(move || match route.as_str() {
    "open" => {
      begin(&req, 201, "text/plain; charset=utf-8");
      never_ending(req, state, label, "a|", "b|", ".");
    }
    "sse" => {
      begin(&req, 200, "text/event-stream");
      never_ending(
        req,
        state,
        "sse".to_string(),
        "retry: 60000\ndata: one\n\n",
        "event: tick\nid: 7\ndata: two\n\n",
        ":hb\n\n",
      );
    }
    "bin" => {
      begin(&req, 200, "application/octet-stream");
      // Late enough that a buffering backend streams it instead.
      std::thread::sleep(Duration::from_millis(150));
      let body = pattern(BIN_LEN);
      for chunk in body.chunks(256 * 1024) {
        if req.exchange.write(chunk) < 0 {
          break;
        }
      }
      req.exchange.finish();
    }
    "fast" => {
      begin(&req, 200, "text/plain");
      req.exchange.write(b"fast body");
      req.exchange.finish();
    }
    // Finished without a head (scheme_response_begin never called).
    "nohead" => req.exchange.finish(),
    "slow" => {
      let state = state.clone();
      on_event_loop(move || {
        begin(&req, 200, "application/octet-stream");
        let body = pattern(SLOW_LEN);
        let busy = block_ui_thread(UI_BUSY_MS);
        let start = Instant::now();
        let mut all = busy;
        for chunk in body.chunks(64 * 1024) {
          if req.exchange.write(chunk) < 0 {
            all = false;
            break;
          }
        }
        *state.slow_write.lock().unwrap() =
          Some((start.elapsed().as_millis(), all));
        req.exchange.finish();
      });
    }
    "ping" => on_event_loop(move || {
      begin(&req, 200, "text/plain");
      req.exchange.write(b"pong");
      req.exchange.finish();
    }),
    "cap" => {
      let state = state.clone();
      on_event_loop(move || {
        begin(&req, 200, "application/octet-stream");
        // WebKitGTK reads eagerly whenever its thread is free: keep it busy
        // so the queue fills. The other engines stop reading for a page that
        // doesn't read; WebView2 must not be held, since it starts streaming
        // (and applies its cap) from a timer on the UI thread.
        if cfg!(target_os = "linux")
          && std::env::var("LAUFEY_E2E_BACKEND").as_deref() == Ok("webview")
        {
          block_ui_thread(UI_BUSY_MS);
        } else {
          // Late enough that WebView2 streams it instead of buffering.
          std::thread::sleep(Duration::from_millis(200));
        }
        let chunk = vec![0x5au8; 1024 * 1024];
        let mut accepted = 0usize;
        let mut failed_at = None;
        while accepted < CAP_LEN {
          if req.exchange.write(&chunk) < 0 {
            failed_at = Some(accepted);
            break;
          }
          accepted += chunk.len();
        }
        *state.cap_write.lock().unwrap() = Some(failed_at);
        req.exchange.finish();
      });
    }
    _ => {
      begin(&req, 404, "text/plain");
      req.exchange.finish();
    }
  });
  None
}

/// Opens the page and checks what it and the handler observed. Returns the
/// window so the caller decides when it closes.
pub async fn run(state: &State) -> Option<Window> {
  if !laufey::scheme_handlers_supported() {
    na("incremental custom-scheme responses (backend has no scheme handler support)");
    return None;
  }
  let reports: Arc<Mutex<HashMap<String, (bool, String)>>> =
    Arc::new(Mutex::new(HashMap::new()));
  let win = Window::new(320, 240)
    .title("native-e2e-stream")
    .bind("streamReport", {
      let reports = reports.clone();
      move |call| {
        let a = &call.args;
        reports
          .lock()
          .unwrap()
          .insert(arg_string(a, 0), (arg_bool(a, 1), arg_string(a, 2)));
        call.resolve(Value::Bool(true));
      }
    })
    .load(PAGE_URL);

  let done = wait_for(
    || {
      let r = reports.lock().unwrap();
      r.contains_key("script") || LABELS.iter().all(|l| r.contains_key(*l))
    },
    900,
    100,
  )
  .await;
  check("streaming page reported every scenario", done);
  let got = reports.lock().unwrap().clone();
  if let Some((_, detail)) = got.get("script") {
    check(&format!("streaming page script ran ({detail})"), false);
  }
  let describe = |label: &str| {
    match label {
    "fetch" => "fetch reads a never-ending response incrementally, with its status and headers",
    "abort" => "AbortController aborts a streaming fetch mid-body",
    "sse" => "EventSource receives events from a never-ending response, then closes",
    "xhr" => "XMLHttpRequest reaches LOADING with the first chunks of a never-ending response",
    "bin" => "a large binary body arrives intact",
    "slow" => "a big body written while the UI thread is busy arrives intact to a slow reader, and another request completes meanwhile",
    "cap" => "a response the page doesn't read fails at the backend's cap (the page's read rejects)",
    "nohead" => "a response finished without a head fails the request instead of leaving it pending",
    _ => "a response complete at once is unaffected",
  }
  };
  for label in LABELS {
    match got.get(label) {
      Some((ok, detail)) => {
        check(&format!("{} ({detail})", describe(label)), *ok)
      }
      None => check(&format!("{} (no report)", describe(label)), false),
    }
  }
  // The writer was never blocked: the whole `slow` body went out while the
  // UI thread (which reads it on WebKitGTK) was busy for UI_BUSY_MS.
  let slow = *state.slow_write.lock().unwrap();
  check(
    &format!(
      "writing a 4 MiB body never blocks the writer while the UI thread is busy for {UI_BUSY_MS} ms (ms, all accepted: {slow:?})"
    ),
    matches!(slow, Some((ms, true)) if ms < (UI_BUSY_MS as u128) * 2 / 3),
  );
  if backend_caps_queue() {
    let cap = *state.cap_write.lock().unwrap();
    check(
      &format!(
        "a write fails once 64 MiB are held for a page that isn't reading (bytes accepted before the failure: {cap:?})"
      ),
      // Up to the 64 MiB held plus WebView2's 4 MiB credit window already
      // posted to the page (wv2_scheme_stream.cc kCreditWindow), which the
      // page received but never read: 68 MiB on windows-11-arm.
      matches!(cap, Some(Some(n)) if (CAP_BYTES / 2..=CAP_BYTES + CREDIT_WINDOW + 1024 * 1024).contains(&n)),
    );
  } else {
    na("the queued-bytes cap (WKWebView hands every write to WebKit)");
  }
  // The engine's cancellation must reach the handler: its next write fails.
  let all_cancelled = wait_for(
    || {
      let c = state.cancelled.lock().unwrap();
      CANCELLED.iter().all(|l| c.contains(*l))
    },
    50,
    100,
  )
  .await;
  let cancelled = state.cancelled.lock().unwrap().clone();
  let mut missing: Vec<&str> = CANCELLED
    .iter()
    .copied()
    .filter(|l| !cancelled.contains(*l))
    .collect();
  missing.sort();
  check(
    &format!(
      "cancelling a never-ending response stops the handler's writes (still writing: {missing:?})"
    ),
    all_cancelled,
  );
  // ... and reaches the handler as a cancel (on_cancel -> is_cancelled),
  // not only as a failed write.
  let seen = state.on_cancel.lock().unwrap().clone();
  let mut unseen: Vec<&str> = CANCELLED
    .iter()
    .copied()
    .filter(|l| cancelled.contains(*l) && !seen.contains(*l))
    .collect();
  unseen.sort();
  check(
    &format!(
      "the engine's cancel reaches the handler (is_cancelled; not reported for: {unseen:?})"
    ),
    unseen.is_empty(),
  );
  Some(win)
}
