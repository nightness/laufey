// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Menus and notifications (API 41): the context-menu close callback, menu
//! accelerator capabilities and test hooks, notification scheduling, the
//! pending list, cancel, and responses (clicks no live notification owns,
//! including the one that launched the app).
//!
//! See `docs/menus.md` and `docs/notifications.md`. Every function here may
//! be called from any thread. Backends older than API 41, and the Winit
//! backend, report all of it as unsupported.

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::{Arc, Mutex, OnceLock};

use crate::{api, MenuItem, Value, Window};

// --- Constants (mirror laufey.h) -------------------------------------------

pub const LAUFEY_MENU_CAP_APP_MENU: u32 = 1 << 0;
pub const LAUFEY_MENU_CAP_ACCELERATORS: u32 = 1 << 1;
pub const LAUFEY_MENU_CAP_CONTEXT_MENU: u32 = 1 << 2;
pub const LAUFEY_MENU_CAP_CONTEXT_CLOSED: u32 = 1 << 3;
pub const LAUFEY_MENU_CAP_ICONS: u32 = 1 << 4;
pub const LAUFEY_MENU_CAP_TOOLTIPS: u32 = 1 << 5;

pub const LAUFEY_NOTIFICATION_CAP_SHOW: u32 = 1 << 0;
pub const LAUFEY_NOTIFICATION_CAP_SCHEDULE: u32 = 1 << 1;
pub const LAUFEY_NOTIFICATION_CAP_SCHEDULE_PERSISTS: u32 = 1 << 2;
pub const LAUFEY_NOTIFICATION_CAP_ACTIONS: u32 = 1 << 3;
pub const LAUFEY_NOTIFICATION_CAP_CLICKS: u32 = 1 << 4;
pub const LAUFEY_NOTIFICATION_CAP_COLD_START: u32 = 1 << 5;

pub const LAUFEY_MAX_PENDING_NOTIFICATION_RESPONSES: usize = 16;
pub const LAUFEY_NOTIFICATION_MAX_TAG_BYTES: usize = 256;
pub const LAUFEY_NOTIFICATION_MAX_DATA_BYTES: usize = 4096;

// ===========================================================================
// Menus
// ===========================================================================

/// What [`menu_capabilities`] reports for this backend and OS.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct MenuCapabilities {
  pub bits: u32,
}

impl MenuCapabilities {
  /// [`Window::set_menu`] shows a menu (bar).
  pub fn app_menu(&self) -> bool {
    self.bits & LAUFEY_MENU_CAP_APP_MENU != 0
  }
  /// App-menu items' accelerators fire them from the keyboard.
  pub fn accelerators(&self) -> bool {
    self.bits & LAUFEY_MENU_CAP_ACCELERATORS != 0
  }
  /// [`Window::show_context_menu`] works.
  pub fn context_menu(&self) -> bool {
    self.bits & LAUFEY_MENU_CAP_CONTEXT_MENU != 0
  }
  /// [`Window::show_context_menu_with_close`] reports the menu closing.
  pub fn context_closed(&self) -> bool {
    self.bits & LAUFEY_MENU_CAP_CONTEXT_CLOSED != 0
  }
  /// Item icons are drawn.
  pub fn icons(&self) -> bool {
    self.bits & LAUFEY_MENU_CAP_ICONS != 0
  }
  /// Item tooltips are shown.
  pub fn tooltips(&self) -> bool {
    self.bits & LAUFEY_MENU_CAP_TOOLTIPS != 0
  }
}

/// The menu capabilities of this backend.
pub fn menu_capabilities() -> MenuCapabilities {
  let api = api();
  let bits = match api.menu_capabilities {
    Some(f) => unsafe { f(api.backend_data) },
    None => 0,
  };
  MenuCapabilities { bits }
}

type MenuClosedHandler = Box<dyn FnOnce() + Send>;

struct ContextMenuCallbacks {
  on_click: Box<dyn Fn(&str) + Send + Sync>,
  on_closed: Mutex<Option<MenuClosedHandler>>,
}

unsafe extern "C" fn context_menu_ex_click(
  user_data: *mut c_void,
  _window_id: u32,
  item_id: *const c_char,
) {
  if user_data.is_null() || item_id.is_null() {
    return;
  }
  // SAFETY: `user_data` is the Arc the menu holds until on_closed.
  let callbacks = &*(user_data as *const ContextMenuCallbacks);
  let id = CStr::from_ptr(item_id).to_string_lossy();
  (callbacks.on_click)(&id);
}

