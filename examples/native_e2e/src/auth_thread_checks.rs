//! API 42: UI-thread tasks and auth sessions (`LAUFEY_E2E_ONLY=auth-thread`,
//! `scripts/native-e2e-run.sh <backend> --auth-thread`).
//!
//! UI-thread tasks: a task dispatched from the runtime thread runs on the
//! backend's UI thread, proved with the OS's own notion of it (macOS
//! `pthread_main_np`, Linux `gettid() == getpid()`, Windows: the thread that
//! owns the window's HWND), in order, exactly once, inline when already on
//! the UI thread; and once the event loop has ended (quit()), a task is
//! answered "not run" at once instead of stranding its caller.
//!
//! Auth sessions: the capabilities per OS; NOT_SUPPORTED off macOS (RFC
//! 8252: the system browser); on macOS argument refusals, a real
//! ASWebAuthenticationSession round trip through a loopback server to a
//! custom-scheme callback (ephemeral: no consent prompt), one session at a
//! time, cancellation through the test hook (the user closing the sheet),
//! through auth_session_cancel (API 43: the app giving up, after which the
//! next session runs), the anchor window closing, and, in CI, cancelling
//! while the OS consent
//! prompt of a non-ephemeral session is up (where the OS itself never
//! reports). auth_session_cancel answers false with no session running,
//! and off macOS (where none can run). See docs/auth-session.md.

use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use laufey::{AuthSessionErrorKind, UiThreadError, Window};

use crate::{check, finish, na, wait_for};

const CALLBACK_SCHEME: &str = "laufey-e2e-auth";

/// The OS's own answer to "is this the UI thread": the process main thread
/// on macOS and Linux (every laufey backend runs its loop there), the thread
/// that created `hwnd` on Windows.
#[allow(unused_variables)]
fn os_says_ui_thread(hwnd: usize) -> bool {
  #[cfg(target_os = "macos")]
  {
    extern "C" {
      fn pthread_main_np() -> i32;
    }
    unsafe { pthread_main_np() == 1 }
  }
  #[cfg(target_os = "linux")]
  {
    extern "C" {
      fn gettid() -> i32;
      fn getpid() -> i32;
    }
    unsafe { gettid() == getpid() }
  }
  #[cfg(target_os = "windows")]
  {
    extern "system" {
      fn GetCurrentThreadId() -> u32;
      fn GetWindowThreadProcessId(
        hwnd: *mut std::ffi::c_void,
        pid: *mut u32,
      ) -> u32;
    }
    let owner =
      unsafe { GetWindowThreadProcessId(hwnd as _, std::ptr::null_mut()) };
    owner != 0 && owner == unsafe { GetCurrentThreadId() }
  }
  #[cfg(not(any(
    target_os = "macos",
    target_os = "linux",
    target_os = "windows"
  )))]
  {
    false
  }
}

