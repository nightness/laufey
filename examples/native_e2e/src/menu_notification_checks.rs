// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! LAUFEY_E2E_ONLY=menus-notifications: menus and notifications (API 41).
//! See docs/e2e-testing.md.
//!
//! Menus:
//! - An app-menu accelerator fires its item through the backend's own
//!   accelerator dispatch (test hook: -[NSMenu performKeyEquivalent:],
//!   TranslateAccelerator, gtk_accel_groups_activate, the CEF window
//!   accelerator); a disabled item's doesn't; on Windows also a real key
//!   press (SendInput) while the window has the focus.
//! - A context menu shown with the close callback is dismissed through the
//!   test hook: the close callback fires exactly once, with no click; an
//!   empty menu reports its close at once.
//! - While a context menu is open the app keeps running: a page timer keeps
//!   reaching the runtime through a binding, and a synchronous UI-thread
//!   call (a window getter, `run_on_ui_thread`) returns. (CEF on Windows
//!   used to stop running its tasks inside TrackPopupMenu's modal loop.)
//! - Windows: an item chosen from the keyboard (SendInput Down + Return
//!   while the web content has the focus) fires its click.
//!
//! Notifications:
//! - Responses: a click before any response handler is buffered and
//!   delivered with "launch": true when one registers; later ones directly.
//! - A live notification's actions and body clicks reach its callback
//!   through the OS response dispatch (test hook), close reports CLOSED.
//! - Scheduling: a notification scheduled two minutes out is listed (tag and
//!   time), cancelled and gone from the list; one scheduled two seconds out
//!   leaves the list once delivered.
//! - Linux (with the mock notification server the CI step runs): a real
//!   ActionInvoked signal reaches the live callback.
//! - Windows: the COM activator answers a CoCreateInstance + Activate (what
//!   Windows does for a click on a toast), and the response arrives.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime};

use laufey::{
  MenuItem, Notification, NotificationEvent, NotificationResponse,
  PermissionKind, PermissionStatus, Window,
};

use super::{check, na, wait_for};

const ACCEL: &str = "CommandOrControl+Shift+F9";

/// LAUFEY_E2E_SCREENSHOT_PAUSE_MS: hold the app menu and the open context
/// menu on screen that long, for a screenshot during a manual run.
async fn screenshot_pause() {
  if let Some(ms) = std::env::var("LAUFEY_E2E_SCREENSHOT_PAUSE_MS")
    .ok()
    .and_then(|v| v.parse::<u64>().ok())
  {
    tokio::time::sleep(Duration::from_millis(ms)).await;
  }
}
const DISABLED_ACCEL: &str = "CommandOrControl+Shift+F8";

pub async fn run() {
  let mcaps = laufey::menu_capabilities();
  let ncaps = laufey::notification_capabilities();
  eprintln!(
    "[e2e] menu capabilities = {:#x}, notification capabilities = {:#x}",
    mcaps.bits, ncaps.bits
  );
  let w = Window::new(480, 360).title("native-e2e-menus");
  w.show();
  tokio::time::sleep(Duration::from_millis(800)).await;
  // LAUFEY_E2E_MN_PARTS=accel,context,notify (default all): a subset, for
  // bisecting a failure by hand.
  let parts = std::env::var("LAUFEY_E2E_MN_PARTS")
    .unwrap_or_else(|_| "accel,context,notify".into());
  if parts.contains("accel") {
    accelerator_checks(&w, &mcaps).await;
  }
  if parts.contains("context") {
    context_menu_checks(&w, &mcaps).await;
    context_menu_live_checks(&mcaps).await;
  }
  if parts.contains("notify") {
    notification_checks(&ncaps).await;
  }
}

/// The process COM started for a toast click (its command line has
/// "-ToastActivated"): the click reaches the response handler, as the
/// launch, and is written to `%TEMP%\laufey-coldstart-result.txt`
/// ("tag\naction\ndata\nlaunch\nargs\nargv") for the script that clicked:
/// `args` whether the runtime's arguments are free of D-Bus activation's,
/// `argv` what this library's `.init_array` function saw (`argv_at_load`).
/// Linux D-Bus activation runs it too. Ends the process.
pub async fn cold_start() -> ! {
  let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();
  laufey::set_notification_response_handler(move |r| {
    let _ = tx.send(r);
  });
  let out = std::env::temp_dir().join("laufey-coldstart-result.txt");
  let got = tokio::time::timeout(Duration::from_secs(20), rx.recv()).await;
  // D-Bus activation's argument is the host's: the runtime's arguments
  // (laufey::args_os) leave it out.
  let args = if laufey::args_os()
    .iter()
    .any(|a| a == laufey::DBUS_ACTIVATION_ARG)
  {
    "args-leaked"
  } else {
    "args-clean"
  };
  let text = match got {
    Ok(Some(r)) => format!(
      "{}\n{}\n{}\n{}\n{}\n{}\n",
      r.tag,
      r.action.unwrap_or_default(),
      r.data.unwrap_or_default(),
      r.launch,
      args,
      super::argv_at_load()
    ),
    _ => "timeout\n".to_string(),
  };
  let _ = std::fs::write(&out, text);
  eprintln!("[e2e-coldstart] wrote {}", out.display());
  let _ = std::io::Write::flush(&mut std::io::stderr());
  // As finish(): _exit skips the backend's static teardown.
  unsafe { super::libc_exit(0) }
}