unsafe extern "C" fn context_menu_ex_closed(
  user_data: *mut c_void,
  _window_id: u32,
) {
  if user_data.is_null() {
    return;
  }
  // SAFETY: the backend fires this exactly once; it releases the Arc taken
  // in `show_context_menu_with_close`.
  let callbacks = Arc::from_raw(user_data as *const ContextMenuCallbacks);
  let closed = callbacks.on_closed.lock().unwrap().take();
  if let Some(f) = closed {
    f();
  }
}

impl Window {
  /// Show a context menu at `(x, y)` (window coordinates, top-left origin),
  /// like [`Window::show_context_menu`], and call `on_closed` once it closed:
  /// after `on_click` when an item was chosen, or alone when it was
  /// dismissed. Never blocks. Without the close event
  /// ([`MenuCapabilities::context_closed`]) the menu still shows through
  /// `show_context_menu`, and `on_closed` is never called.
  pub fn show_context_menu_with_close<F, G>(
    &self,
    x: i32,
    y: i32,
    template: &[MenuItem],
    on_click: F,
    on_closed: G,
  ) where
    F: Fn(&str) + Send + Sync + 'static,
    G: FnOnce() + Send + 'static,
  {
    let api = api();
    let Some(f) = api.show_context_menu_ex else {
      self.show_context_menu(x, y, template, on_click);
      return;
    };
    let value = Value::List(template.iter().map(|i| i.to_value()).collect());
    let callbacks = Arc::new(ContextMenuCallbacks {
      on_click: Box::new(on_click),
      on_closed: Mutex::new(Some(Box::new(on_closed))),
    });
    let raw = value.to_raw();
    let user_data = Arc::into_raw(callbacks) as *mut c_void;
    // SAFETY: the backend owns `raw` (frees it) and calls
    // context_menu_ex_closed exactly once, which releases `user_data`.
    unsafe {
      f(
        api.backend_data,
        self.id,
        x,
        y,
        raw,
        Some(context_menu_ex_click),
        user_data,
        Some(context_menu_ex_closed),
        user_data,
      );
    }
  }
}

/// Test-only. Close the open context menu as Escape would. Returns `false`
/// when none is open or the backend has no hook.
pub fn test_dismiss_context_menu() -> bool {
  let api = api();
  match api.test_dismiss_context_menu {
    Some(f) => unsafe { f(api.backend_data) },
    None => false,
  }
}

/// Test-only. Press `accelerator` in `window` through the backend's own
/// accelerator dispatch. Returns `true` if an app-menu item fired.
pub fn test_trigger_menu_accelerator(
  window: &Window,
  accelerator: &str,
) -> bool {
  let api = api();
  let Some(f) = api.test_trigger_menu_accelerator else {
    return false;
  };
  let Ok(c) = CString::new(accelerator) else {
    return false;
  };
  unsafe { f(api.backend_data, window.id(), c.as_ptr()) }
}

// ===========================================================================
// Notifications
// ===========================================================================

/// What [`notification_capabilities`] reports for this backend and OS.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct NotificationCapabilities {
  pub bits: u32,
}

impl NotificationCapabilities {
  pub fn show(&self) -> bool {
    self.bits & LAUFEY_NOTIFICATION_CAP_SHOW != 0
  }
  /// [`crate::Notification::schedule_at`] delivers at that time, at least
  /// while the app runs.
  pub fn schedule(&self) -> bool {
    self.bits & LAUFEY_NOTIFICATION_CAP_SCHEDULE != 0
  }
  /// The OS delivers a scheduled notification while the app isn't running.
  pub fn schedule_persists(&self) -> bool {
    self.bits & LAUFEY_NOTIFICATION_CAP_SCHEDULE_PERSISTS != 0
  }
  pub fn actions(&self) -> bool {
    self.bits & LAUFEY_NOTIFICATION_CAP_ACTIONS != 0
  }
  pub fn clicks(&self) -> bool {
    self.bits & LAUFEY_NOTIFICATION_CAP_CLICKS != 0
  }
  /// A click while the app isn't running launches it and is delivered.
  pub fn cold_start(&self) -> bool {
    self.bits & LAUFEY_NOTIFICATION_CAP_COLD_START != 0
  }
}

/// The notification capabilities of this backend (on Linux, also whether a
/// notification server runs).
pub fn notification_capabilities() -> NotificationCapabilities {
  let api = api();
  let bits = match api.notification_capabilities {
    Some(f) => unsafe { f(api.backend_data) },
    None => 0,
  };
  NotificationCapabilities { bits }
}

