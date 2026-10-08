//! Single-instance / deep-link e2e (see docs/deep-links.md).
//!
//! One launch of this runtime is one process of the scenario driven by
//! `scripts/single-instance-e2e-run.sh`. It opens a window, prints
//! `[e2e] ready`, and then waits (up to `LAUFEY_E2E_SI_WAIT_MS`, default
//! 30 s) for what the driver told it to expect:
//!
//!   LAUFEY_E2E_SI_COLD_ARGC / _COLD_ARG_<i>
//!       its own arguments after the executable, read with std::env::args()
//!       — what a cold-start deep link or file looks like to the runtime.
//!   LAUFEY_E2E_SI_SECOND_ARGC / _SECOND_ARG_<i> / _SECOND_CWD
//!       one `second_instance` callback with exactly these arguments and
//!       working directory (a later launch forwarded by the lock).
//!   LAUFEY_E2E_SI_OPEN_URLS / _OPEN_URL_SUFFIX_<i>
//!       that many `open_url` callbacks, in order, each a `file://` or
//!       custom-scheme URL ending with the given suffix (macOS LaunchServices).
//!   LAUFEY_E2E_SI_NO_OPEN_URL=1
//!       no `open_url` callback at all during the wait.
//!   LAUFEY_E2E_SI_HOLD_MS
//!       stay up at least this long.
//!   LAUFEY_E2E_SI_HOLD_FILE
//!       stay up until this file exists (at most 120 s), so the driver can
//!       run another instance alongside however long that one takes to
//!       start, then release this one.
//!   LAUFEY_E2E_SI_LINGER_MS
//!       after quit(), keep the runtime thread (and with it the process and
//!       its single-instance lock) alive this long: an app that takes a
//!       while to end, for the launch-while-ending scenario.
//!   LAUFEY_E2E_SI_RESULT_FILE
//!       also write the PASS/FAIL lines and the OVERALL line to this file
//!       (for a launch the OS starts, whose output the driver can't see).
//!
//! Started as a headless worker (`<exe> run <script> ...`, or with
//! NODE_CHANNEL_FD / NEXT_PRIVATE_WORKER set: what every backend runs before
//! its single-instance check, with no web engine), it prints
//! `[e2e] headless worker args=[...]` and returns at once: no window, no
//! `laufey::run()`.
//!
//! Emits `[e2e] PASS/FAIL <name>` lines and a final `[e2e] OVERALL
//! PASS|FAIL`, then quits through the backend's normal path.

use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use laufey::Window;

static FAILED: AtomicBool = AtomicBool::new(false);
static LINES: Mutex<Vec<String>> = Mutex::new(Vec::new());

fn report(line: String) {
  eprintln!("{line}");
  LINES.lock().unwrap().push(line);
}

fn check(name: &str, ok: bool) {
  if ok {
    report(format!("[e2e] PASS {name}"));
  } else {
    report(format!("[e2e] FAIL {name}"));
    FAILED.store(true, Ordering::SeqCst);
  }
}

fn env(name: &str) -> Option<String> {
  std::env::var(name).ok().filter(|v| !v.is_empty())
}

fn env_num(name: &str) -> Option<u64> {
  env(name).and_then(|v| v.parse().ok())
}

/// `<prefix>_ARGC` values `<prefix>_ARG_<i>` (a missing value reads as "").
fn expected_args(prefix: &str) -> Option<Vec<String>> {
  let argc = env_num(&format!("{prefix}_ARGC"))?;
  Some(
    (0..argc)
      .map(|i| std::env::var(format!("{prefix}_ARG_{i}")).unwrap_or_default())
      .collect(),
  )
}

#[derive(Clone, Default)]
struct Seen {
  second: Vec<(Vec<String>, String)>,
  urls: Vec<String>,
}