/// A step of scripts/notification-coldstart-e2e.sh: post the notification
/// the script then clicks (LAUFEY_E2E_NOTIFY_POST: tag "cold-tag", data
/// {"c":1}, the action "cold-action"), or schedule "sched-tag" that many
/// milliseconds ahead (LAUFEY_E2E_NOTIFY_SCHEDULE_MS), print the
/// capabilities and platform features, and quit. Ends the process.
pub async fn post_and_quit() -> ! {
  let caps = laufey::notification_capabilities();
  eprintln!("[e2e-notify] notification capabilities = {:#x}", caps.bits);
  if let Some(json) = laufey::platform_features() {
    eprintln!("[e2e-notify] platform features = {json}");
  }
  let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();
  if let Ok(ms) = std::env::var("LAUFEY_E2E_NOTIFY_SCHEDULE_MS") {
    let ms: i64 = ms.parse().unwrap_or(10_000);
    let now = std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .map(|d| d.as_millis() as i64)
      .unwrap_or(0);
    let _handle = Notification::new("laufey scheduled e2e")
      .body("posted by the app's systemd timer")
      .tag("sched-tag")
      .data(r#"{"s":1}"#)
      .action("sched-action", "Open")
      .schedule_at_ms(now + ms)
      .show();
    eprintln!("[e2e-notify] scheduled sched-tag {ms} ms ahead");
  } else {
    let _handle = Notification::new("laufey cold-start e2e")
      .body("click me after the app quit")
      .tag("cold-tag")
      .data(r#"{"c":1}"#)
      .action("cold-action", "Act")
      .on_event(move |e| {
        let _ = tx.send(format!("{e:?}"));
      });
    let shown = tokio::time::timeout(Duration::from_secs(10), rx.recv()).await;
    eprintln!("[e2e-notify] posted cold-tag: {shown:?}");
  }
  let _ = std::io::Write::flush(&mut std::io::stderr());
  unsafe { super::libc_exit(0) }
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

async fn accelerator_checks(w: &Window, caps: &laufey::MenuCapabilities) {
  let clicks: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
  {
    let clicks = clicks.clone();
    w.set_menu(
      &[MenuItem::Submenu {
        label: "File".into(),
        items: vec![
          MenuItem::Item {
            label: "Accelerated".into(),
            id: Some("accel-item".into()),
            accelerator: Some(ACCEL.into()),
            enabled: true,
            checked: false,
            icon: None,
            tooltip: Some("fires from the keyboard".into()),
          },
          MenuItem::Item {
            label: "Disabled".into(),
            id: Some("disabled-item".into()),
            accelerator: Some(DISABLED_ACCEL.into()),
            enabled: false,
            checked: false,
            icon: None,
            tooltip: None,
          },
        ],
      }],
      move |id| clicks.lock().unwrap().push(id.to_string()),
    );
  }
  if !caps.accelerators() {
    na("menu accelerators (not supported by this backend)");
    check(
      "test_trigger_menu_accelerator reports nothing without the capability",
      !laufey::test_trigger_menu_accelerator(w, ACCEL),
    );
    return;
  }
  screenshot_pause().await;
  // set_menu applies on the UI thread; wait until the accelerator is bound.
  let fired =
    wait_for(|| laufey::test_trigger_menu_accelerator(w, ACCEL), 50, 100).await;
  check("an app-menu accelerator fires its item", fired);
  check(
    "the accelerator's click carries the item id",
    wait_for(
      || clicks.lock().unwrap().iter().any(|c| c == "accel-item"),
      50,
      20,
    )
    .await,
  );
  check(
    "a disabled item's accelerator fires nothing",
    !laufey::test_trigger_menu_accelerator(w, DISABLED_ACCEL),
  );
  check(
    "an unbound accelerator fires nothing",
    !laufey::test_trigger_menu_accelerator(w, "CommandOrControl+Shift+F7"),
  );
  tokio::time::sleep(Duration::from_millis(200)).await;
  check(
    "no click for the disabled item",
    !clicks.lock().unwrap().iter().any(|c| c == "disabled-item"),
  );
  real_key_press_check(w, &clicks).await;
}

/// Windows: a real key press, injected with SendInput while the window has
/// the focus, reaches the menu item through TranslateAccelerator or
/// WebView2's AcceleratorKeyPressed (CEF: the window accelerator).
async fn real_key_press_check(w: &Window, clicks: &Arc<Mutex<Vec<String>>>) {
  #[cfg(windows)]
  {
    let mut hwnd = w.get_window_handle();
    if hwnd.is_null() {
      // Backends that don't hand out the HWND: find the window by its title.
      hwnd = win::find_window("native-e2e-menus");
      if hwnd.is_null() {
        // The page may have retitled it; this mode has one window.
        hwnd = win::find_window("");
      }
    }
    if !win::focus(hwnd) {
      na("a real accelerator key press (the window could not take the focus)");
      return;
    }
    tokio::time::sleep(Duration::from_millis(300)).await;
    let before = clicks.lock().unwrap().len();
    check(
      "SendInput injected Ctrl+Shift+F9",
      win::press_ctrl_shift_f9(),
    );
    check(
      "a real accelerator key press fires the menu item",
      wait_for(|| clicks.lock().unwrap().len() > before, 100, 20).await,
    );
  }
  #[cfg(not(windows))]
  {
    // X11: the window gets the focus (through the window manager the run
    // starts), then xdotool presses the keys (XTEST).
    if crate::os_view::xdotool().is_none() {
      let _ = (w, clicks);
      na(
        "a real accelerator key press (posting key events needs \
          Accessibility permission on macOS and xdotool on X11; the test \
          hook covers the dispatch elsewhere)",
      );
      return;
    }
    let _ = w;
    check(
      "the menu window takes the keyboard focus",
      crate::os_view::x_focus("native-e2e-menus").await,
    );
    tokio::time::sleep(Duration::from_millis(300)).await;
    let before = clicks.lock().unwrap().len();
    check(
      "xdotool injected Ctrl+Shift+F9",
      crate::os_view::xdo(&["key", "--clearmodifiers", "ctrl+shift+F9"])
        .is_some(),
    );
    check(
      "a real accelerator key press fires the menu item",
      wait_for(|| clicks.lock().unwrap().len() > before, 100, 20).await,
    );
    check(
      "the real press clicks the accelerated item",
      clicks.lock().unwrap()[before..]
        .iter()
        .all(|c| c == "accel-item"),
    );
  }
}

async fn context_menu_checks(w: &Window, caps: &laufey::MenuCapabilities) {
  if !caps.context_closed() {
    na("context-menu close callback (not supported by this backend)");
    return;
  }
  // An empty menu shows nothing and reports its close at once.
  let empty_closed = Arc::new(Mutex::new(0u32));
  {
    let c = empty_closed.clone();
    w.show_context_menu_with_close(
      10,
      10,
      &[],
      |_| {},
      move || {
        *c.lock().unwrap() += 1;
      },
    );
  }
  check(
    "an empty context menu reports its close",
    wait_for(|| *empty_closed.lock().unwrap() == 1, 100, 20).await,
  );

  let closed = Arc::new(Mutex::new(0u32));
  let clicked = Arc::new(Mutex::new(Vec::<String>::new()));
  {
    let (c, k) = (closed.clone(), clicked.clone());
    w.show_context_menu_with_close(
      20,
      20,
      &[
        MenuItem::Item {
          label: "Copy".into(),
          id: Some("ctx-copy".into()),
          accelerator: Some("CommandOrControl+C".into()),
          enabled: true,
          checked: false,
          icon: Some(super::TINY_PNG.to_vec()),
          tooltip: Some("tooltip".into()),
        },
        MenuItem::Separator,
        MenuItem::Item {
          label: "Checked".into(),
          id: Some("ctx-checked".into()),
          accelerator: None,
          enabled: true,
          checked: true,
          icon: None,
          tooltip: None,
        },
      ],
      move |id| k.lock().unwrap().push(id.to_string()),
      move || *c.lock().unwrap() += 1,
    );
  }
  tokio::time::sleep(Duration::from_millis(300)).await;
  screenshot_pause().await;
  // The menu opens on the UI thread; dismiss it once it is up. A menu the OS
  // refused to show (no interactive desktop) closes by itself.
  let dismissed = wait_for(
    || laufey::test_dismiss_context_menu() || *closed.lock().unwrap() > 0,
    150,
    20,
  )
  .await;
  check("the context menu opened (or closed by itself)", dismissed);
  check(
    "dismissing the context menu fires the close callback",
    wait_for(|| *closed.lock().unwrap() == 1, 150, 20).await,
  );
  tokio::time::sleep(Duration::from_millis(300)).await;
  check(
    "the close callback fires exactly once",
    *closed.lock().unwrap() == 1,
  );
  check(
    "a dismissed context menu reports no click",
    clicked.lock().unwrap().is_empty(),
  );
  check(
    "no context menu is open afterwards",
    !laufey::test_dismiss_context_menu(),
  );
}

/// Runs `f` on a thread of its own and waits up to `secs` for it: `None` if
/// it hasn't returned by then (the thread is left to finish later).
async fn returns_within<T: Send + 'static>(
  secs: u64,
  f: impl FnOnce() -> T + Send + 'static,
) -> Option<T> {
  let (tx, rx) = tokio::sync::oneshot::channel();
  std::thread::spawn(move || {
    let _ = tx.send(f());
  });
  tokio::time::timeout(Duration::from_secs(secs), rx)
    .await
    .ok()
    .and_then(|r| r.ok())
}

fn live_menu_items() -> Vec<MenuItem> {
  vec![
    MenuItem::Item {
      label: "First".into(),
      id: Some("live-first".into()),
      accelerator: None,
      enabled: true,
      checked: false,
      icon: None,
      tooltip: None,
    },
    MenuItem::Item {
      label: "Second".into(),
      id: Some("live-second".into()),
      accelerator: None,
      enabled: true,
      checked: false,
      icon: None,
      tooltip: None,
    },
  ]
}

/// The app keeps running while a context menu is open (the menu's modal
/// loop must not starve the backend's own task queue), and on Windows an
/// item can be chosen from the keyboard.
async fn context_menu_live_checks(caps: &laufey::MenuCapabilities) {
  if !caps.context_closed() {
    na("the app runs while a context menu is open (no close callback)");
    return;
  }
  let ticks = Arc::new(AtomicUsize::new(0));
  let w = {
    let t = ticks.clone();
    Window::new(480, 360)
      .title("native-e2e-menu-live")
      .bind("menuTick", move |call| {
        t.fetch_add(1, Ordering::SeqCst);
        call.resolve(laufey::Value::Bool(true));
      })
      .load("laufey-e2e://app/ticker")
  };
  w.show();
  let engine = laufey::scheme_handlers_supported();
  let page_ticks =
    engine && wait_for(|| ticks.load(Ordering::SeqCst) >= 3, 100, 100).await;
  if engine {
    check("the ticker page's timer reaches the runtime", page_ticks);
  }

  let closed = Arc::new(AtomicUsize::new(0));
  let clicked = Arc::new(Mutex::new(Vec::<String>::new()));
  let show_menu = |closed: &Arc<AtomicUsize>,
                   clicked: &Arc<Mutex<Vec<String>>>| {
    let (c, k) = (closed.clone(), clicked.clone());
    w.show_context_menu_with_close(
      40,
      40,
      &live_menu_items(),
      move |id| k.lock().unwrap().push(id.to_string()),
      move || {
        c.fetch_add(1, Ordering::SeqCst);
      },
    );
  };
  show_menu(&closed, &clicked);
  tokio::time::sleep(Duration::from_millis(500)).await;
  if closed.load(Ordering::SeqCst) > 0 {
    // No interactive desktop: the OS refused the menu.
    na("the app runs while a context menu is open (the menu didn't open)");
    return;
  }
  #[cfg(windows)]
  check(
    "the context menu is up (its popup window is visible)",
    win::popup_menu_open(),
  );

  // A page timer keeps reaching the runtime through the backend.
  if page_ticks {
    let before = ticks.load(Ordering::SeqCst);
    tokio::time::sleep(Duration::from_millis(1500)).await;
    let during = ticks.load(Ordering::SeqCst) - before;
    check(
      &format!(
        "a page timer keeps reaching the runtime while a context menu is \
         open ({during} ticks in 1.5 s)"
      ),
      during >= 3,
    );
  }
  // Synchronous calls that hop to the UI thread return.
  let id = w.id();
  let size = returns_within(5, move || Window::from_id(id).get_size()).await;
  check(
    &format!(
      "a synchronous window call returns while a context menu is open \
       ({size:?})"
    ),
    size.is_some(),
  );
  let task = returns_within(5, || laufey::try_run_on_ui_thread(|| 7)).await;
  match task {
    Some(Err(e)) => na(&format!(
      "run_on_ui_thread while a context menu is open ({e})"
    )),
    _ => check(
      &format!(
        "run_on_ui_thread returns while a context menu is open ({task:?})"
      ),
      matches!(task, Some(Ok(7))),
    ),
  }
  let still_open = closed.load(Ordering::SeqCst) == 0;
  check("the context menu stayed open during the checks", still_open);
  let dismissed = wait_for(
    || laufey::test_dismiss_context_menu() || closed.load(Ordering::SeqCst) > 0,
    150,
    20,
  )
  .await;
  check(
    "the context menu closes afterwards (exactly once)",
    dismissed
      && wait_for(|| closed.load(Ordering::SeqCst) == 1, 150, 20).await
      && clicked.lock().unwrap().is_empty(),
  );

  keyboard_choice_check(&w, show_menu).await;
  #[cfg(windows)]
  tray_menu_live_checks(w.id(), &ticks, page_ticks).await;
}

/// Windows WebView2 / CEF: the app keeps running while the TRAY icon's menu
/// is open too. tray_win.cc runs that menu's TrackPopupMenu modal loop on the
/// UI thread like the context menu's. The right-click is posted to the
/// tray's hidden window exactly as Shell_NotifyIcon delivers it (only the
/// OS-side click is synthesized), and WM_CANCELMODE to that window, the
/// menu's owner, closes the menu without a choice.
#[cfg(windows)]
async fn tray_menu_live_checks(
  window_id: u32,
  ticks: &Arc<AtomicUsize>,
  page_ticks: bool,
) {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  if backend != "webview" && backend != "cef" {
    na("the app runs while a tray menu is open (WebView2 / CEF tray only)");
    return;
  }
  let clicked = Arc::new(Mutex::new(Vec::<String>::new()));
  let tray = {
    let k = clicked.clone();
    laufey::TrayIcon::new()
      .icon(super::TINY_PNG)
      .menu(&live_menu_items(), move |id| {
        k.lock().unwrap().push(id.to_string())
      })
  };
  if tray.id() == 0 {
    check("tray created for the tray-menu check", false);
    return;
  }
  // CEF creates the window, and sets the menu, on its UI thread after the
  // calls return.
  let _ = wait_for(|| !win::tray_window().is_null(), 100, 50).await;
  let hwnd = win::tray_window();
  check("the tray window exists", !hwnd.is_null());
  if hwnd.is_null() {
    return;
  }
  // A right-click that lands before the menu is set opens nothing: re-post
  // until the menu is up.
  let mut up = false;
  for _ in 0..10 {
    win::post(
      hwnd,
      win::WM_TRAYICON,
      tray.id() as usize,
      win::WM_RBUTTONUP,
    );
    if wait_for(win::popup_menu_open, 20, 50).await {
      up = true;
      break;
    }
  }
  check("the tray menu is up (its popup window is visible)", up);
  if !up {
    return;
  }

  if page_ticks {
    let before = ticks.load(Ordering::SeqCst);
    tokio::time::sleep(Duration::from_millis(1500)).await;
    let during = ticks.load(Ordering::SeqCst) - before;
    check(
      &format!(
        "a page timer keeps reaching the runtime while a tray menu is open \
         ({during} ticks in 1.5 s)"
      ),
      during >= 3,
    );
  }
  let size =
    returns_within(5, move || Window::from_id(window_id).get_size()).await;
  check(
    &format!(
      "a synchronous window call returns while a tray menu is open ({size:?})"
    ),
    size.is_some(),
  );
  let task = returns_within(5, || laufey::try_run_on_ui_thread(|| 7)).await;
  match task {
    Some(Err(e)) => {
      na(&format!("run_on_ui_thread while a tray menu is open ({e})"))
    }
    _ => check(
      &format!("run_on_ui_thread returns while a tray menu is open ({task:?})"),
      matches!(task, Some(Ok(7))),
    ),
  }
  check(
    "the tray menu stayed open during the checks",
    win::popup_menu_open(),
  );

  // Close it. A right-click still queued from the loop above would open the
  // menu again once this one closes, so cancel until none is up.
  let mut closed = false;
  for _ in 0..5 {
    win::post(hwnd, win::WM_CANCELMODE, 0, 0);
    if wait_for(|| !win::popup_menu_open(), 40, 50).await {
      tokio::time::sleep(Duration::from_millis(300)).await;
      if !win::popup_menu_open() {
        closed = true;
        break;
      }
    }
  }
  check("WM_CANCELMODE closes the tray menu", closed);
  check(
    "a cancelled tray menu reports no click",
    clicked.lock().unwrap().is_empty(),
  );
}

/// Windows: a context menu is driven from the keyboard, injected with
/// SendInput, the way a person does it: Escape dismisses it, then Down +
/// Return chooses the first item of the next one. Before each menu the test
/// brings the window to the front the way an automation script (or a
/// person) does: an Alt press around SetForegroundWindow, the usual way to
/// be allowed to take the foreground, which leaves the window in menu mode
/// (a lone Alt release selects the system menu).
async fn keyboard_choice_check<F>(w: &Window, show_menu: F)
where
  F: Fn(&Arc<AtomicUsize>, &Arc<Mutex<Vec<String>>>),
{
  #[cfg(windows)]
  {
    let mut hwnd = w.get_window_handle();
    if hwnd.is_null() {
      hwnd = win::find_window("native-e2e-menu-live");
    }
    if hwnd.is_null() {
      na("a context menu driven from the keyboard (no window handle)");
      return;
    }
    for (keys, label, want) in [
      (
        &[win::VK_ESCAPE][..],
        "Escape dismisses the context menu",
        None,
      ),
      (
        &[win::VK_DOWN, win::VK_RETURN][..],
        "Down + Return chooses the first context-menu item",
        Some("live-first"),
      ),
    ] {
      w.focus();
      win::activate_with_alt(hwnd);
      tokio::time::sleep(Duration::from_millis(500)).await;
      let closed = Arc::new(AtomicUsize::new(0));
      let clicked = Arc::new(Mutex::new(Vec::<String>::new()));
      show_menu(&closed, &clicked);
      let up = wait_for(win::popup_menu_open, 50, 50).await;
      tokio::time::sleep(Duration::from_millis(500)).await;
      let early = closed.load(Ordering::SeqCst);
      check(
        &format!("{label}: the menu is up (popup {up}, closed early {early})"),
        up && early == 0,
      );
      if up && early == 0 && !win::is_foreground(hwnd) {
        // Some runners never let this process take the foreground (the
        // accelerator check above is N/A there too), and keys then go to
        // whatever window has it.
        na(&format!(
          "{label} (the window could not take the foreground on this desktop)"
        ));
        wait_for(
          || {
            laufey::test_dismiss_context_menu()
              || closed.load(Ordering::SeqCst) > 0
          },
          150,
          20,
        )
        .await;
        continue;
      }
      for &k in keys {
        check("SendInput injected the key", win::press(k));
        tokio::time::sleep(Duration::from_millis(300)).await;
      }
      let done = wait_for(|| closed.load(Ordering::SeqCst) > 0, 100, 30).await;
      let got = clicked.lock().unwrap().clone();
      check(
        &format!("{label} (closed {done}, clicks {got:?})"),
        done
          && match want {
            Some(id) => got.iter().any(|c| c == id),
            None => got.is_empty(),
          },
      );
      if !done {
        // Leave no menu behind for the checks that follow.
        wait_for(
          || {
            laufey::test_dismiss_context_menu()
              || closed.load(Ordering::SeqCst) > 0
          },
          150,
          20,
        )
        .await;
      }
      check(
        &format!("{label}: the menu reports its close once"),
        wait_for(|| closed.load(Ordering::SeqCst) == 1, 150, 20).await,
      );
      tokio::time::sleep(Duration::from_millis(300)).await;
    }
  }
  #[cfg(not(windows))]
  {
    let _ = (w, show_menu);
    na(
      "a context menu driven from the keyboard (OS key injection is \
       Windows-only here)",
    );
  }
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

async fn permission(kind: PermissionKind) -> PermissionStatus {
  let (tx, rx) = tokio::sync::oneshot::channel();
  laufey::request_permission(kind, move |s| {
    let _ = tx.send(s);
  });
  rx.await.unwrap_or(PermissionStatus::Unsupported)
}

async fn notification_checks(caps: &laufey::NotificationCapabilities) {
  // Responses no live callback owns: buffered until a handler registers
  // (the cold-start click), then delivered directly.
  let responses: Arc<Mutex<Vec<NotificationResponse>>> =
    Arc::new(Mutex::new(Vec::new()));
  if std::env::var("LAUFEY_E2E_BACKEND").as_deref() == Ok("winit") {
    na("notifications (the Winit backend has no API 41 notifications)");
    check(
      "the response hook reports nothing on Winit",
      !laufey::test_notification_respond("e2e-cold", None),
    );
    return;
  }
  let have_hook = laufey::test_notification_respond("e2e-cold", Some("open"));
  check(
    "a response before any handler is buffered (not delivered)",
    !have_hook,
  );
  {
    let r = responses.clone();
    laufey::set_notification_response_handler(move |resp| {
      r.lock().unwrap().push(resp);
    });
  }
  check(
    "the buffered response is delivered when the handler registers",
    wait_for(|| !responses.lock().unwrap().is_empty(), 50, 20).await,
  );
  {
    let list = responses.lock().unwrap();
    let first = list.first();
    check(
      "the buffered response carries tag, action and launch: true",
      first.is_some_and(|r| {
        r.tag == "e2e-cold" && r.action.as_deref() == Some("open") && r.launch
      }),
    );
  }
  check(
    "a later response reaches the handler directly",
    laufey::test_notification_respond("e2e-warm", None),
  );
  check(
    "it carries launch: false and no action",
    wait_for(
      || {
        responses
          .lock()
          .unwrap()
          .iter()
          .any(|r| r.tag == "e2e-warm" && r.action.is_none() && !r.launch)
      },
      50,
      20,
    )
    .await,
  );

  // Permission: macOS grants quiet ("provisional") notifications without a
  // prompt, which is what lets CI post them.
  let status = permission(PermissionKind::NotificationsProvisional).await;
  eprintln!("[e2e] notification permission (provisional request) = {status:?}");
  let can_post = caps.show() && status == PermissionStatus::Granted;
  if !can_post {
    na(&format!(
      "posting notifications (capabilities {:#x}, permission {status:?})",
      caps.bits
    ));
  } else {
    live_notification_checks().await;
  }

  #[cfg(windows)]
  if caps.cold_start()
    && !std::env::var("LAUFEY_E2E_MN_PARTS").is_ok_and(|p| p.contains("nocom"))
  {
    com_activation_check(&responses).await;
  }

  if can_post && caps.schedule() {
    schedule_checks().await;
  } else {
    na("scheduled notifications (no scheduling or permission here)");
  }
  laufey::clear_notification_response_handler();
}

async fn live_notification_checks() {
  let events: Arc<Mutex<Vec<NotificationEvent>>> =
    Arc::new(Mutex::new(Vec::new()));
  let body = if cfg!(target_os = "linux") {
    // The CI mock server answers this with a real ActionInvoked.
    "[[invoke:reply]]"
  } else {
    "native e2e"
  };
  let handle = {
    let e = events.clone();
    Notification::new("laufey e2e")
      .body(body)
      .tag("e2e-live")
      .data("{\"n\":1}")
      .silent(true)
      .action("reply", "Reply")
      .action("later", "Later")
      .on_event(move |ev| e.lock().unwrap().push(ev))
  };
  check("show_notification returned an id", handle.id() != 0);
  if handle.id() == 0 {
    return;
  }
  let saw = |events: &Arc<Mutex<Vec<NotificationEvent>>>,
             f: &dyn Fn(&NotificationEvent) -> bool| {
    events.lock().unwrap().iter().any(f)
  };
  if cfg!(target_os = "linux")
    && std::env::var_os("LAUFEY_E2E_NOTIFY_MOCK").is_some()
  {
    check(
      "a real ActionInvoked from the notification server reaches the callback",
      wait_for(
        || {
          saw(
            &events,
            &|e| matches!(e, NotificationEvent::Action(a) if a == "reply"),
          )
        },
        150,
        20,
      )
      .await,
    );
    check(
      "the server reported the notification shown",
      saw(&events, &|e| matches!(e, NotificationEvent::Shown)),
    );
  }
  check(
    "an action click reaches the live callback",
    laufey::test_notification_respond("e2e-live", Some("later")),
  );
  check(
    "the callback gets Action(\"later\")",
    wait_for(
      || {
        saw(
          &events,
          &|e| matches!(e, NotificationEvent::Action(a) if a == "later"),
        )
      },
      50,
      20,
    )
    .await,
  );
  check(
    "a body click reaches the live callback",
    laufey::test_notification_respond("e2e-live", None),
  );
  check(
    "the callback gets Clicked",
    wait_for(
      || saw(&events, &|e| matches!(e, NotificationEvent::Clicked)),
      50,
      20,
    )
    .await,
  );
  handle.close();
  check(
    "close reports Closed",
    wait_for(
      || saw(&events, &|e| matches!(e, NotificationEvent::Closed)),
      100,
      20,
    )
    .await,
  );
}

async fn list_tags() -> Vec<laufey::ScheduledNotification> {
  let (tx, rx) = tokio::sync::oneshot::channel();
  laufey::list_scheduled_notifications(move |list| {
    let _ = tx.send(list);
  });
  rx.await.unwrap_or_default()
}

async fn schedule_checks() {
  let at = SystemTime::now() + Duration::from_secs(120);
  let at_ms = at
    .duration_since(SystemTime::UNIX_EPOCH)
    .unwrap()
    .as_millis() as i64;
  let handle = Notification::new("laufey e2e (scheduled)")
    .body("in two minutes")
    .tag("e2e-sched")
    .data("sched-data")
    .silent(true)
    .action("snooze", "Snooze")
    .schedule_at(at)
    .show();
  check("a scheduled notification gets an id", handle.id() != 0);
  let mut listed = None;
  for _ in 0..50 {
    listed = list_tags().await.into_iter().find(|n| n.tag == "e2e-sched");
    if listed.is_some() {
      break;
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
  }
  check("the scheduled notification is listed", listed.is_some());
  if let Some(n) = &listed {
    check(
      &format!("its time is listed (got {}, want {at_ms})", n.at_ms),
      (n.at_ms - at_ms).abs() <= 2000,
    );
    check("its title is listed", n.title == "laufey e2e (scheduled)");
    check(
      "its data is listed",
      n.data.as_deref() == Some("sched-data"),
    );
    check(
      "its actions are listed",
      n.actions.iter().any(|a| a.id == "snooze"),
    );
  }
  laufey::cancel_notification("e2e-sched");
  let mut gone = false;
  for _ in 0..50 {
    if !list_tags().await.iter().any(|n| n.tag == "e2e-sched") {
      gone = true;
      break;
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
  }
  check("a cancelled notification leaves the list", gone);

  // Two seconds out: delivered, so it leaves the list by itself.
  let soon = SystemTime::now() + Duration::from_secs(2);
  let handle = Notification::new("laufey e2e (soon)")
    .tag("e2e-soon")
    .silent(true)
    .schedule_at(soon)
    .show();
  check("a near notification gets an id", handle.id() != 0);
  let mut delivered = false;
  for _ in 0..100 {
    tokio::time::sleep(Duration::from_millis(150)).await;
    if SystemTime::now() > soon
      && !list_tags().await.iter().any(|n| n.tag == "e2e-soon")
    {
      delivered = true;
      break;
    }
  }
  check(
    "a scheduled notification is delivered at its time",
    delivered,
  );
  laufey::cancel_notification("e2e-soon");
}

/// What Windows does for a click on a toast: CoCreateInstance on the app's
/// registered activator CLSID, then INotificationActivationCallback::
/// Activate with the toast's arguments. Any process of the user can do that,
/// so laufey takes only arguments MAC'd with the install's key
/// (docs/notifications.md, "Click authenticity"): a forged Activate is
/// dropped, and the genuine one is signed here as laufey signs its toasts.
#[cfg(windows)]
async fn com_activation_check(
  responses: &Arc<Mutex<Vec<NotificationResponse>>>,
) {
  let forged = "laufey=1&tag=e2e-forged&action=yes&data=%7B%7D";
  if let Err(e) = win::activate_toast(forged) {
    check(&format!("the COM activator is reachable ({e})"), false);
    return;
  }
  check(
    "a forged Activate (no MAC) never reaches the response handler",
    !wait_for(
      || {
        responses
          .lock()
          .unwrap()
          .iter()
          .any(|r| r.tag == "e2e-forged")
      },
      20,
      100,
    )
    .await,
  );
  let args = match click_key() {
    Ok(key) => sign_click("laufey=1&tag=e2e-com&action=yes&data=%7B%7D", &key),
    Err(e) => {
      check(&format!("the install's click key is readable ({e})"), false);
      return;
    }
  };
  match win::activate_toast(&args) {
    Ok(()) => {
      check("the COM activator accepted Activate", true);
      check(
        "the activation reaches the response handler",
        wait_for(
          || {
            responses.lock().unwrap().iter().any(|r| {
              r.tag == "e2e-com"
                && r.action.as_deref() == Some("yes")
                && r.data.as_deref() == Some("{}")
            })
          },
          100,
          30,
        )
        .await,
      );
    }
    Err(e) => check(&format!("the COM activator is reachable ({e})"), false),
  }
}

/// The install's notification click key (`laufey-notification-key` in the
/// app data directory: LAUFEY_DATA_DIR, else %LOCALAPPDATA%\<app id>).
#[cfg(windows)]
fn click_key() -> Result<Vec<u8>, String> {
  let dir = match std::env::var("LAUFEY_DATA_DIR") {
    Ok(d) if !d.is_empty() => std::path::PathBuf::from(d),
    _ => {
      let base = std::env::var("LOCALAPPDATA")
        .map_err(|_| "LOCALAPPDATA not set".to_string())?;
      let app = std::env::var("LAUFEY_APP_ID")
        .map_err(|_| "LAUFEY_APP_ID not set".to_string())?;
      std::path::Path::new(&base).join(app)
    }
  };
  let path = dir.join("laufey-notification-key");
  let text = std::fs::read_to_string(&path)
    .map_err(|e| format!("{}: {e}", path.display()))?;
  let hex = text.trim();
  if hex.len() != 64 {
    return Err(format!("{} is not 32 bytes of hex", path.display()));
  }
  (0..32)
    .map(|i| {
      u8::from_str_radix(&hex[2 * i..2 * i + 2], 16)
        .map_err(|e| format!("{}: {e}", path.display()))
    })
    .collect()
}

/// `args` + "&mac=" + the hex HMAC-SHA256 laufey verifies
/// (notification_auth.cc).
#[cfg(windows)]
fn sign_click(args: &str, key: &[u8]) -> String {
  let message = format!("laufey-notification-click/1\n{args}");
  let mac = hmac_sha256(key, message.as_bytes());
  let hex: String = mac.iter().map(|b| format!("{b:02x}")).collect();
  format!("{args}&mac={hex}")
}

#[cfg(windows)]
fn hmac_sha256(key: &[u8], message: &[u8]) -> [u8; 32] {
  let mut k = [0u8; 64];
  if key.len() > 64 {
    k[..32].copy_from_slice(&sha256(key));
  } else {
    k[..key.len()].copy_from_slice(key);
  }
  let mut inner: Vec<u8> = k.iter().map(|b| b ^ 0x36).collect();
  inner.extend_from_slice(message);
  let mut outer: Vec<u8> = k.iter().map(|b| b ^ 0x5c).collect();
  outer.extend_from_slice(&sha256(&inner));
  sha256(&outer)
}

/// SHA-256 (FIPS 180-4).
#[cfg(windows)]
fn sha256(data: &[u8]) -> [u8; 32] {
  const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ];
  let mut h: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
    0x1f83d9ab, 0x5be0cd19,
  ];
  let mut msg = data.to_vec();
  msg.push(0x80);
  while msg.len() % 64 != 56 {
    msg.push(0);
  }
  msg.extend_from_slice(&((data.len() as u64) * 8).to_be_bytes());
  for block in msg.chunks(64) {
    let mut w = [0u32; 64];
    for i in 0..16 {
      w[i] = u32::from_be_bytes([
        block[4 * i],
        block[4 * i + 1],
        block[4 * i + 2],
        block[4 * i + 3],
      ]);
    }
    for i in 16..64 {
      let s0 = w[i - 15].rotate_right(7)
        ^ w[i - 15].rotate_right(18)
        ^ (w[i - 15] >> 3);
      let s1 = w[i - 2].rotate_right(17)
        ^ w[i - 2].rotate_right(19)
        ^ (w[i - 2] >> 10);
      w[i] = w[i - 16]
        .wrapping_add(s0)
        .wrapping_add(w[i - 7])
        .wrapping_add(s1);
    }
    let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = h;
    for i in 0..64 {
      let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
      let ch = (e & f) ^ (!e & g);
      let t1 = hh
        .wrapping_add(s1)
        .wrapping_add(ch)
        .wrapping_add(K[i])
        .wrapping_add(w[i]);
      let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
      let maj = (a & b) ^ (a & c) ^ (b & c);
      let t2 = s0.wrapping_add(maj);
      hh = g;
      g = f;
      f = e;
      e = d.wrapping_add(t1);
      d = c;
      c = b;
      b = a;
      a = t1.wrapping_add(t2);
    }
    for (x, y) in h.iter_mut().zip([a, b, c, d, e, f, g, hh]) {
      *x = x.wrapping_add(y);
    }
  }
  let mut out = [0u8; 32];
  for (i, word) in h.iter().enumerate() {
    out[4 * i..4 * i + 4].copy_from_slice(&word.to_be_bytes());
  }
  out
}

#[cfg(windows)]
pub(crate) mod win {
  use std::ffi::c_void;

  #[repr(C)]
  #[derive(Default)]
  struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
  }

  #[repr(C)]
  #[derive(Default)]
  struct Point {
    x: i32,
    y: i32,
  }

  #[repr(C)]
  #[derive(Clone, Copy)]
  struct KeybdInput {
    vk: u16,
    scan: u16,
    flags: u32,
    time: u32,
    extra: usize,
  }

  #[repr(C)]
  #[derive(Clone, Copy)]
  struct Input {
    kind: u32,
    ki: KeybdInput,
    _pad: [u8; 8],
  }

  #[repr(C)]
  #[derive(Clone, Copy)]
  struct Guid {
    data1: u32,
    data2: u16,
    data3: u16,
    data4: [u8; 8],
  }

  #[link(name = "user32")]
  extern "system" {
    fn SendInput(count: u32, inputs: *const Input, size: i32) -> u32;
    fn SetForegroundWindow(hwnd: *mut c_void) -> i32;
    fn GetForegroundWindow() -> *mut c_void;
    fn GetAncestor(hwnd: *mut c_void, flags: u32) -> *mut c_void;
    fn GetWindowThreadProcessId(hwnd: *mut c_void, pid: *mut u32) -> u32;
    fn AttachThreadInput(from: u32, to: u32, attach: i32) -> i32;
    fn BringWindowToTop(hwnd: *mut c_void) -> i32;
    fn ShowWindow(hwnd: *mut c_void, cmd: i32) -> i32;
    fn EnumWindows(
      cb: unsafe extern "system" fn(*mut c_void, isize) -> i32,
      lparam: isize,
    ) -> i32;
    fn IsWindowVisible(hwnd: *mut c_void) -> i32;
    fn GetWindowTextW(hwnd: *mut c_void, buf: *mut u16, len: i32) -> i32;
    fn GetClientRect(hwnd: *mut c_void, rect: *mut Rect) -> i32;
    fn ClientToScreen(hwnd: *mut c_void, point: *mut Point) -> i32;
    fn GetClassNameW(hwnd: *mut c_void, buf: *mut u16, len: i32) -> i32;
    fn PostMessageW(hwnd: *mut c_void, msg: u32, wp: usize, lp: isize) -> i32;
  }

  #[link(name = "ole32")]
  extern "system" {
    fn CoInitializeEx(reserved: *mut c_void, coinit: u32) -> i32;
    fn CoUninitialize();
    fn CLSIDFromString(s: *const u16, clsid: *mut Guid) -> i32;
    fn CoCreateInstance(
      clsid: *const Guid,
      outer: *mut c_void,
      ctx: u32,
      iid: *const Guid,
      out: *mut *mut c_void,
    ) -> i32;
  }

  #[link(name = "advapi32")]
  extern "system" {
    fn RegGetValueW(
      hkey: isize,
      subkey: *const u16,
      value: *const u16,
      flags: u32,
      kind: *mut u32,
      data: *mut c_void,
      len: *mut u32,
    ) -> i32;
  }

  const HKEY_CURRENT_USER: isize = 0x8000_0001u32 as i32 as isize;
  const RRF_RT_REG_SZ: u32 = 0x2;
  const CLSCTX_LOCAL_SERVER: u32 = 0x4;
  const COINIT_APARTMENTTHREADED: u32 = 2;
  // {53E31837-6600-4A81-9395-75CFFE746F94}
  const IID_INOTIFICATION_ACTIVATION_CALLBACK: Guid = Guid {
    data1: 0x53E31837,
    data2: 0x6600,
    data3: 0x4A81,
    data4: [0x93, 0x95, 0x75, 0xCF, 0xFE, 0x74, 0x6F, 0x94],
  };

  fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
  }

  fn key(vk: u16, up: bool) -> Input {
    Input {
      kind: 1, // INPUT_KEYBOARD
      ki: KeybdInput {
        vk,
        scan: 0,
        flags: if up { 2 } else { 0 }, // KEYEVENTF_KEYUP
        time: 0,
        extra: 0,
      },
      _pad: [0; 8],
    }
  }

  /// A visible top-level window of this process whose title starts with
  /// `title` (the backends that don't hand out the HWND).
  pub fn find_window(title: &str) -> *mut c_void {
    struct Search {
      pid: u32,
      title: Vec<u16>,
      found: *mut c_void,
    }
    unsafe extern "system" fn visit(hwnd: *mut c_void, lparam: isize) -> i32 {
      let search = &mut *(lparam as *mut Search);
      let mut pid = 0u32;
      GetWindowThreadProcessId(hwnd, &mut pid);
      if pid != search.pid || IsWindowVisible(hwnd) == 0 {
        return 1;
      }
      let mut buf = [0u16; 256];
      let n = GetWindowTextW(hwnd, buf.as_mut_ptr(), buf.len() as i32);
      if n >= 0 && buf[..n as usize].starts_with(&search.title) {
        search.found = hwnd;
        return 0;
      }
      1
    }
    let mut search = Search {
      pid: std::process::id(),
      title: title.encode_utf16().collect(),
      found: std::ptr::null_mut(),
    };
    unsafe { EnumWindows(visit, &mut search as *mut Search as isize) };
    search.found
  }

  /// The client area of the window titled `title` in screen pixels (this
  /// process is per-monitor DPI aware, so physical): (x, y, width, height).
  pub fn client_rect(title: &str) -> Option<(i32, i32, i32, i32)> {
    let hwnd = find_window(title);
    if hwnd.is_null() {
      return None;
    }
    let mut rect = Rect::default();
    let mut origin = Point::default();
    unsafe {
      if GetClientRect(hwnd, &mut rect) == 0
        || ClientToScreen(hwnd, &mut origin) == 0
      {
        return None;
      }
    }
    Some((
      origin.x,
      origin.y,
      rect.right - rect.left,
      rect.bottom - rect.top,
    ))
  }

  pub fn focus(hwnd: *mut c_void) -> bool {
    if hwnd.is_null() {
      return false;
    }
    unsafe {
      let root = GetAncestor(hwnd, 2); // GA_ROOT
      let target = if root.is_null() { hwnd } else { root };
      // The foreground lock lets only the foreground thread hand the focus
      // over: share its input state for the call.
      let fg = GetForegroundWindow();
      let fg_thread = if fg.is_null() {
        0
      } else {
        GetWindowThreadProcessId(fg, std::ptr::null_mut())
      };
      let our_thread = GetWindowThreadProcessId(target, std::ptr::null_mut());
      let attached = fg_thread != 0
        && fg_thread != our_thread
        && AttachThreadInput(fg_thread, our_thread, 1) != 0;
      BringWindowToTop(target);
      SetForegroundWindow(target);
      if attached {
        AttachThreadInput(fg_thread, our_thread, 0);
      }
      for attempt in 0..40 {
        if GetForegroundWindow() == target {
          return true;
        }
        if attempt == 10 {
          // A restored window is activated: minimize and restore it.
          ShowWindow(target, 6); // SW_MINIMIZE
          ShowWindow(target, 9); // SW_RESTORE
        }
        std::thread::sleep(std::time::Duration::from_millis(50));
      }
      eprintln!(
        "[e2e] focus: foreground is {:?}, wanted {target:?}",
        GetForegroundWindow()
      );
      false
    }
  }

  /// Whether `hwnd`'s top-level window is the foreground window.
  pub fn is_foreground(hwnd: *mut c_void) -> bool {
    unsafe {
      let root = GetAncestor(hwnd, 2); // GA_ROOT
      let target = if root.is_null() { hwnd } else { root };
      !target.is_null() && GetForegroundWindow() == target
    }
  }

  /// Whether a popup menu window ("#32768") of this process is visible.
  pub fn popup_menu_open() -> bool {
    struct Search {
      pid: u32,
      found: bool,
    }
    unsafe extern "system" fn visit(hwnd: *mut c_void, lparam: isize) -> i32 {
      let search = &mut *(lparam as *mut Search);
      let mut pid = 0u32;
      GetWindowThreadProcessId(hwnd, &mut pid);
      if pid != search.pid || IsWindowVisible(hwnd) == 0 {
        return 1;
      }
      let mut buf = [0u16; 16];
      let n = GetClassNameW(hwnd, buf.as_mut_ptr(), buf.len() as i32);
      let class: Vec<u16> = "#32768".encode_utf16().collect();
      if n > 0 && buf[..n as usize] == class[..] {
        search.found = true;
        return 0;
      }
      1
    }
    let mut search = Search {
      pid: std::process::id(),
      found: false,
    };
    unsafe { EnumWindows(visit, &mut search as *mut Search as isize) };
    search.found
  }

  /// This process's tray window (tray_win.cc): a hidden top-level window of
  /// class "LaufeyCommonTrayWindow", never shown; null if there is none.
  pub fn tray_window() -> *mut c_void {
    struct Search {
      pid: u32,
      found: *mut c_void,
    }
    unsafe extern "system" fn visit(hwnd: *mut c_void, lparam: isize) -> i32 {
      let search = &mut *(lparam as *mut Search);
      let mut pid = 0u32;
      GetWindowThreadProcessId(hwnd, &mut pid);
      if pid != search.pid {
        return 1;
      }
      let mut buf = [0u16; 64];
      let n = GetClassNameW(hwnd, buf.as_mut_ptr(), buf.len() as i32);
      let class: Vec<u16> = "LaufeyCommonTrayWindow".encode_utf16().collect();
      if n > 0 && buf[..n as usize] == class[..] {
        search.found = hwnd;
        return 0;
      }
      1
    }
    let mut search = Search {
      pid: std::process::id(),
      found: std::ptr::null_mut(),
    };
    unsafe { EnumWindows(visit, &mut search as *mut Search as isize) };
    search.found
  }

  /// WM_LAUFEY_COMMON_TRAYICON (tray_win.cc): Shell_NotifyIcon's callback
  /// message, wParam = the tray id, LOWORD(lParam) = the mouse message.
  pub const WM_TRAYICON: u32 = 0x8000 + 65; // WM_APP + 65
  pub const WM_LBUTTONUP: isize = 0x0202;
  pub const WM_RBUTTONUP: isize = 0x0205;
  pub const WM_CANCELMODE: u32 = 0x001F;

  pub fn post(hwnd: *mut c_void, msg: u32, wp: usize, lp: isize) -> bool {
    unsafe { PostMessageW(hwnd, msg, wp, lp) != 0 }
  }

  pub const VK_ESCAPE: u16 = 0x1B;
  pub const VK_DOWN: u16 = 0x28;
  pub const VK_RETURN: u16 = 0x0D;
  const VK_MENU: u16 = 0x12;

  /// Press and release one key.
  pub fn press(vk: u16) -> bool {
    let seq = [key(vk, false), key(vk, true)];
    let sent = unsafe {
      SendInput(
        seq.len() as u32,
        seq.as_ptr(),
        std::mem::size_of::<Input>() as i32,
      )
    };
    sent as usize == seq.len()
  }

  /// Bring `hwnd`'s window to the front with an Alt press around
  /// SetForegroundWindow: the input lets this process take the foreground.
  pub fn activate_with_alt(hwnd: *mut c_void) {
    let root = unsafe { GetAncestor(hwnd, 2) }; // GA_ROOT
    let target = if root.is_null() { hwnd } else { root };
    let down = [key(VK_MENU, false)];
    let up = [key(VK_MENU, true)];
    unsafe {
      SendInput(1, down.as_ptr(), std::mem::size_of::<Input>() as i32);
      SetForegroundWindow(target);
      SendInput(1, up.as_ptr(), std::mem::size_of::<Input>() as i32);
    }
  }

  pub fn press_ctrl_shift_f9() -> bool {
    const CTRL: u16 = 0x11;
    const SHIFT: u16 = 0x10;
    const F9: u16 = 0x78;
    let seq = [
      key(CTRL, false),
      key(SHIFT, false),
      key(F9, false),
      key(F9, true),
      key(SHIFT, true),
      key(CTRL, true),
    ];
    let sent = unsafe {
      SendInput(
        seq.len() as u32,
        seq.as_ptr(),
        std::mem::size_of::<Input>() as i32,
      )
    };
    sent as usize == seq.len()
  }

  /// The AUMID registration laufey wrote, then the activator's Activate, on
  /// a thread of its own whose apartment is gone afterwards (a lingering
  /// MTA in the process would change how other DLLs exit).
  pub fn activate_toast(args: &str) -> Result<(), String> {
    let args = args.to_string();
    std::thread::spawn(move || {
      unsafe { CoInitializeEx(std::ptr::null_mut(), COINIT_APARTMENTTHREADED) };
      let result = activate_toast_on_this_thread(&args);
      unsafe { CoUninitialize() };
      result
    })
    .join()
    .unwrap_or_else(|_| Err("the activation thread panicked".into()))
  }

  fn activate_toast_on_this_thread(args: &str) -> Result<(), String> {
    let app = std::env::var("LAUFEY_APP_ID")
      .map_err(|_| "LAUFEY_APP_ID not set".to_string())?;
    let key = wide(&format!("Software\\Classes\\AppUserModelId\\{app}"));
    let name = wide("CustomActivator");
    let mut buf = [0u16; 64];
    let mut len = (buf.len() * 2) as u32;
    let rc = unsafe {
      RegGetValueW(
        HKEY_CURRENT_USER,
        key.as_ptr(),
        name.as_ptr(),
        RRF_RT_REG_SZ,
        std::ptr::null_mut(),
        buf.as_mut_ptr() as *mut c_void,
        &mut len,
      )
    };
    if rc != 0 {
      return Err(format!("no CustomActivator registered ({rc})"));
    }
    unsafe {
      let mut clsid: Guid = std::mem::zeroed();
      if CLSIDFromString(buf.as_ptr(), &mut clsid) < 0 {
        return Err("bad CLSID".into());
      }
      let mut obj: *mut c_void = std::ptr::null_mut();
      let hr = CoCreateInstance(
        &clsid,
        std::ptr::null_mut(),
        CLSCTX_LOCAL_SERVER,
        &IID_INOTIFICATION_ACTIVATION_CALLBACK,
        &mut obj,
      );
      if hr < 0 || obj.is_null() {
        return Err(format!("CoCreateInstance failed ({hr:#x})"));
      }
      // vtable: QueryInterface, AddRef, Release, Activate.
      type Activate = unsafe extern "system" fn(
        *mut c_void,
        *const u16,
        *const u16,
        *const c_void,
        u32,
      ) -> i32;
      type Release = unsafe extern "system" fn(*mut c_void) -> u32;
      let vtbl = *(obj as *const *const usize);
      let activate: Activate = std::mem::transmute(*vtbl.add(3));
      let release: Release = std::mem::transmute(*vtbl.add(2));
      let aumid = wide(&app);
      let wargs = wide(args);
      let hr =
        activate(obj, aumid.as_ptr(), wargs.as_ptr(), std::ptr::null(), 0);
      release(obj);
      if hr < 0 {
        return Err(format!("Activate failed ({hr:#x})"));
      }
    }
    Ok(())
  }
}