/// A click on a notification that no live [`crate::Notification`] callback
/// owns: one from an earlier run of the app, or the one that launched it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NotificationResponse {
  pub tag: String,
  /// The action button's id, or `None` for the body.
  pub action: Option<String>,
  pub data: Option<String>,
  /// Delivered before a handler was registered: the click that launched the
  /// app (or one made while it was starting).
  pub launch: bool,
}

/// A notification scheduled with [`crate::Notification::schedule_at`] that
/// hasn't been delivered yet.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScheduledNotification {
  pub tag: String,
  pub title: String,
  pub body: String,
  /// Unix time in milliseconds.
  pub at_ms: i64,
  pub data: Option<String>,
  pub actions: Vec<crate::NotificationAction>,
}

type ResponseHandler = Box<dyn Fn(NotificationResponse) + Send + Sync>;

fn response_handler() -> &'static Mutex<Option<Arc<ResponseHandler>>> {
  static HANDLER: OnceLock<Mutex<Option<Arc<ResponseHandler>>>> =
    OnceLock::new();
  HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn notification_response_trampoline(
  _user_data: *mut c_void,
  json: *const c_char,
) {
  if json.is_null() {
    return;
  }
  let text = CStr::from_ptr(json).to_string_lossy();
  let Some(response) = parse_response(&text) else {
    return;
  };
  let handler = response_handler().lock().unwrap().clone();
  if let Some(h) = handler {
    h(response);
  }
}

/// Register the (process-wide) handler for notification responses (see
/// [`NotificationResponse`]). Responses that arrived before (the click that
/// launched the app) are delivered right away, on this thread; later ones on
/// a backend thread.
pub fn set_notification_response_handler<F>(handler: F)
where
  F: Fn(NotificationResponse) + Send + Sync + 'static,
{
  *response_handler().lock().unwrap() = Some(Arc::new(Box::new(handler)));
  let api = api();
  if let Some(f) = api.set_notification_response_handler {
    unsafe {
      f(
        api.backend_data,
        Some(notification_response_trampoline),
        std::ptr::null_mut(),
      )
    };
  }
}

/// Unregister the response handler; later responses are held again.
pub fn clear_notification_response_handler() {
  let api = api();
  if let Some(f) = api.set_notification_response_handler {
    unsafe { f(api.backend_data, None, std::ptr::null_mut()) };
  }
  *response_handler().lock().unwrap() = None;
}

type ListCallback = Box<dyn FnOnce(Vec<ScheduledNotification>) + Send>;

unsafe extern "C" fn notification_list_trampoline(
  user_data: *mut c_void,
  json: *const c_char,
) {
  if user_data.is_null() {
    return;
  }
  // SAFETY: the box made in list_scheduled_notifications; fired once.
  let cb = Box::from_raw(user_data as *mut ListCallback);
  let list = if json.is_null() {
    Vec::new()
  } else {
    parse_list(&CStr::from_ptr(json).to_string_lossy())
  };
  cb(list);
}

/// The scheduled notifications not delivered yet, soonest first. `callback`
/// runs exactly once: on this thread or a backend thread.
pub fn list_scheduled_notifications<F>(callback: F)
where
  F: FnOnce(Vec<ScheduledNotification>) + Send + 'static,
{
  let api = api();
  let Some(f) = api.list_scheduled_notifications else {
    callback(Vec::new());
    return;
  };
  let boxed: Box<ListCallback> = Box::new(Box::new(callback));
  unsafe {
    f(
      api.backend_data,
      Some(notification_list_trampoline),
      Box::into_raw(boxed) as *mut c_void,
    )
  };
}

/// Cancel the scheduled notification `tag` and remove delivered ones with
/// that tag from the notification center.
pub fn cancel_notification(tag: &str) {
  let api = api();
  let Some(f) = api.cancel_notification else {
    return;
  };
  if let Ok(c) = CString::new(tag) {
    unsafe { f(api.backend_data, c.as_ptr()) };
  }
}

/// Test-only. A click on notification `tag` (its body, or the action
/// `action`) through the dispatch the OS response uses. Returns `true` if a
/// live callback or the response handler received it.
pub fn test_notification_respond(tag: &str, action: Option<&str>) -> bool {
  let api = api();
  let Some(f) = api.test_notification_respond else {
    return false;
  };
  let Ok(t) = CString::new(tag) else {
    return false;
  };
  let a = match action.map(CString::new) {
    Some(Ok(a)) => Some(a),
    Some(Err(_)) => return false,
    None => None,
  };
  unsafe {
    f(
      api.backend_data,
      t.as_ptr(),
      a.as_ref().map_or(std::ptr::null(), |a| a.as_ptr()),
    )
  }
}

