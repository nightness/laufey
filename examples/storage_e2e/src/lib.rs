//! Per-app web storage e2e (LAUFEY_DATA_DIR / LAUFEY_APP_ID).
//!
//! One launch of this runtime is one step of the scenario driven by
//! `scripts/storage-e2e-run.sh`, which relaunches the backend with different
//! app ids / data dirs and asserts that storage persists per app and stays
//! isolated between apps. Each launch serves a page on a fixed loopback port
//! (so every launch sees the same origin), loads it, and then, by
//! `LAUFEY_E2E_STORAGE_MODE`:
//!
//!   write  stores `LAUFEY_E2E_STORAGE_VALUE` in localStorage and in a
//!          persistent cookie, checks it reads back, then quits normally so
//!          the engine flushes its profile to disk.
//!   read   reads both back and checks them against
//!          `LAUFEY_E2E_STORAGE_EXPECT` (must equal) and/or
//!          `LAUFEY_E2E_STORAGE_EXPECT_NOT` (must not equal).
//!
//! `LAUFEY_E2E_STORAGE_HOLD_MS` keeps the launch alive that long before it
//! quits (used to start a second instance against a running one).
//!
//! `LAUFEY_E2E_STORAGE_SCHEME=<name>` serves the page over that custom scheme
//! (`<name>://app/`, registered with `register_scheme_handler`) instead of
//! loopback HTTP, and also requires it to be a secure context. Used for CEF,
//! which only makes a scheme a real origin when it was declared at startup
//! (e.g. by the launch file). Cookies are not used in this mode: custom
//! schemes are not cookie-enabled on every engine, and on CEF touching
//! `document.cookie` there takes the renderer down.
//!
//! Emits `[e2e] PASS/FAIL <name>` lines and a final `[e2e] OVERALL PASS|FAIL`.
//! The process exits through the backend's normal quit path (which owns the
//! exit code), so the driver judges each step by the OVERALL line.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use laufey::{SchemeRequest, Value, Window};
use tokio::sync::oneshot;

static FAILED: AtomicBool = AtomicBool::new(false);

const KEY: &str = "laufey-e2e-storage";
const COOKIE: &str = "laufey_e2e_storage";

fn check(name: &str, ok: bool) {
  if ok {
    eprintln!("[e2e] PASS {name}");
  } else {
    eprintln!("[e2e] FAIL {name}");
    FAILED.store(true, Ordering::SeqCst);
  }
}

fn env(name: &str) -> Option<String> {
  std::env::var(name).ok().filter(|v| !v.is_empty())
}

/// Serves the same tiny page for every request on 127.0.0.1:`port`.
///
/// Each connection gets its own thread. A persistent profile remembers this
/// origin, so on a relaunch Chromium's loading predictor preconnects idle
/// sockets to it alongside the navigation's own connection; a server that
/// handles one connection at a time can block reading an idle preconnect and
/// never answer the request waiting on the other socket.
fn serve(port: u16) -> std::io::Result<()> {
  let listener = TcpListener::bind(("127.0.0.1", port))?;
  std::thread::spawn(move || {
    for stream in listener.incoming() {
      let Ok(stream) = stream else { continue };
      std::thread::spawn(move || answer(stream));
    }
  });
  Ok(())
}

/// Reads one request's headers from `stream` and answers with the page. A
/// connection that sends nothing (an unused preconnect) is dropped once the
/// peer closes it or the read times out.
fn answer(mut stream: TcpStream) {
  let _ = stream.set_read_timeout(Some(Duration::from_secs(30)));
  let mut buf = [0u8; 4096];
  let mut req = Vec::new();
  loop {
    match stream.read(&mut buf) {
      Ok(0) | Err(_) => break,
      Ok(n) => req.extend_from_slice(&buf[..n]),
    }
    if req.windows(4).any(|w| w == b"\r\n\r\n") || req.len() > 65536 {
      break;
    }
  }
  if req.is_empty() {
    return;
  }
  let body = "<!doctype html><title>storage-e2e</title><p>storage-e2e</p>";
  let _ = write!(
    stream,
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\
     Content-Length: {}\r\nCache-Control: no-store\r\n\
     Connection: close\r\n\r\n{body}",
    body.len()
  );
}

/// Serves the page for every request on the custom scheme.
fn serve_scheme(req: SchemeRequest) {
  let body = "<!doctype html><title>storage-e2e</title><p>storage-e2e</p>";
  let headers = vec![
    (
      "content-type".to_string(),
      "text/html; charset=utf-8".to_string(),
    ),
    ("cache-control".to_string(), "no-store".to_string()),
  ];
  req.exchange.begin(200, &headers);
  req.exchange.write(body.as_bytes());
  req.exchange.finish();
}