fn e2e_main() {
  let args: Vec<String> = std::env::args().skip(1).collect();
  // The backend's classification (laufey_common::IsHeadlessWorkerLaunch),
  // which an embedder mirrors: the backend started no web engine for it.
  let forked = ["NODE_CHANNEL_FD", "NEXT_PRIVATE_WORKER"]
    .iter()
    .any(|name| std::env::var_os(name).is_some());
  if args.first().map(String::as_str) == Some("run") || forked {
    eprintln!("[e2e] headless worker args={args:?}");
    let _ = std::io::stderr().flush();
    return;
  }

  let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
  rt.block_on(async move {
    tokio::spawn(async { laufey::run().await });

    report(format!("[e2e] pid={} args={args:?}", std::process::id()));
    if let Some(want) = expected_args("LAUFEY_E2E_SI_COLD") {
      check(
        &format!("cold-start arguments visible to the runtime ({want:?})"),
        args == want,
      );
    }

    let launched = Instant::now();
    let seen = Arc::new(Mutex::new(Seen::default()));
    {
      let seen = seen.clone();
      laufey::on_second_instance(move |args, cwd| {
        eprintln!(
          "[e2e] second_instance after {} ms: args={args:?} cwd={cwd:?}",
          launched.elapsed().as_millis()
        );
        seen
          .lock()
          .unwrap()
          .second
          .push((args.to_vec(), cwd.to_string()));
      });
    }
    {
      let seen = seen.clone();
      laufey::on_open_url(move |url| {
        eprintln!("[e2e] open_url {url:?}");
        seen.lock().unwrap().urls.push(url.to_string());
      });
    }

    let win = Window::new(360, 240).title("single-instance-e2e");
    // A beat for the window to appear before announcing readiness.
    tokio::time::sleep(Duration::from_millis(500)).await;
    eprintln!("[e2e] ready ({} ms)", launched.elapsed().as_millis());
    let _ = std::io::stderr().flush();

    let second_want = expected_args("LAUFEY_E2E_SI_SECOND");
    let second_cwd = env("LAUFEY_E2E_SI_SECOND_CWD");
    let url_count = env_num("LAUFEY_E2E_SI_OPEN_URLS").unwrap_or(0) as usize;
    let no_url = env("LAUFEY_E2E_SI_NO_OPEN_URL").is_some();
    let hold =
      Duration::from_millis(env_num("LAUFEY_E2E_SI_HOLD_MS").unwrap_or(0));
    let wait =
      Duration::from_millis(env_num("LAUFEY_E2E_SI_WAIT_MS").unwrap_or(30000));
    let hold_file =
      env("LAUFEY_E2E_SI_HOLD_FILE").map(std::path::PathBuf::from);
    let hold_file_limit = Duration::from_secs(120);

    let start = Instant::now();
    loop {
      let done = {
        let s = seen.lock().unwrap();
        (second_want.is_none() || !s.second.is_empty())
          && s.urls.len() >= url_count
      };
      let elapsed = start.elapsed();
      let released = hold_file.as_ref().is_none_or(|f| f.exists());
      if !released {
        if elapsed >= hold_file_limit {
          check("released through LAUFEY_E2E_SI_HOLD_FILE", false);
          break;
        }
      } else if (done && elapsed >= hold) || elapsed >= wait.max(hold) {
        break;
      }
      tokio::time::sleep(Duration::from_millis(100)).await;
    }

    // A snapshot, so no lock is held across the awaits below.
    let s = seen.lock().unwrap().clone();
    if let Some(want) = second_want {
      check(
        "second_instance delivered exactly once",
        s.second.len() == 1,
      );
      if let Some((got_args, got_cwd)) = s.second.first() {
        check(
          &format!("second_instance arguments are {want:?}"),
          *got_args == want,
        );
        if let Some(cwd) = &second_cwd {
          // Compare case-insensitively on Windows, where the drive letter's
          // case depends on how the directory was entered.
          let same = if cfg!(windows) {
            got_cwd.eq_ignore_ascii_case(cwd)
          } else {
            got_cwd == cwd
          };
          check(
            &format!("second_instance working directory is {cwd:?}"),
            same,
          );
        }
      }
    } else {
      check("no second_instance callback", s.second.is_empty());
    }
    if url_count > 0 {
      check(
        &format!("{url_count} open_url callback(s)"),
        s.urls.len() == url_count,
      );
      for i in 0..url_count {
        let suffix = env(&format!("LAUFEY_E2E_SI_OPEN_URL_SUFFIX_{i}"))
          .unwrap_or_default();
        let got = s.urls.get(i).cloned().unwrap_or_default();
        check(
          &format!("open_url #{i} ends with {suffix:?} (got {got:?})"),
          !suffix.is_empty() && got.ends_with(&suffix),
        );
      }
    }
    if no_url {
      check(
        &format!("no open_url callback (got {:?})", s.urls),
        s.urls.is_empty(),
      );
    }

    let failed = FAILED.load(Ordering::SeqCst);
    report(format!(
      "[e2e] OVERALL {}",
      if failed { "FAIL" } else { "PASS" }
    ));
    let _ = std::io::stderr().flush();
    if let Some(path) = env("LAUFEY_E2E_SI_RESULT_FILE") {
      let mut text = LINES.lock().unwrap().join("\n");
      text.push('\n');
      if std::fs::write(&path, text).is_err() {
        eprintln!("[e2e] could not write {path}");
      }
    }

    win.close();
    tokio::time::sleep(Duration::from_millis(300)).await;
    laufey::quit();
    if let Some(ms) = env_num("LAUFEY_E2E_SI_LINGER_MS") {
      eprintln!("[e2e] quitting; lingering {ms} ms");
      let _ = std::io::stderr().flush();
      std::thread::sleep(Duration::from_millis(ms));
    }
  });
}

laufey::main!(e2e_main);