async fn ui_thread_checks() -> Option<Window> {
  let win = Window::new(320, 240).title("native-e2e-ui-thread");
  let _ = wait_for(|| win.get_size().0 != 0, 100, 50).await;
  #[cfg_attr(not(target_os = "windows"), allow(unused_mut))]
  let mut hwnd = win.get_window_handle() as usize;
  // CEF exposes no native handle: find the top-level window by its title.
  #[cfg(target_os = "windows")]
  if hwnd == 0 {
    extern "system" {
      fn FindWindowW(class: *const u16, title: *const u16) -> isize;
    }
    let title: Vec<u16> = "native-e2e-ui-thread"
      .encode_utf16()
      .chain(std::iter::once(0))
      .collect();
    for _ in 0..100 {
      hwnd = unsafe { FindWindowW(std::ptr::null(), title.as_ptr()) } as usize;
      if hwnd != 0 {
        break;
      }
      tokio::time::sleep(Duration::from_millis(50)).await;
    }
  }

  check(
    "the runtime's own thread is not the UI thread",
    !laufey::is_ui_thread() && !os_says_ui_thread(hwnd),
  );

  // Async: runs on the UI thread, by laufey's and by the OS's account.
  let got = tokio::time::timeout(
    Duration::from_secs(10),
    laufey::spawn_on_ui_thread(move || {
      (laufey::is_ui_thread(), os_says_ui_thread(hwnd))
    }),
  )
  .await;
  check(
    &format!("spawn_on_ui_thread runs the task on the UI thread ({got:?})"),
    matches!(got, Ok(Ok((true, true)))),
  );

  // Blocking, from a plain thread; and nested from the UI thread (inline,
  // no deadlock).
  let blocking = std::thread::spawn(move || {
    laufey::try_run_on_ui_thread(move || os_says_ui_thread(hwnd))
  })
  .join()
  .ok();
  check(
    &format!(
      "try_run_on_ui_thread blocks until the UI thread ran it ({blocking:?})"
    ),
    blocking == Some(Ok(true)),
  );
  let nested = tokio::time::timeout(
    Duration::from_secs(10),
    laufey::spawn_on_ui_thread(|| laufey::try_run_on_ui_thread(|| 42)),
  )
  .await;
  check(
    &format!("a UI-thread call made on the UI thread runs inline ({nested:?})"),
    matches!(nested, Ok(Ok(Ok(42)))),
  );
  // #79's blocking wrapper (hops on every platform now).
  let classic = std::thread::spawn(move || {
    laufey::run_on_ui_thread(move || os_says_ui_thread(hwnd))
  })
  .join()
  .ok();
  check(
    "run_on_ui_thread (littledivy/laufey#79) runs on the UI thread",
    classic == Some(true),
  );

  // In order, and exactly once each, from several threads at once.
  let order: Arc<Mutex<Vec<u32>>> = Arc::new(Mutex::new(Vec::new()));
  let futs: Vec<_> = (0..20u32)
    .map(|i| {
      let order = order.clone();
      laufey::spawn_on_ui_thread(move || order.lock().unwrap().push(i))
    })
    .collect();
  for f in futs {
    let _ = tokio::time::timeout(Duration::from_secs(10), f).await;
  }
  let seen = order.lock().unwrap().clone();
  check(
    &format!(
      "UI tasks run in the order they were dispatched ({} ran)",
      seen.len()
    ),
    seen == (0..20).collect::<Vec<_>>(),
  );
  let count = Arc::new(AtomicUsize::new(0));
  let threads: Vec<_> = (0..4)
    .map(|_| {
      let count = count.clone();
      std::thread::spawn(move || {
        for _ in 0..50 {
          let c = count.clone();
          let _ = laufey::try_run_on_ui_thread(move || {
            c.fetch_add(1, Ordering::SeqCst);
          });
        }
      })
    })
    .collect();
  for t in threads {
    let _ = t.join();
  }
  check(
    &format!(
      "200 blocking UI tasks from 4 threads each ran once ({})",
      count.load(Ordering::SeqCst)
    ),
    count.load(Ordering::SeqCst) == 200,
  );
  Some(win)
}

/// Every path the sign-in server was asked for, in order: a session that
/// never answers says whether the OS ever loaded its page.
static IDP_HITS: Mutex<Vec<String>> = Mutex::new(Vec::new());

/// A loopback "identity provider": `/auth/redirect?state=S` redirects to the
/// custom-scheme callback, `/auth/wait` is a sign-in page that never does.
fn start_idp() -> Option<String> {
  let listener = TcpListener::bind("127.0.0.1:0").ok()?;
  let port = listener.local_addr().ok()?.port();
  std::thread::spawn(move || {
    for stream in listener.incoming().flatten() {
      std::thread::spawn(move || {
        let mut reader = BufReader::new(match stream.try_clone() {
          Ok(s) => s,
          Err(_) => return,
        });
        let mut line = String::new();
        if reader.read_line(&mut line).is_err() {
          return;
        }
        // Drain the headers.
        loop {
          let mut h = String::new();
          match reader.read_line(&mut h) {
            Ok(0) | Err(_) => break,
            Ok(_) if h == "\r\n" || h == "\n" => break,
            Ok(_) => {}
          }
        }
        let path = line.split_whitespace().nth(1).unwrap_or("/").to_string();
        if let Ok(mut hits) = IDP_HITS.lock() {
          hits.push(path.clone());
        }
        let mut out = stream;
        let response = if let Some(q) = path.strip_prefix("/auth/redirect?") {
          let state = q
            .split('&')
            .find_map(|kv| kv.strip_prefix("state="))
            .unwrap_or("");
          format!(
            "HTTP/1.1 302 Found\r\nLocation: {CALLBACK_SCHEME}://cb?code=e2e-code&state={state}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
          )
        } else {
          let body = "<!doctype html><title>sign in</title><p>signing in…</p>";
          format!(
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
          )
        };
        let _ = out.write_all(response.as_bytes());
      });
    }
  });
  Some(format!("http://127.0.0.1:{port}"))
}