/// How an `execute_js` call ended.
enum Eval {
  /// The script's string result.
  String(String),
  /// The script threw (or its promise rejected): the error the engine gave.
  Exception(Value),
  /// It ran, but its result isn't a string.
  NotString(Value),
  /// No answer within the wait.
  Timeout(Duration),
}

impl std::fmt::Display for Eval {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      Eval::String(s) => write!(f, "string {s:?}"),
      Eval::Exception(e) => write!(f, "exception {}", describe(e)),
      Eval::NotString(v) => write!(f, "non-string result {}", describe(v)),
      Eval::Timeout(d) => write!(f, "no answer within {d:?}"),
    }
  }
}

/// A short description of a value for the log.
fn describe(v: &Value) -> String {
  match v {
    Value::Null => "null".into(),
    Value::Bool(b) => format!("bool {b}"),
    Value::Int(i) => format!("int {i}"),
    Value::Double(d) => format!("double {d}"),
    Value::String(s) => format!("{s:?}"),
    Value::List(l) => {
      format!(
        "[{}]",
        l.iter().map(describe).collect::<Vec<_>>().join(", ")
      )
    }
    Value::Dict(m) => {
      let mut keys: Vec<_> = m.iter().collect();
      keys.sort_by(|a, b| a.0.cmp(b.0));
      let parts: Vec<String> = keys
        .into_iter()
        .map(|(k, v)| format!("{k}: {}", describe(v)))
        .collect();
      format!("{{{}}}", parts.join(", "))
    }
    Value::Binary(b) => format!("{} bytes", b.len()),
  }
}

impl Eval {
  fn ok(self) -> Option<String> {
    match self {
      Eval::String(s) => Some(s),
      _ => None,
    }
  }
}

/// Runs `script` in the page and waits up to `wait` for its result.
async fn eval(win: &Window, script: &str, wait: Duration) -> Eval {
  let (tx, rx) = oneshot::channel::<Result<Value, Value>>();
  win.execute_js(
    script,
    Some(move |r: Result<Value, Value>| {
      let _ = tx.send(r);
    }),
  );
  match tokio::time::timeout(wait, rx).await {
    Ok(Ok(Ok(Value::String(s)))) => Eval::String(s),
    Ok(Ok(Ok(other))) => Eval::NotString(other),
    Ok(Ok(Err(e))) => Eval::Exception(e),
    // The callback was dropped without an answer: as good as no answer.
    Ok(Err(_)) | Err(_) => Eval::Timeout(wait),
  }
}

/// How long a script the scenario depends on (the secure-context check, the
/// storage write and read) may take to answer once the page is up. A launch
/// is a cold start: the renderer answers its first scripts in milliseconds on
/// an idle machine, but on a loaded Windows runner they took 5-11 s to come
/// back (measured with CPU contention on a 2-core VM), and the old 5 s bound
/// read that as "no value" and quit before the write had run.
const SCRIPT_WAIT: Duration = Duration::from_secs(30);

/// `"<localStorage value>|<cookie value>"`, each "" when absent.
const READ_JS: &str = r#"(() => {
  const m = document.cookie.match(/(?:^|; )laufey_e2e_storage=([^;]*)/);
  return (localStorage.getItem("laufey-e2e-storage") || "") + "|" +
    (m ? decodeURIComponent(m[1]) : "");
})()"#;

/// `"<localStorage value>|"`: READ_JS without the cookie.
const READ_LS_JS: &str =
  r#"((localStorage.getItem("laufey-e2e-storage") || "") + "|")"#;

fn split(pair: &str) -> (String, String) {
  let mut it = pair.splitn(2, '|');
  let ls = it.next().unwrap_or("").to_string();
  let cookie = it.next().unwrap_or("").to_string();
  (ls, cookie)
}

