//! A window the user closes through the OS, then every kind of call naming
//! its id (part of the window-api battery).
//!
//! The close goes through the window system, not `close_window`: the window
//! manager's close (Alt+F4 under openbox, X11), `WM_CLOSE` posted to the
//! window (Windows), `-[NSWindow performClose:]` on the main thread (macOS).
//! The backend then tears the window down from its own destroy path, which
//! must drop the window's state the way `close_window` does: afterwards the
//! getters read as a closed window and the setters, `execute_js` and the
//! answer to a JS call the page made before the close are no-ops — none of
//! them may reach the destroyed native window (a use after free on the
//! WebKitGTK and WebView2 backends before laufey 0.8.0).

use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{JsCall, Value, Window};

use crate::{check, na, wait_for};

const TITLE: &str = "native-e2e-user-close";

pub async fn run() {
  let held: Arc<Mutex<Option<JsCall>>> = Arc::new(Mutex::new(None));
  let win = Window::new(260, 200)
    .title(TITLE)
    .bind("closeHold", {
      let held = held.clone();
      // Never answered here: the answer goes out after the close.
      move |call| *held.lock().unwrap() = Some(call)
    })
    .load(&format!("laufey-e2e://app/titled/{TITLE}"));
  win.show();
  if !wait_for(|| win.get_size().0 != 0, 100, 50).await {
    check("user-close window reports a size", false);
    return;
  }

  // A JS call from the page, left pending across the close (web engines).
  // Asked for until the page makes it, for up to 60 s: the window exists
  // well before its page does, and on a busy machine the page took up to
  // 21 s to start loading (windows-ci WebView2 with both cores busy: the
  // page answered nothing until 15-21 s in 3 of 20 runs, then made the call
  // within 0.6 s), which the old 10 s budget read as a call never made.
  if laufey::scheme_handlers_supported() {
    let asked = std::time::Instant::now();
    while asked.elapsed() < Duration::from_secs(60) {
      if held.lock().unwrap().is_some() {
        break;
      }
      win.execute_js(
        "typeof Laufey !== 'undefined' && typeof Laufey.closeHold === \
         'function' && Laufey.closeHold().catch(() => {})",
        None::<fn(_)>,
      );
      tokio::time::sleep(Duration::from_millis(100)).await;
    }
    check(
      &format!(
        "the page's JS call is pending before the user close (after {} ms)",
        asked.elapsed().as_millis()
      ),
      held.lock().unwrap().is_some(),
    );
  }

  match os_close(TITLE).await {
    None => {
      win.close();
      return;
    }
    Some(false) => {
      check("the OS close reached the window", false);
      win.close();
      return;
    }
    Some(true) => {}
  }
  let closed = wait_for(|| win.get_size() == (0, 0), 100, 50).await;
  check(
    "a window the user closed reads as closed (size 0x0)",
    closed,
  );
  if !closed {
    return;
  }

  // Every call below names the destroyed window. Before the fix they
  // dereferenced its freed native state; now they find no window.
  check("get_position of the closed window is (0, 0)", {
    win.get_position() == (0, 0)
  });
  // What these read for an unknown id differs between backends; that they
  // return at all is the check.
  let _ = win.get_visible();
  let _ = win.get_opacity();
  let _ = win.get_always_on_top();
  win.set_title("after-close");
  win.set_size(300, 200);
  win.set_position(10, 10);
  win.navigate("about:blank");
  win.hide();
  win.execute_js("1 + 1", None::<fn(_)>);
  if let Some(call) = held.lock().unwrap().take() {
    call.resolve(Value::Bool(true));
  }
  // A beat for anything queued above to reach the UI thread.
  tokio::time::sleep(Duration::from_millis(300)).await;
  check(
    "calls naming a user-closed window are no-ops (the process is alive)",
    win.get_size() == (0, 0),
  );
}

/// Closes the window titled `title` the way the user would. `None` when this
/// run can't (no window manager on X11, Wayland), after reporting N/A.
async fn os_close(title: &str) -> Option<bool> {
  #[cfg(target_os = "linux")]
  {
    if crate::os_view::xdotool().is_none()
      || std::env::var_os("LAUFEY_E2E_WM_RUNNING").is_none()
    {
      na("a user close through the window manager (needs xdotool and a WM)");
      return None;
    }
    if !crate::os_view::x_focus(title).await {
      return Some(false);
    }
    // openbox binds Alt+F4 to Close: WM_DELETE_WINDOW to the client.
    Some(crate::os_view::xdo(&["key", "--clearmodifiers", "alt+F4"]).is_some())
  }
  #[cfg(windows)]
  {
    #[link(name = "user32")]
    extern "system" {
      fn PostMessageW(
        hwnd: *mut std::ffi::c_void,
        msg: u32,
        wparam: usize,
        lparam: isize,
      ) -> i32;
    }
    const WM_CLOSE: u32 = 0x0010;
    let mut hwnd = std::ptr::null_mut();
    for _ in 0..100 {
      hwnd = crate::menu_notification_checks::win::find_window(title);
      if !hwnd.is_null() {
        break;
      }
      tokio::time::sleep(Duration::from_millis(50)).await;
    }
    if hwnd.is_null() {
      return Some(false);
    }
    Some(unsafe { PostMessageW(hwnd, WM_CLOSE, 0, 0) } != 0)
  }
  #[cfg(target_os = "macos")]
  {
    let title = title.to_string();
    let closed = laufey::spawn_on_ui_thread(move || mac::perform_close(&title))
      .await
      .unwrap_or(false);
    Some(closed)
  }
  #[cfg(not(any(target_os = "linux", windows, target_os = "macos")))]
  {
    let _ = title;
    na("a user close through the window system");
    None
  }
}

#[cfg(target_os = "macos")]
mod mac {
  use std::ffi::{c_char, c_void, CStr};

  #[link(name = "objc")]
  extern "C" {
    fn objc_getClass(name: *const c_char) -> *mut c_void;
    fn sel_registerName(name: *const c_char) -> *mut c_void;
    fn objc_msgSend();
  }

  unsafe fn send0(obj: *mut c_void, sel: &CStr) -> *mut c_void {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel_registerName(sel.as_ptr()))
  }

  unsafe fn send_usize(obj: *mut c_void, sel: &CStr) -> usize {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> usize =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel_registerName(sel.as_ptr()))
  }

  unsafe fn send_index(obj: *mut c_void, sel: &CStr, i: usize) -> *mut c_void {
    let f: unsafe extern "C" fn(
      *mut c_void,
      *mut c_void,
      usize,
    ) -> *mut c_void =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel_registerName(sel.as_ptr()), i)
  }

  unsafe fn send_obj(obj: *mut c_void, sel: &CStr, arg: *mut c_void) {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel_registerName(sel.as_ptr()), arg)
  }

  /// `-[NSWindow performClose:]` on the first window of the app titled
  /// `title` — what the title bar's close button does. Main thread.
  pub fn perform_close(title: &str) -> bool {
    unsafe {
      let app = send0(
        objc_getClass(c"NSApplication".as_ptr()),
        c"sharedApplication",
      );
      let windows = send0(app, c"windows");
      for i in 0..send_usize(windows, c"count") {
        let window = send_index(windows, c"objectAtIndex:", i);
        let name = send0(window, c"title");
        let utf8 = send0(name, c"UTF8String") as *const c_char;
        if !utf8.is_null()
          && CStr::from_ptr(utf8).to_bytes() == title.as_bytes()
        {
          send_obj(window, c"performClose:", std::ptr::null_mut());
          return true;
        }
      }
      false
    }
  }
}