async fn answer(
  name: &str,
  fut: impl std::future::Future<Output = Result<String, laufey::AuthSessionError>>,
  secs: u64,
) -> Option<Result<String, laufey::AuthSessionError>> {
  match tokio::time::timeout(Duration::from_secs(secs), fut).await {
    Ok(r) => Some(r),
    Err(_) => {
      check(&format!("{name}: an answer within {secs}s"), false);
      None
    }
  }
}

fn kind(r: &Option<Result<String, laufey::AuthSessionError>>) -> String {
  match r {
    Some(Ok(url)) => format!("ok {url}"),
    Some(Err(e)) => e.kind.code().to_string(),
    None => "no answer".to_string(),
  }
}

/// How many times `cancel_restart_stress` cancels a session and starts the
/// next one at once (`LAUFEY_E2E_AUTH_STRESS`, default 10).
fn stress_iterations() -> usize {
  std::env::var("LAUFEY_E2E_AUTH_STRESS")
    .ok()
    .and_then(|v| v.parse().ok())
    .unwrap_or(10)
}

/// The number of times the sign-in server was asked for `path` so far.
fn idp_hits(path: &str) -> usize {
  IDP_HITS
    .lock()
    .map(|h| h.iter().filter(|p| p.as_str() == path).count())
    .unwrap_or(0)
}

/// A session cancelled by the app (auth_session_cancel) while its sheet is
/// up, and the next one started at once, over and over: every next session
/// must be presented and answered. ASWebAuthenticationSession is still
/// dismissing the cancelled sheet then, and a session started meanwhile was
/// now and then never presented and never answered, its slot held for good
/// (about 2% of macOS CI legs, where the battery did this once). The time
/// from the sheet's page loading to the cancel varies, to cover the
/// dismissal at different points.
async fn cancel_restart_stress(anchor_id: u32, idp: &str) {
  let n = stress_iterations();
  if n == 0 {
    na("auth session cancel / restart stress (LAUFEY_E2E_AUTH_STRESS=0)");
    return;
  }
  let mut unanswered = 0usize;
  let mut wrong = 0usize;
  let mut slowest = Duration::ZERO;
  for i in 0..n {
    let loaded = idp_hits("/auth/wait");
    let pending = laufey::auth_session_start(
      anchor_id,
      &format!("{idp}/auth/wait"),
      CALLBACK_SCHEME,
      true,
    );
    // The sheet is up once its page was asked for.
    let up = wait_for(|| idp_hits("/auth/wait") > loaded, 200, 50).await;
    tokio::time::sleep(Duration::from_millis((i as u64 * 97) % 400)).await;
    let cancelled = laufey::auth_session_cancel();
    let state = format!("stress{i}");
    let t = Instant::now();
    let next = laufey::auth_session_start(
      anchor_id,
      &format!("{idp}/auth/redirect?state={state}"),
      CALLBACK_SCHEME,
      true,
    );
    let first = tokio::time::timeout(Duration::from_secs(10), pending).await;
    let next = tokio::time::timeout(Duration::from_secs(30), next).await;
    slowest = slowest.max(t.elapsed());
    match &next {
      Ok(Ok(url)) if url.ends_with(&format!("state={state}")) => {}
      Err(_) => {
        unanswered += 1;
        let hits = IDP_HITS.lock().map(|h| h.len()).unwrap_or(0);
        eprintln!(
          "[e2e] INFO stress #{i}: the session after the cancel never answered (sheet up {up}, cancel {cancelled}, cancelled one {first:?}, {hits} sign-in requests so far)"
        );
        // Free the slot for the next iteration.
        laufey::auth_session_cancel();
        tokio::time::sleep(Duration::from_secs(1)).await;
      }
      Ok(other) => {
        wrong += 1;
        eprintln!(
          "[e2e] INFO stress #{i}: the session after the cancel answered {other:?} (sheet up {up}, cancel {cancelled}, cancelled one {first:?})"
        );
      }
    }
  }
  eprintln!(
    "[e2e] INFO auth cancel / restart stress: {n} runs, {unanswered} never answered, {wrong} answered wrongly, slowest next session {} ms",
    slowest.as_millis()
  );
  check(
    &format!(
      "a session started right after auth_session_cancel() is presented and answered, {n} times over ({unanswered} never answered, {wrong} wrong)"
    ),
    unanswered == 0 && wrong == 0,
  );
}