/// A tag for a scheduled notification the caller didn't name.
pub(crate) fn generated_tag() -> String {
  use std::sync::atomic::{AtomicU64, Ordering};
  static NEXT: AtomicU64 = AtomicU64::new(1);
  let now = std::time::SystemTime::now()
    .duration_since(std::time::UNIX_EPOCH)
    .map(|d| d.as_nanos() as u64)
    .unwrap_or(0);
  format!(
    "laufey-rs-{:x}-{}-{}",
    now,
    std::process::id(),
    NEXT.fetch_add(1, Ordering::Relaxed)
  )
}

// --- A minimal JSON reader for the backend's response and list JSON --------

#[derive(Debug, Clone, PartialEq)]
enum Json {
  Null,
  Bool(bool),
  Number(f64),
  String(String),
  Array(Vec<Json>),
  Object(Vec<(String, Json)>),
}

impl Json {
  fn get(&self, key: &str) -> Option<&Json> {
    match self {
      Json::Object(fields) => {
        fields.iter().find(|(k, _)| k == key).map(|(_, v)| v)
      }
      _ => None,
    }
  }
  fn str(&self) -> Option<&str> {
    match self {
      Json::String(s) => Some(s),
      _ => None,
    }
  }
}

struct Parser<'a> {
  s: &'a [u8],
  i: usize,
}

impl<'a> Parser<'a> {
  fn ws(&mut self) {
    while self.i < self.s.len()
      && matches!(self.s[self.i], b' ' | b'\n' | b'\r' | b'\t')
    {
      self.i += 1;
    }
  }
  fn eat(&mut self, lit: &str) -> bool {
    if self.s[self.i..].starts_with(lit.as_bytes()) {
      self.i += lit.len();
      true
    } else {
      false
    }
  }
  fn value(&mut self, depth: u32) -> Option<Json> {
    if depth > 16 {
      return None;
    }
    self.ws();
    match *self.s.get(self.i)? {
      b'n' => self.eat("null").then_some(Json::Null),
      b't' => self.eat("true").then_some(Json::Bool(true)),
      b'f' => self.eat("false").then_some(Json::Bool(false)),
      b'"' => self.string().map(Json::String),
      b'[' => {
        self.i += 1;
        let mut items = Vec::new();
        self.ws();
        if self.eat("]") {
          return Some(Json::Array(items));
        }
        loop {
          items.push(self.value(depth + 1)?);
          self.ws();
          if self.eat("]") {
            return Some(Json::Array(items));
          }
          if !self.eat(",") {
            return None;
          }
        }
      }
      b'{' => {
        self.i += 1;
        let mut fields = Vec::new();
        self.ws();
        if self.eat("}") {
          return Some(Json::Object(fields));
        }
        loop {
          self.ws();
          let k = self.string()?;
          self.ws();
          if !self.eat(":") {
            return None;
          }
          fields.push((k, self.value(depth + 1)?));
          self.ws();
          if self.eat("}") {
            return Some(Json::Object(fields));
          }
          if !self.eat(",") {
            return None;
          }
        }
      }
      _ => {
        let start = self.i;
        while self.i < self.s.len()
          && matches!(
            self.s[self.i],
            b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9'
          )
        {
          self.i += 1;
        }
        std::str::from_utf8(&self.s[start..self.i])
          .ok()?
          .parse()
          .ok()
          .map(Json::Number)
      }
    }
  }
  fn string(&mut self) -> Option<String> {
    if !self.eat("\"") {
      return None;
    }
    let mut out: Vec<u8> = Vec::new();
    loop {
      let c = *self.s.get(self.i)?;
      self.i += 1;
      match c {
        b'"' => return String::from_utf8(out).ok(),
        b'\\' => {
          let e = *self.s.get(self.i)?;
          self.i += 1;
          match e {
            b'"' => out.push(b'"'),
            b'\\' => out.push(b'\\'),
            b'/' => out.push(b'/'),
            b'b' => out.push(8),
            b'f' => out.push(12),
            b'n' => out.push(b'\n'),
            b'r' => out.push(b'\r'),
            b't' => out.push(b'\t'),
            b'u' => {
              let mut cp = self.hex4()?;
              if (0xD800..0xDC00).contains(&cp) {
                if !self.eat("\\u") {
                  return None;
                }
                let lo = self.hex4()?;
                cp =
                  0x10000 + ((cp - 0xD800) << 10) + (lo.checked_sub(0xDC00)?);
              }
              let ch = char::from_u32(cp)?;
              let mut buf = [0u8; 4];
              out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
            }
            _ => return None,
          }
        }
        _ => out.push(c),
      }
    }
  }
  fn hex4(&mut self) -> Option<u32> {
    let h = std::str::from_utf8(self.s.get(self.i..self.i + 4)?).ok()?;
    self.i += 4;
    u32::from_str_radix(h, 16).ok()
  }
}