fn e2e_main() {
  let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
  rt.block_on(async move {
    tokio::spawn(async { laufey::run().await });

    let mode = env("LAUFEY_E2E_STORAGE_MODE").unwrap_or_default();
    let port: u16 = env("LAUFEY_E2E_STORAGE_PORT")
      .and_then(|p| p.parse().ok())
      .unwrap_or(0);
    let scheme = env("LAUFEY_E2E_STORAGE_SCHEME");
    eprintln!(
      "[e2e] storage mode={mode} port={port} scheme={scheme:?} LAUFEY_APP_ID={:?} LAUFEY_DATA_DIR={:?}",
      env("LAUFEY_APP_ID"),
      env("LAUFEY_DATA_DIR")
    );

    let (origin, served) = match &scheme {
      Some(name) => {
        // Registered before the first window, as the scheme contract requires.
        laufey::register_scheme_handler(name, serve_scheme);
        (format!("{name}://app"), true)
      }
      None => (
        format!("http://127.0.0.1:{port}"),
        port != 0 && serve(port).is_ok(),
      ),
    };
    check(&format!("serve {origin}"), served);

    let win = Window::new(480, 320)
      .title("storage-e2e")
      .load(&format!("{origin}/"));

    // Wait until the page's own origin answers (not about:blank or an error
    // page). Polled rather than via on_page_load, which CEF doesn't fire. A
    // poll sent while the first navigation commits can go unanswered; the
    // next one follows its 5 s wait.
    let started = Instant::now();
    let mut ready = false;
    let mut polls = 0;
    let mut unanswered = 0;
    if served {
      for _ in 0..100 {
        polls += 1;
        match eval(&win, "location.origin", Duration::from_secs(5)).await {
          Eval::String(o) if o == origin => {
            ready = true;
            break;
          }
          Eval::Timeout(_) => unanswered += 1,
          _ => {}
        }
        tokio::time::sleep(Duration::from_millis(200)).await;
      }
    }
    eprintln!(
      "[e2e] page check: {} ms, {polls} polls, {unanswered} unanswered",
      started.elapsed().as_millis()
    );
    check(&format!("page loaded at {origin}"), ready);
    if ready && scheme.is_some() {
      let t = Instant::now();
      let result =
        eval(&win, "String(window.isSecureContext)", SCRIPT_WAIT).await;
      eprintln!(
        "[e2e] isSecureContext ({} ms): {result}",
        t.elapsed().as_millis()
      );
      check(
        "custom-scheme page is a secure context",
        result.ok().as_deref() == Some("true"),
      );
    }
    // Cookies are only used over loopback HTTP (see the module docs).
    let check_cookie = scheme.is_none();
    let read_js = if check_cookie { READ_JS } else { READ_LS_JS };

    if ready {
      match mode.as_str() {
        "write" => {
          let value = env("LAUFEY_E2E_STORAGE_VALUE").unwrap_or_default();
          let set_cookie = if check_cookie {
            format!(
              r#"document.cookie = "{COOKIE}=" + encodeURIComponent("{value}") +
    "; max-age=86400; path=/; samesite=lax";"#
            )
          } else {
            String::new()
          };
          let script = format!(
            r#"(() => {{
  localStorage.setItem("{KEY}", "{value}");
  {set_cookie}
  return {read_js};
}})()"#
          );
          let t = Instant::now();
          let result = eval(&win, &script, SCRIPT_WAIT).await;
          eprintln!(
            "[e2e] write script ({} ms): {result}",
            t.elapsed().as_millis()
          );
          let got = result.ok().unwrap_or_default();
          let (ls, cookie) = split(&got);
          eprintln!("[e2e] wrote localStorage={ls:?} cookie={cookie:?}");
          check("localStorage write reads back", !value.is_empty() && ls == value);
          if check_cookie {
            check(
              "cookie write reads back",
              !value.is_empty() && cookie == value,
            );
          }
          // Give the engine a moment to commit before the normal quit below.
          tokio::time::sleep(Duration::from_millis(1500)).await;
        }
        "read" => {
          let t = Instant::now();
          let result = eval(&win, read_js, SCRIPT_WAIT).await;
          eprintln!(
            "[e2e] read script ({} ms): {result}",
            t.elapsed().as_millis()
          );
          let got = result.ok();
          check("read storage", got.is_some());
          let (ls, cookie) = split(&got.unwrap_or_default());
          eprintln!("[e2e] read localStorage={ls:?} cookie={cookie:?}");
          if let Some(want) = env("LAUFEY_E2E_STORAGE_EXPECT") {
            check(&format!("localStorage is {want:?}"), ls == want);
            if check_cookie {
              check(&format!("cookie is {want:?}"), cookie == want);
            }
          }
          if let Some(not) = env("LAUFEY_E2E_STORAGE_EXPECT_NOT") {
            check(&format!("localStorage is not {not:?}"), ls != not);
            if check_cookie {
              check(&format!("cookie is not {not:?}"), cookie != not);
            }
          }
        }
        other => check(&format!("known mode (got {other:?})"), false),
      }
    }

    // Stay up (e.g. so the driver can launch a second instance against it).
    if let Some(ms) = env("LAUFEY_E2E_STORAGE_HOLD_MS").and_then(|v| v.parse().ok()) {
      eprintln!("[e2e] holding for {ms} ms");
      tokio::time::sleep(Duration::from_millis(ms)).await;
    }

    let failed = FAILED.load(Ordering::SeqCst);
    eprintln!("[e2e] OVERALL {}", if failed { "FAIL" } else { "PASS" });
    let _ = std::io::stderr().flush();

    // Quit through the backend's normal path so the engine shuts down and
    // flushes storage; returning lets the backend join this thread.
    win.close();
    tokio::time::sleep(Duration::from_millis(300)).await;
    laufey::quit();
  });
}

laufey::main!(e2e_main);