async fn auth_session_checks(anchor: Option<&Window>) {
  let caps = laufey::auth_session_capabilities();
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  let expect_supported = cfg!(target_os = "macos") && backend != "winit";
  check(
    &format!("auth session capabilities match the OS ({caps:?})"),
    caps.supported == expect_supported && caps.ephemeral == expect_supported,
  );
  if !caps.supported {
    let r = answer(
      "auth session without an OS auth session",
      laufey::auth_session_start(
        0,
        "https://example.com/authorize",
        "myapp",
        false,
      ),
      10,
    )
    .await;
    let rfc = matches!(&r, Some(Err(e)) if e.message.contains("RFC 8252"));
    check(
      &format!(
        "no OS auth session here: not_supported, pointing at the system browser ({})",
        kind(&r)
      ),
      matches!(&r, Some(Err(e)) if e.kind == AuthSessionErrorKind::NotSupported)
        && rfc,
    );
    // API 43: cancelling is safe where no session can run.
    check(
      "auth_session_cancel() with no session answers false",
      !laufey::auth_session_cancel(),
    );
    return;
  }
  check(
    "auth_session_cancel() with no session running answers false",
    !laufey::auth_session_cancel(),
  );

  // Refusals, synchronously and without touching the OS.
  for (name, url, cb, window, want) in [
    (
      "a non-http url",
      "myapp://x",
      "myapp",
      0,
      AuthSessionErrorKind::Invalid,
    ),
    (
      "an http(s) callback scheme",
      "https://example.com/",
      "https",
      0,
      AuthSessionErrorKind::Invalid,
    ),
    (
      "an unknown window",
      "https://example.com/",
      "myapp",
      999_999,
      AuthSessionErrorKind::Invalid,
    ),
  ] {
    let r =
      answer(name, laufey::auth_session_start(window, url, cb, true), 10).await;
    check(
      &format!("auth session refuses {name} ({})", kind(&r)),
      matches!(&r, Some(Err(e)) if e.kind == want),
    );
  }
  if !caps.https_callback {
    let r = answer(
      "an https callback before macOS 14.4",
      laufey::auth_session_start(
        0,
        "https://example.com/",
        "https://example.com/cb",
        true,
      ),
      10,
    )
    .await;
    check(
      &format!("an https callback needs macOS 14.4 ({})", kind(&r)),
      matches!(&r, Some(Err(e)) if e.kind == AuthSessionErrorKind::NotSupported),
    );
  }

  let Some(idp) = start_idp() else {
    check("auth session: loopback sign-in server", false);
    return;
  };
  let anchor_id = anchor.map(|w| w.id()).unwrap_or(0);

  // A real round trip: the OS loads the sign-in page, which redirects to the
  // callback scheme; the session resolves with that URL.
  let r = answer(
    "auth session round trip",
    laufey::auth_session_start(
      anchor_id,
      &format!("{idp}/auth/redirect?state=s1"),
      CALLBACK_SCHEME,
      true,
    ),
    60,
  )
  .await;
  let want = format!("{CALLBACK_SCHEME}://cb?code=e2e-code&state=s1");
  check(
    &format!(
      "ASWebAuthenticationSession signs in through the browser and returns the callback URL ({})",
      kind(&r)
    ),
    matches!(&r, Some(Ok(url)) if *url == want),
  );

  // One at a time, and the test hook cancels as the user closing the sheet.
  let first = laufey::auth_session_start(
    anchor_id,
    &format!("{idp}/auth/wait"),
    CALLBACK_SCHEME,
    true,
  );
  tokio::time::sleep(Duration::from_millis(1500)).await;
  let second = answer(
    "a second auth session",
    laufey::auth_session_start(
      anchor_id,
      &format!("{idp}/auth/wait"),
      CALLBACK_SCHEME,
      true,
    ),
    10,
  )
  .await;
  check(
    &format!(
      "a second auth session meanwhile is busy ({})",
      kind(&second)
    ),
    matches!(&second, Some(Err(e)) if e.kind == AuthSessionErrorKind::Busy),
  );
  let cancelled = laufey::test_cancel_auth_session();
  let first = answer("the cancelled auth session", first, 10).await;
  check(
    &format!(
      "closing the sheet ends the session cancelled ({cancelled}, {})",
      kind(&first)
    ),
    cancelled
      && matches!(&first, Some(Err(e)) if e.kind == AuthSessionErrorKind::Cancelled),
  );
  check(
    "no session left to cancel afterwards",
    !laufey::test_cancel_auth_session(),
  );

  // The app cancels (API 43: the page gave up, a timeout): the sheet goes,
  // the session ends cancelled once, and the next one isn't busy.
  let pending = laufey::auth_session_start(
    anchor_id,
    &format!("{idp}/auth/wait"),
    CALLBACK_SCHEME,
    true,
  );
  tokio::time::sleep(Duration::from_millis(1500)).await;
  let cancelled = laufey::auth_session_cancel();
  let again = laufey::auth_session_cancel();
  let r = answer("the auth session the app cancelled", pending, 10).await;
  check(
    &format!(
      "auth_session_cancel() ends the running session cancelled ({cancelled}, then {again}, {})",
      kind(&r)
    ),
    cancelled
      && !again
      && matches!(&r, Some(Err(e)) if e.kind == AuthSessionErrorKind::Cancelled),
  );
  let next = laufey::auth_session_start(
    anchor_id,
    &format!("{idp}/auth/redirect?state=s2"),
    CALLBACK_SCHEME,
    true,
  );
  let next = answer("the auth session after a cancel", next, 60).await;
  check(
    &format!(
      "after auth_session_cancel() the next session runs ({})",
      kind(&next)
    ),
    matches!(&next, Some(Ok(url)) if url.ends_with("state=s2")),
  );
  if next.is_none() {
    // Say whether the OS ever loaded the page, and free the slot so the
    // checks below test what they name instead of failing busy.
    let hits = IDP_HITS.lock().map(|h| h.clone()).unwrap_or_default();
    eprintln!("[e2e] INFO sign-in server requests so far: {hits:?}");
    let freed = laufey::auth_session_cancel();
    eprintln!("[e2e] INFO cancelled the unanswered session: {freed}");
  }

  cancel_restart_stress(anchor_id, &idp).await;

  // The anchor window closing ends the session cancelled.
  let w = Window::new(320, 240).title("native-e2e-auth-anchor");
  let _ = wait_for(|| w.get_size().0 != 0, 100, 50).await;
  let pending = laufey::auth_session_start(
    w.id(),
    &format!("{idp}/auth/wait"),
    CALLBACK_SCHEME,
    true,
  );
  tokio::time::sleep(Duration::from_millis(1500)).await;
  w.close();
  let r = answer("the auth session on a closed window", pending, 10).await;
  check(
    &format!(
      "closing the anchor window ends the session cancelled ({})",
      kind(&r)
    ),
    matches!(&r, Some(Err(e)) if e.kind == AuthSessionErrorKind::Cancelled),
  );

  // A non-ephemeral session shows the OS consent prompt first, and the OS
  // never reports a cancel made while it is up; laufey must still answer.
  // Only in CI: it puts a prompt on a developer's screen.
  if std::env::var("CI").is_ok()
    || std::env::var("LAUFEY_E2E_AUTH_PROMPT").is_ok()
  {
    let pending = laufey::auth_session_start(
      anchor_id,
      &format!("{idp}/auth/wait"),
      CALLBACK_SCHEME,
      false,
    );
    tokio::time::sleep(Duration::from_millis(1500)).await;
    let cancelled = laufey::test_cancel_auth_session();
    let r = answer("the auth session at its consent prompt", pending, 10).await;
    check(
      &format!(
        "cancelling at the OS consent prompt still ends the session ({cancelled}, {})",
        kind(&r)
      ),
      cancelled && matches!(&r, Some(Err(e)) if e.kind == AuthSessionErrorKind::Cancelled),
    );
  } else {
    na("auth session cancelled at the OS consent prompt (CI only: it shows a prompt)");
  }
}