fn parse_json(text: &str) -> Option<Json> {
  let mut p = Parser {
    s: text.as_bytes(),
    i: 0,
  };
  let v = p.value(0)?;
  p.ws();
  (p.i == p.s.len()).then_some(v)
}

fn parse_response(text: &str) -> Option<NotificationResponse> {
  let v = parse_json(text)?;
  Some(NotificationResponse {
    tag: v.get("tag")?.str()?.to_string(),
    action: v.get("action").and_then(|a| a.str()).map(str::to_string),
    data: v.get("data").and_then(|d| d.str()).map(str::to_string),
    launch: matches!(v.get("launch"), Some(Json::Bool(true))),
  })
}

fn parse_list(text: &str) -> Vec<ScheduledNotification> {
  let Some(Json::Array(items)) = parse_json(text) else {
    return Vec::new();
  };
  items
    .iter()
    .filter_map(|item| {
      let actions = match item.get("actions") {
        Some(Json::Array(a)) => a
          .iter()
          .filter_map(|a| {
            Some(crate::NotificationAction {
              id: a.get("id")?.str()?.to_string(),
              title: a.get("title")?.str()?.to_string(),
            })
          })
          .collect(),
        _ => Vec::new(),
      };
      Some(ScheduledNotification {
        tag: item.get("tag")?.str()?.to_string(),
        title: item.get("title")?.str().unwrap_or_default().to_string(),
        body: item
          .get("body")
          .and_then(|b| b.str())
          .unwrap_or_default()
          .to_string(),
        at_ms: match item.get("at") {
          Some(Json::Number(n)) => *n as i64,
          _ => 0,
        },
        data: item.get("data").and_then(|d| d.str()).map(str::to_string),
        actions,
      })
    })
    .collect()
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn parses_responses() {
    let r = parse_response(
      r#"{"tag":"té\"x","action":"ok","data":"{\"a\":1}","launch":true}"#,
    )
    .unwrap();
    assert_eq!(r.tag, "té\"x");
    assert_eq!(r.action.as_deref(), Some("ok"));
    assert_eq!(r.data.as_deref(), Some("{\"a\":1}"));
    assert!(r.launch);
    let r =
      parse_response(r#"{"tag":"t","action":null,"data":null,"launch":false}"#)
        .unwrap();
    assert_eq!(r.action, None);
    assert_eq!(r.data, None);
    assert!(!r.launch);
    assert!(parse_response("{").is_none());
    assert!(parse_response(r#"{"action":null}"#).is_none());
    // A surrogate pair.
    let r = parse_response(r#"{"tag":"😀","launch":false}"#).unwrap();
    assert_eq!(r.tag, "😀");
  }

  #[test]
  fn parses_lists() {
    let list = parse_list(
      r#"[{"tag":"a","title":"A","body":"","at":100,"data":"d","actions":[{"id":"x","title":"X"}]},{"tag":"b","title":"B","body":"b","at":1700000000123,"data":null,"actions":[]}]"#,
    );
    assert_eq!(list.len(), 2);
    assert_eq!(list[0].tag, "a");
    assert_eq!(list[0].at_ms, 100);
    assert_eq!(list[0].data.as_deref(), Some("d"));
    assert_eq!(list[0].actions[0].id, "x");
    assert_eq!(list[1].at_ms, 1700000000123);
    assert_eq!(list[1].data, None);
    assert!(parse_list("[]").is_empty());
    assert!(parse_list("nope").is_empty());
  }

  #[test]
  fn capability_bits() {
    let m = MenuCapabilities {
      bits: LAUFEY_MENU_CAP_ACCELERATORS | LAUFEY_MENU_CAP_CONTEXT_CLOSED,
    };
    assert!(m.accelerators() && m.context_closed() && !m.icons());
    let n = NotificationCapabilities {
      bits: LAUFEY_NOTIFICATION_CAP_SCHEDULE
        | LAUFEY_NOTIFICATION_CAP_COLD_START,
    };
    assert!(n.schedule() && n.cold_start() && !n.actions());
  }

  #[test]
  fn generated_tags_differ() {
    assert_ne!(generated_tag(), generated_tag());
  }
}
