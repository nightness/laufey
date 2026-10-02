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
/// ("tag\naction\ndata\nlaunch") for the script that clicked. Ends the
/// process.
pub async fn cold_start() -> ! {
  let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();
  laufey::set_notification_response_handler(move |r| {
    let _ = tx.send(r);
  });
  let out = std::env::temp_dir().join("laufey-coldstart-result.txt");
  let got = tokio::time::timeout(Duration::from_secs(20), rx.recv()).await;
  let text = match got {
    Ok(Some(r)) => format!(
      "{}\n{}\n{}\n{}\n",
      r.tag,
      r.action.unwrap_or_default(),
      r.data.unwrap_or_default(),
      r.launch
    ),
    _ => "timeout\n".to_string(),
  };
  let _ = std::fs::write(&out, text);
  eprintln!("[e2e-coldstart] wrote {}", out.display());
  let _ = std::io::Write::flush(&mut std::io::stderr());
  // As finish(): _exit skips the backend's static teardown.
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
/// Activate with the toast's arguments.
#[cfg(windows)]
async fn com_activation_check(
  responses: &Arc<Mutex<Vec<NotificationResponse>>>,
) {
  let args = "laufey=1&tag=e2e-com&action=yes&data=%7B%7D";
  match win::activate_toast(args) {
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