/// Ends the process: quit(), then UI tasks after the loop ended.
async fn after_shutdown_checks() -> ! {
  // A task queued while the loop is ending is answered either way.
  let racing = laufey::spawn_on_ui_thread(|| ());
  laufey::quit();
  let ended = wait_for(laufey::should_shutdown, 300, 50).await;
  check("quit() ends the event loop", ended);
  let racing = tokio::time::timeout(Duration::from_secs(5), racing).await;
  check(
    &format!("a UI task racing quit() is answered ({racing:?})"),
    racing.is_ok(),
  );
  let t = Instant::now();
  let late = laufey::try_run_on_ui_thread(|| ());
  let fut = tokio::time::timeout(
    Duration::from_secs(5),
    laufey::spawn_on_ui_thread(|| ()),
  )
  .await;
  check(
    &format!(
      "after the loop ended a UI task is refused at once ({late:?}, {fut:?}, {} ms)",
      t.elapsed().as_millis()
    ),
    late == Err(UiThreadError::Shutdown)
      && matches!(fut, Ok(Err(UiThreadError::Shutdown)))
      && t.elapsed() < Duration::from_secs(2),
  );
  finish();
}

pub async fn run() -> ! {
  let win = ui_thread_checks().await;
  auth_session_checks(win.as_ref()).await;
  after_shutdown_checks().await
}
