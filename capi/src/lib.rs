// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#![allow(clippy::type_complexity)]

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::future::Future;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};

use tokio::sync::Notify;

#[allow(non_upper_case_globals)]
#[allow(non_camel_case_types)]
#[allow(non_snake_case)]
#[allow(dead_code)]
mod ffi {
  include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

mod keyboard;
pub use keyboard::*;

mod mouse;
pub use mouse::*;

mod window_state;
pub use window_state::*;

mod io;
pub use io::*;

mod system;
pub use system::*;

mod menus_notifications;
pub use menus_notifications::*;

mod ui_thread;
pub use ui_thread::*;

mod auth_session;
pub use auth_session::*;

/// Version of this laufey crate. Used by downstream consumers (e.g. the Deno CLI)
/// to locate matching prebuilt backend binaries in GitHub releases
/// (`github.com/denoland/laufey/releases/tag/v{VERSION}`).
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

pub const LAUFEY_API_VERSION: u32 = 43;

/// Creation-time window style flags for [`Window::new_with_options`].
/// Mirror the `LAUFEY_WINDOW_FLAG_*` constants in `laufey.h`.
pub const LAUFEY_WINDOW_FLAG_FRAMELESS: u32 = 1 << 0;
pub const LAUFEY_WINDOW_FLAG_NO_ACTIVATE: u32 = 1 << 1;
pub const LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR: u32 = 1 << 2;
pub const LAUFEY_WINDOW_FLAG_HIDDEN: u32 = 1 << 3;
pub const LAUFEY_WINDOW_FLAG_TRANSPARENT: u32 = 1 << 4;

pub const LAUFEY_WINDOW_HANDLE_UNKNOWN: i32 = 0;
pub const LAUFEY_WINDOW_HANDLE_APPKIT: i32 = 1;
pub const LAUFEY_WINDOW_HANDLE_WIN32: i32 = 2;
pub const LAUFEY_WINDOW_HANDLE_X11: i32 = 3;
pub const LAUFEY_WINDOW_HANDLE_WAYLAND: i32 = 4;
pub type LaufeyValue = ffi::laufey_value_t;
pub type LaufeyBackendApi = ffi::laufey_backend_api_t;

unsafe impl Send for LaufeyBackendApi {}
unsafe impl Sync for LaufeyBackendApi {}

static BACKEND_API: OnceLock<&'static LaufeyBackendApi> = OnceLock::new();
static SHUTDOWN_FLAG: AtomicBool = AtomicBool::new(false);
static BINDINGS: OnceLock<
  Mutex<HashMap<u32, HashMap<String, Arc<BindingHandler>>>>,
> = OnceLock::new();
static JS_CALL_NOTIFY: OnceLock<Notify> = OnceLock::new();
static MENU_CLICK_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Arc<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();
static CONTEXT_MENU_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Arc<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();
static DOCK_MENU_HANDLER: OnceLock<
  Mutex<Option<Arc<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();
static DOCK_REOPEN_HANDLER: OnceLock<
  Mutex<Option<Arc<dyn Fn(bool) + Send + Sync>>>,
> = OnceLock::new();
static OPEN_URL_HANDLER: OnceLock<
  Mutex<Option<Arc<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();
type SecondInstanceHandler = Arc<dyn Fn(&[String], &str) + Send + Sync>;
static SECOND_INSTANCE_HANDLER: OnceLock<Mutex<Option<SecondInstanceHandler>>> =
  OnceLock::new();
static TRAY_MENU_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Arc<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();
static TRAY_CLICK_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Arc<dyn Fn() + Send + Sync>>>,
> = OnceLock::new();
static TRAY_DBLCLICK_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Arc<dyn Fn() + Send + Sync>>>,
> = OnceLock::new();
static NOTIFICATION_HANDLERS: OnceLock<
  Mutex<HashMap<u32, NotificationHandler>>,
> = OnceLock::new();

/// A notification's event callback: the notification id and the event.
type NotificationHandler = Arc<dyn Fn(u32, NotificationEvent) + Send + Sync>;

enum BindingHandler {
  Sync(Box<dyn Fn(JsCall) + Send + Sync>),
  Async(
    Box<
      dyn Fn(JsCall) -> std::pin::Pin<Box<dyn Future<Output = ()> + Send>>
        + Send
        + Sync,
    >,
  ),
}

fn api() -> &'static LaufeyBackendApi {
  BACKEND_API.get().expect("Backend API not initialized")
}

// Non-panicking variant for callback paths that may run before (or without)
// init_api — e.g. unit tests driving trampolines directly.
pub(crate) fn try_api() -> Option<&'static LaufeyBackendApi> {
  BACKEND_API.get().copied()
}

fn bindings(
) -> &'static Mutex<HashMap<u32, HashMap<String, Arc<BindingHandler>>>> {
  BINDINGS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn js_call_notify() -> &'static Notify {
  JS_CALL_NOTIFY.get_or_init(Notify::new)
}

/// # Safety
/// `api` must be either null or a valid pointer to a `LaufeyBackendApi` with
/// static lifetime.
pub unsafe fn init_api(api: *const LaufeyBackendApi) -> c_int {
  if api.is_null() {
    return -1;
  }
  let api_ref: &'static LaufeyBackendApi = unsafe { &*api };
  if api_ref.version != LAUFEY_API_VERSION {
    eprintln!(
      "API version mismatch: expected {}, got {}",
      LAUFEY_API_VERSION, api_ref.version
    );
    return -2;
  }
  match BACKEND_API.set(api_ref) {
    Ok(_) => 0,
    Err(_) => -3,
  }
}

pub fn shutdown() {
  SHUTDOWN_FLAG.store(true, Ordering::SeqCst);
  if let Some(notify) = JS_CALL_NOTIFY.get() {
    notify.notify_one();
  }
}

pub fn should_shutdown() -> bool {
  SHUTDOWN_FLAG.load(Ordering::SeqCst)
}

#[derive(Clone)]
pub enum Value {
  Null,
  Bool(bool),
  Int(i32),
  Double(f64),
  String(String),
  List(Vec<Value>),
  Dict(HashMap<String, Value>),
  Binary(Vec<u8>),
}

/// Frees a value the caller owns (null is fine).
///
/// # Safety
/// `val` must be null or an owned value of the backend behind `api`.
unsafe fn free_value(api: &LaufeyBackendApi, val: *mut LaufeyValue) {
  if val.is_null() {
    return;
  }
  if let Some(free) = api.value_free {
    free(val);
  }
}

impl Value {
  /// # Safety
  /// `ptr` must be null or a valid pointer to a `LaufeyValue` produced by the
  /// backend API.
  pub unsafe fn from_raw(ptr: *mut LaufeyValue) -> Option<Self> {
    if ptr.is_null() {
      return None;
    }
    let api = api();

    if api.value_is_null.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Null);
    }
    if api.value_is_bool.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Bool(
        api.value_get_bool.map(|f| f(ptr)).unwrap_or(false),
      ));
    }
    if api.value_is_int.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Int(api.value_get_int.map(|f| f(ptr)).unwrap_or(0)));
    }
    if api.value_is_double.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Double(
        api.value_get_double.map(|f| f(ptr)).unwrap_or(0.0),
      ));
    }
    if api.value_is_string.map(|f| f(ptr)).unwrap_or(false) {
      let mut len: usize = 0;
      if let Some(get_str) = api.value_get_string {
        let c_str = get_str(ptr, &mut len);
        if !c_str.is_null() {
          let s = CStr::from_ptr(c_str).to_string_lossy().into_owned();
          if let Some(free_str) = api.value_free_string {
            free_str(c_str);
          }
          return Some(Value::String(s));
        }
      }
      return Some(Value::String(String::new()));
    }
    if api.value_is_list.map(|f| f(ptr)).unwrap_or(false) {
      let size = api.value_list_size.map(|f| f(ptr)).unwrap_or(0);
      let mut list = Vec::with_capacity(size);
      if let Some(get_item) = api.value_list_get {
        for i in 0..size {
          // An owned copy of the item (laufey.h): converted, then freed.
          let item = get_item(ptr, i);
          if let Some(v) = Value::from_raw(item) {
            list.push(v);
          }
          free_value(api, item);
        }
      }
      return Some(Value::List(list));
    }
    if api.value_is_dict.map(|f| f(ptr)).unwrap_or(false) {
      let mut dict = HashMap::new();
      let mut count: usize = 0;
      if let Some(get_keys) = api.value_dict_keys {
        let keys = get_keys(ptr, &mut count);
        if !keys.is_null() {
          for i in 0..count {
            let key_ptr = *keys.add(i);
            if !key_ptr.is_null() {
              let key = CStr::from_ptr(key_ptr).to_string_lossy().into_owned();
              if let Some(get_val) = api.value_dict_get {
                // An owned copy of the entry (laufey.h): converted, then
                // freed.
                let val = get_val(ptr, key_ptr);
                if let Some(v) = Value::from_raw(val) {
                  dict.insert(key, v);
                }
                free_value(api, val);
              }
            }
          }
          if let Some(free_keys) = api.value_free_keys {
            free_keys(keys, count);
          }
        }
      }
      return Some(Value::Dict(dict));
    }
    if api.value_is_binary.map(|f| f(ptr)).unwrap_or(false) {
      let mut len: usize = 0;
      if let Some(get_bin) = api.value_get_binary {
        let data = get_bin(ptr, &mut len);
        if !data.is_null() && len > 0 {
          let slice = std::slice::from_raw_parts(data as *const u8, len);
          return Some(Value::Binary(slice.to_vec()));
        }
      }
      return Some(Value::Binary(Vec::new()));
    }

    Some(Value::Null)
  }

  pub fn to_raw(&self) -> *mut LaufeyValue {
    let api = api();
    let bd = api.backend_data;

    unsafe {
      match self {
        Value::Null => api
          .value_null
          .map(|f| f(bd))
          .unwrap_or(std::ptr::null_mut()),
        Value::Bool(v) => api
          .value_bool
          .map(|f| f(bd, *v))
          .unwrap_or(std::ptr::null_mut()),
        Value::Int(v) => api
          .value_int
          .map(|f| f(bd, *v))
          .unwrap_or(std::ptr::null_mut()),
        Value::Double(v) => api
          .value_double
          .map(|f| f(bd, *v))
          .unwrap_or(std::ptr::null_mut()),
        // A string with a NUL byte cannot cross the C ABI (C strings end at
        // the first NUL): it becomes null rather than a truncated string.
        Value::String(s) => match CString::new(s.as_str()) {
          Ok(c_str) => api
            .value_string
            .map(|f| f(bd, c_str.as_ptr()))
            .unwrap_or(std::ptr::null_mut()),
          Err(_) => api
            .value_null
            .map(|f| f(bd))
            .unwrap_or(std::ptr::null_mut()),
        },
        Value::List(items) => {
          let list = api
            .value_list
            .map(|f| f(bd))
            .unwrap_or(std::ptr::null_mut());
          if !list.is_null() {
            if let Some(append) = api.value_list_append {
              for item in items {
                let raw = item.to_raw();
                append(list, raw);
              }
            }
          }
          list
        }
        Value::Dict(map) => {
          let dict = api
            .value_dict
            .map(|f| f(bd))
            .unwrap_or(std::ptr::null_mut());
          if !dict.is_null() {
            if let Some(set) = api.value_dict_set {
              for (k, v) in map {
                // A key with a NUL byte cannot cross the C ABI: the entry
                // is left out.
                let Ok(c_key) = CString::new(k.as_str()) else {
                  continue;
                };
                let raw = v.to_raw();
                set(dict, c_key.as_ptr(), raw);
              }
            }
          }
          dict
        }
        Value::Binary(data) => api
          .value_binary
          .map(|f| f(bd, data.as_ptr() as *const c_void, data.len()))
          .unwrap_or(std::ptr::null_mut()),
      }
    }
  }

  /// Whether a string or dict key anywhere in this value contains a NUL
  /// byte (which the C ABI's strings cannot carry).
  fn has_interior_nul(&self) -> bool {
    match self {
      Value::String(s) => s.contains('\0'),
      Value::List(items) => items.iter().any(Value::has_interior_nul),
      Value::Dict(map) => map
        .iter()
        .any(|(k, v)| k.contains('\0') || v.has_interior_nul()),
      _ => false,
    }
  }

  pub fn as_string(&self) -> Option<&str> {
    match self {
      Value::String(s) => Some(s.as_str()),
      _ => None,
    }
  }

  pub fn as_int(&self) -> Option<i32> {
    match self {
      Value::Int(i) => Some(*i),
      _ => None,
    }
  }

  pub fn as_bool(&self) -> Option<bool> {
    match self {
      Value::Bool(b) => Some(*b),
      _ => None,
    }
  }

  pub fn as_list(&self) -> Option<&Vec<Value>> {
    match self {
      Value::List(l) => Some(l),
      _ => None,
    }
  }

  pub fn as_dict(&self) -> Option<&HashMap<String, Value>> {
    match self {
      Value::Dict(d) => Some(d),
      _ => None,
    }
  }
}

const NUL_IN_RESULT: &str =
  "the result contains a NUL byte, which cannot cross the laufey C ABI";

pub struct JsCall {
  pub window_id: u32,
  pub call_id: u64,
  pub method: String,
  pub args: Vec<Value>,
}

impl JsCall {
  /// Answers the call with `value`. A string in `value` (or a dict key)
  /// containing a NUL byte cannot cross the C ABI, whose strings end at the
  /// first NUL: the call is then rejected with an error saying so instead.
  pub fn resolve(self, value: Value) {
    if value.has_interior_nul() {
      return self.reject(Value::String(NUL_IN_RESULT.into()));
    }
    let api = api();
    if let Some(respond) = api.js_call_respond {
      let raw = value.to_raw();
      unsafe {
        respond(api.backend_data, self.call_id, raw, std::ptr::null_mut())
      };
    }
  }

  /// Rejects the call with `error` (NUL bytes as for [`JsCall::resolve`]).
  pub fn reject(self, error: Value) {
    let error = if error.has_interior_nul() {
      Value::String(NUL_IN_RESULT.into())
    } else {
      error
    };
    let api = api();
    if let Some(respond) = api.js_call_respond {
      let raw = error.to_raw();
      unsafe {
        respond(api.backend_data, self.call_id, std::ptr::null_mut(), raw)
      };
    }
  }
}

unsafe extern "C" fn js_call_handler(
  _user_data: *mut c_void,
  window_id: u32,
  call_id: u64,
  method_path: *const c_char,
  args: *mut LaufeyValue,
) {
  let method = if method_path.is_null() {
    String::new()
  } else {
    CStr::from_ptr(method_path).to_string_lossy().into_owned()
  };

  let args_vec = if args.is_null() {
    Vec::new()
  } else {
    match Value::from_raw(args) {
      Some(Value::List(l)) => l,
      _ => Vec::new(),
    }
  };

  let call = JsCall {
    window_id,
    call_id,
    method: method.clone(),
    args: args_vec,
  };

  // Cloned out so the handler runs without the lock held: it may bind or
  // unbind (which take the lock) itself.
  let handler = bindings()
    .lock()
    .unwrap()
    .get(&window_id)
    .and_then(|b| b.get(&method).cloned());
  if let Some(handler) = handler {
    match handler.as_ref() {
      BindingHandler::Sync(f) => f(call),
      BindingHandler::Async(f) => {
        let fut = f(call);
        tokio::spawn(fut);
      }
    }
    return;
  }
  call.reject(Value::String(format!("No binding for '{}'", method)));
}

fn register_js_handler() {
  let api = api();
  if let Some(set_handler) = api.set_js_call_handler {
    unsafe {
      set_handler(
        api.backend_data,
        Some(js_call_handler),
        std::ptr::null_mut(),
      );
    }
  }
}

unsafe extern "C" fn js_call_notify_callback(_user_data: *mut c_void) {
  js_call_notify().notify_one();
}

fn register_js_notify() {
  let api = api();
  if let Some(set_notify) = api.set_js_call_notify {
    unsafe {
      set_notify(
        api.backend_data,
        Some(js_call_notify_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

fn ensure_js_handler() {
  static HANDLER_REGISTERED: AtomicBool = AtomicBool::new(false);
  if !HANDLER_REGISTERED.swap(true, Ordering::SeqCst) {
    register_js_handler();
    register_js_notify();
  }
}

fn poll_js_calls() {
  let api = api();
  if let Some(f) = api.poll_js_calls {
    unsafe { f(api.backend_data) };
  }
}

/// Async event loop that dispatches JS calls as they arrive.
/// Blocks until `should_shutdown()` returns true.
pub async fn run() {
  ensure_js_handler();
  loop {
    js_call_notify().notified().await;
    poll_js_calls();
    if should_shutdown() {
      break;
    }
  }
}

// --- Custom URL scheme handler (in-process app transport) -------------------

static SCHEME_HANDLER: OnceLock<
  Mutex<Option<Arc<dyn Fn(SchemeRequest) + Send + Sync>>>,
> = OnceLock::new();

fn scheme_handler_store(
) -> &'static Mutex<Option<Arc<dyn Fn(SchemeRequest) + Send + Sync>>> {
  SCHEME_HANDLER.get_or_init(|| Mutex::new(None))
}

/// One in-flight scheme request/response exchange. Bridges a webview request
/// for a registered scheme to the embedder, which streams a response back.
pub struct SchemeRequest {
  pub window_id: u32,
  pub method: String,
  pub url: String,
  pub headers: Vec<(String, String)>,
  pub exchange: SchemeExchange,
}

/// Handle used to read the request body and stream the response back to the
/// webview. Backed by the backend's `scheme_*` vtable functions.
pub struct SchemeExchange(*mut ffi::laufey_scheme_exchange_t, Arc<AtomicBool>);

/// The cancel flag of every exchange the runtime holds, by exchange pointer:
/// set by the backend's on_cancel, dropped when the exchange is finished
/// (the backend never calls on_cancel once finish has returned, so a later
/// exchange at the same address can't be marked by an earlier one's cancel).
fn scheme_cancel_flags() -> &'static Mutex<HashMap<usize, Arc<AtomicBool>>> {
  static FLAGS: OnceLock<Mutex<HashMap<usize, Arc<AtomicBool>>>> =
    OnceLock::new();
  FLAGS.get_or_init(|| Mutex::new(HashMap::new()))
}

unsafe extern "C" fn scheme_cancel_trampoline(
  _user_data: *mut c_void,
  exchange: *mut ffi::laufey_scheme_exchange_t,
) {
  if let Some(flag) = scheme_cancel_flags()
    .lock()
    .unwrap()
    .get(&(exchange as usize))
  {
    flag.store(true, Ordering::SeqCst);
  }
}

// The exchange handle is owned and synchronized by the backend; the embedder
// may move it across threads and share references across them (e.g. hold a
// `&SchemeExchange` across an await on a multi-threaded tokio runtime). Every
// method forwards to a backend vtable function that synchronizes internally.
unsafe impl Send for SchemeExchange {}
unsafe impl Sync for SchemeExchange {}

impl SchemeExchange {
  /// Pull up to `buf.len()` bytes of the request body. Returns bytes read
  /// (>0), 0 at end of body, or a negative value on error.
  pub fn read_body(&self, buf: &mut [u8]) -> isize {
    let api = api();
    match api.scheme_request_read_body {
      Some(f) => unsafe {
        f(api.backend_data, self.0, buf.as_mut_ptr(), buf.len())
      },
      None => -1,
    }
  }

  /// Send the response status and headers. Call once, before `write`.
  pub fn begin(&self, status: i32, headers: &[(String, String)]) {
    let api = api();
    if let Some(f) = api.scheme_response_begin {
      let flat = flatten_headers(headers);
      unsafe {
        f(
          api.backend_data,
          self.0,
          status as c_int,
          flat.as_ptr() as *const c_char,
          flat.len(),
        )
      };
    }
  }

  /// Append response body bytes. Returns bytes accepted, or a negative value
  /// if the webview has gone away (the embedder should then stop and drop
  /// the exchange via `finish`).
  pub fn write(&self, buf: &[u8]) -> isize {
    let api = api();
    match api.scheme_response_write {
      Some(f) => unsafe {
        f(api.backend_data, self.0, buf.as_ptr(), buf.len())
      },
      None => -1,
    }
  }

  /// Whether the webview cancelled the request (the fetch was aborted, the
  /// document replaced, the window closed) before the response finished.
  /// Once true, stop writing and call [`SchemeExchange::finish`]; the calls
  /// stay safe until then. Any thread.
  ///
  /// Where a backend can't see a cancel (WebKitGTK before the head is sent,
  /// a WebView2 request answered in one piece), it stays false and the next
  /// [`SchemeExchange::write`] after the cancel returns a negative value
  /// instead. See docs/custom-schemes.md.
  pub fn is_cancelled(&self) -> bool {
    self.1.load(Ordering::SeqCst)
  }

  /// Complete the response and release the exchange.
  pub fn finish(self) {
    {
      let mut flags = scheme_cancel_flags().lock().unwrap();
      if flags
        .get(&(self.0 as usize))
        .is_some_and(|f| Arc::ptr_eq(f, &self.1))
      {
        flags.remove(&(self.0 as usize));
      }
    }
    let api = api();
    if let Some(f) = api.scheme_response_finish {
      unsafe { f(api.backend_data, self.0) };
    }
  }
}

fn flatten_headers(headers: &[(String, String)]) -> Vec<u8> {
  let mut out = Vec::new();
  for (name, value) in headers {
    out.extend_from_slice(name.as_bytes());
    out.push(0);
    out.extend_from_slice(value.as_bytes());
    out.push(0);
  }
  out
}

unsafe fn parse_flat_headers(
  ptr: *const c_char,
  len: usize,
) -> Vec<(String, String)> {
  if ptr.is_null() || len == 0 {
    return Vec::new();
  }
  let bytes = unsafe { std::slice::from_raw_parts(ptr as *const u8, len) };
  let mut out = Vec::new();
  let mut parts = bytes.split(|&b| b == 0);
  while let (Some(name), Some(value)) = (parts.next(), parts.next()) {
    if name.is_empty() && value.is_empty() {
      break;
    }
    out.push((
      String::from_utf8_lossy(name).into_owned(),
      String::from_utf8_lossy(value).into_owned(),
    ));
  }
  out
}

unsafe extern "C" fn scheme_request_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  exchange: *mut ffi::laufey_scheme_exchange_t,
  method: *const c_char,
  url: *const c_char,
  headers: *const c_char,
  headers_len: usize,
) {
  let to_string = |p: *const c_char| {
    if p.is_null() {
      String::new()
    } else {
      unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
    }
  };
  let req = SchemeRequest {
    window_id,
    method: to_string(method),
    url: to_string(url),
    headers: unsafe { parse_flat_headers(headers, headers_len) },
    exchange: SchemeExchange(exchange, {
      let flag = Arc::new(AtomicBool::new(false));
      scheme_cancel_flags()
        .lock()
        .unwrap()
        .insert(exchange as usize, flag.clone());
      flag
    }),
  };
  // Cloned out: the handler runs without the lock held.
  let handler = scheme_handler_store().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(req);
  } else {
    // No handler registered: release the exchange so the webview isn't hung.
    req.exchange.finish();
  }
}

/// Register `handler` to service every webview request for `scheme` (the
/// scheme name only, e.g. `"app"`). The handler runs on a backend-internal
/// thread and must not block it; offload work (e.g. onto a tokio task).
/// Requires a backend built against laufey API version 26 or newer.
///
/// Call it once per scheme (the built-in `"app"` plus any of your own). One
/// handler serves every registered scheme — a later call replaces the handler
/// for all of them and adds the new scheme — so dispatch on `request.url`.
/// Each registered scheme is a real origin in the page (`<scheme>://<host>`,
/// secure context, CORS, per-origin storage).
///
/// Register every scheme **before creating the first window**: the system
/// web views read their scheme tables when a web view is created, so a later
/// registration is not served by existing windows (WebKitGTK excepted;
/// WebView2 fixes the set for the whole process). The CEF backend instead
/// needs the schemes declared at launch — `--laufey-custom-schemes=myapp` or
/// `LAUFEY_CUSTOM_SCHEMES=myapp` — because Chromium registers them before the
/// runtime is loaded.
///
/// ```no_run
/// // Before the first window: the engines read their scheme tables then.
/// laufey::register_scheme_handler("myapp", |req| {
///   let (status, body): (i32, &[u8]) = match req.url.as_str() {
///     "myapp://app/" => (200, b"<!doctype html><h1>Hello</h1>"),
///     _ => (404, b"not found"),
///   };
///   let headers = [("content-type".to_string(), "text/html".to_string())];
///   req.exchange.begin(status, &headers);
///   req.exchange.write(body);
///   req.exchange.finish();
/// });
/// // location.origin in this window is "myapp://app".
/// let _window = laufey::Window::new(800, 600).load("myapp://app/");
/// ```
pub fn register_scheme_handler<F>(scheme: &str, handler: F)
where
  F: Fn(SchemeRequest) + Send + Sync + 'static,
{
  // A scheme with a NUL byte is no scheme: nothing is registered.
  let Ok(c_scheme) = CString::new(scheme) else {
    return;
  };
  *scheme_handler_store().lock().unwrap() = Some(Arc::new(handler));
  let api = api();
  if let Some(register) = api.register_scheme_handler {
    unsafe {
      register(
        api.backend_data,
        c_scheme.as_ptr(),
        Some(scheme_request_trampoline),
        Some(scheme_cancel_trampoline),
        std::ptr::null_mut(),
      );
    }
  }
}

/// Whether the backend implements custom scheme handlers (API >= 26 and a
/// web engine). `false` on engine-less backends such as Winit, where
/// [`register_scheme_handler`] is a no-op. Lets capability-probed tests tell
/// "unsupported here" from "supported but broken".
///
/// ```no_run
/// if !laufey::scheme_handlers_supported() {
///   // Engine-less backend: serve the app over a loopback socket instead.
/// }
/// ```
pub fn scheme_handlers_supported() -> bool {
  supports_scheme_handlers(api())
}

fn supports_scheme_handlers(api: &LaufeyBackendApi) -> bool {
  api.register_scheme_handler.is_some()
}

pub fn quit() {
  let api = api();
  if let Some(f) = api.quit {
    unsafe { f(api.backend_data) };
  }
}

/// Run `f` on the backend UI thread and block until it returns.
///
/// AppKit requires main-thread access (e.g. `raw-window-metal` / `NSView`),
/// and so do Win32 windows and GTK objects for their own threads. With a
/// backend of API 42 or newer this hops through `dispatch_ui_task` on every
/// platform (inline when already on the UI thread); see
/// [`try_run_on_ui_thread`], which returns an error instead of panicking,
/// and [`spawn_on_ui_thread`], which doesn't block.
///
/// Without `dispatch_ui_task` it only hops on Apple platforms, through the
/// C ABI `post_ui_task` (winit event loop / GCD main / CEF `TID_UI`), unless
/// already on the process main thread (`pthread_main_np`) or the backend has
/// no `post_ui_task`; elsewhere `f` runs inline. Prefer this over
/// `dispatch_sync`, which deadlocks against a backend that owns the main run
/// loop (winit).
///
/// # Panics
///
/// Panics if the UI task can't run: the backend's event loop has ended (the
/// app is quitting) or ends before the task got to run. A panic in `f` is
/// resumed on the caller's thread.
pub fn run_on_ui_thread<F, R>(f: F) -> R
where
  F: FnOnce() -> R + Send + 'static,
  R: Send + 'static,
{
  if api().dispatch_ui_task.is_some() {
    return match try_run_on_ui_thread(f) {
      Ok(value) => value,
      Err(e) => panic!("run_on_ui_thread: {e}"),
    };
  }
  #[cfg(not(any(target_os = "macos", target_os = "ios")))]
  {
    f()
  }

  #[cfg(any(target_os = "macos", target_os = "ios"))]
  {
    unsafe extern "C" {
      fn pthread_main_np() -> i32;
    }
    // SAFETY: libSystem symbol on Apple.
    if unsafe { pthread_main_np() != 0 } {
      return f();
    }
    let api = api();
    let Some(post) = api.post_ui_task else {
      return f();
    };

    let (tx, rx) = std::sync::mpsc::sync_channel(0);
    type Job = Box<dyn FnOnce() + Send>;
    let job = Box::into_raw(Box::new(Box::new(move || {
      let _ = tx.send(f());
    }) as Job));

    unsafe extern "C" fn trampoline(data: *mut c_void) {
      // SAFETY: `data` is the `Box<Job>` passed to `post_ui_task`; not retained.
      let job = unsafe { Box::from_raw(data as *mut Job) };
      (*job)();
    }

    // SAFETY: trampoline owns and frees `job`; we block until it runs.
    unsafe {
      post(api.backend_data, Some(trampoline), job as *mut c_void);
    }
    rx.recv().expect("UI task dropped without running")
  }
}

// --- Window ---

pub struct Window {
  id: u32,
}

/// Creation-time window style options. Properties that must be decided when
/// the OS window is constructed (frameless chrome, non-activating panel
/// behavior). Post-creation properties (size, position, resizable,
/// always-on-top) are set through their respective `Window` setters.
#[derive(Clone, Copy, Debug, Default)]
pub struct WindowOptions {
  /// Remove the title bar and standard window chrome.
  pub frameless: bool,
  /// Float above normal windows as a utility "panel" and do not activate
  /// the app / steal key focus when shown. Combined with `frameless`, this
  /// is the configuration used for tray / menu-bar popovers.
  pub no_activate: bool,
  /// Keep the standard frame and traffic-light buttons, but make the title
  /// bar transparent and let the web content extend under it (Electron
  /// `titleBarStyle: 'hidden'`). macOS only; ignored elsewhere.
  pub transparent_titlebar: bool,
  /// Create the window without showing it. It stays hidden until [`Window::show`]
  /// (or [`Window::focus`]) is called — typically from a [`crate::on_page_load`]
  /// handler so the first reveal happens only once content has painted, avoiding
  /// the empty/black initial frame (most visible on Wayland).
  pub hidden: bool,
  /// Give the window a transparent background so the web content's own alpha
  /// composites against whatever is behind the window. Any region the page
  /// leaves transparent (e.g. `background: transparent` on the root element)
  /// shows the desktop through it. Often combined with `frameless` for a
  /// borderless translucent look. Distinct from [`Window::set_opacity`], which
  /// uniformly fades the whole window. Supported by the system-WebView backend
  /// on macOS and Linux and by the Winit backend; ignored by the Windows
  /// WebView2 and CEF backends, which paint an opaque window background.
  pub transparent: bool,
}

impl WindowOptions {
  fn to_flags(self) -> u32 {
    let mut flags = 0;
    if self.frameless {
      flags |= LAUFEY_WINDOW_FLAG_FRAMELESS;
    }
    if self.no_activate {
      flags |= LAUFEY_WINDOW_FLAG_NO_ACTIVATE;
    }
    if self.transparent_titlebar {
      flags |= LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR;
    }
    if self.hidden {
      flags |= LAUFEY_WINDOW_FLAG_HIDDEN;
    }
    if self.transparent {
      flags |= LAUFEY_WINDOW_FLAG_TRANSPARENT;
    }
    flags
  }
}

/// How long [`Window::print_to_pdf`] waits for the backend's completion
/// handler before resolving the callback with a timeout `Err`. Rendering that
/// takes this long has effectively hung (navigation mid-render, a completion
/// handler the platform never delivers); the watchdog guarantees the caller's
/// callback always resolves instead of leaking.
const PRINT_TO_PDF_TIMEOUT: std::time::Duration =
  std::time::Duration::from_secs(60);

type PdfCallback = Box<dyn FnOnce(Result<Vec<u8>, String>) + Send>;

// Shared completion slot for one `print_to_pdf` call: the backend trampoline
// and the watchdog race to `take()` the callback (that is what makes delivery
// exactly-once), and the trampoline signals `done` so the watchdog thread
// exits as soon as the result arrives instead of sleeping out the full
// timeout.
struct PdfSlot {
  callback: Mutex<Option<PdfCallback>>,
  done: Condvar,
}

impl Window {
  pub fn new(width: i32, height: i32) -> Self {
    Self::new_with_options(width, height, WindowOptions::default())
  }

  /// Create a window with creation-time style options. Falls back to a plain
  /// window (ignoring the options) on backends older than API version 25.
  pub fn new_with_options(
    width: i32,
    height: i32,
    options: WindowOptions,
  ) -> Self {
    let api = api();
    let flags = options.to_flags();
    let id = if let (Some(f), true) = (api.create_window_ex, flags != 0) {
      unsafe { f(api.backend_data, flags) }
    } else if let Some(f) = api.create_window {
      unsafe { f(api.backend_data) }
    } else {
      0
    };
    let win = Window { id };
    if let Some(f) = api.set_window_size {
      unsafe { f(api.backend_data, id, width, height) };
    }
    win
  }

  /// Wrap an existing window by its ID (does not create a new OS window).
  pub fn from_id(id: u32) -> Self {
    Window { id }
  }

  pub fn id(&self) -> u32 {
    self.id
  }

  pub fn title(self, title: &str) -> Self {
    self.set_title(title);
    self
  }

  /// A `title` containing a NUL byte cannot cross the C ABI; the call is
  /// then a no-op.
  pub fn set_title(&self, title: &str) {
    let api = api();
    if let (Some(f), Ok(c_title)) = (api.set_title, CString::new(title)) {
      unsafe { f(api.backend_data, self.id, c_title.as_ptr()) };
    }
  }

  pub fn load(self, path: &str) -> Self {
    self.navigate(path);
    self
  }

  /// A `url` containing a NUL byte is not a URL; the call is then a no-op.
  pub fn navigate(&self, url: &str) {
    let api = api();
    if let (Some(f), Ok(c_url)) = (api.navigate, CString::new(url)) {
      unsafe { f(api.backend_data, self.id, c_url.as_ptr()) };
    }
  }

  pub fn size(self, width: i32, height: i32) -> Self {
    self.set_size(width, height);
    self
  }

  pub fn set_size(&self, width: i32, height: i32) {
    let api = api();
    if let Some(f) = api.set_window_size {
      unsafe { f(api.backend_data, self.id, width, height) };
    }
  }

  pub fn get_size(&self) -> (i32, i32) {
    let api = api();
    let mut width: c_int = 0;
    let mut height: c_int = 0;
    if let Some(f) = api.get_window_size {
      unsafe { f(api.backend_data, self.id, &mut width, &mut height) };
    }
    (width, height)
  }

  /// Chrome-inclusive size (`window.outerWidth` / `outerHeight`).
  /// Falls back to [`Window::get_size`] when the backend does not report it.
  pub fn get_outer_size(&self) -> (i32, i32) {
    let api = api();
    if let Some(f) = api.get_window_outer_size {
      let mut width: c_int = 0;
      let mut height: c_int = 0;
      unsafe { f(api.backend_data, self.id, &mut width, &mut height) };
      (width, height)
    } else {
      self.get_size()
    }
  }

  /// Physical pixels per DIP for this window (`window.devicePixelRatio`).
  /// Returns `1.0` when the backend does not report a scale.
  pub fn get_scale_factor(&self) -> f64 {
    let api = api();
    if let Some(f) = api.get_window_scale_factor {
      unsafe { f(api.backend_data, self.id) }
    } else {
      1.0
    }
  }

  pub fn position(self, x: i32, y: i32) -> Self {
    self.set_position(x, y);
    self
  }

  pub fn set_position(&self, x: i32, y: i32) {
    let api = api();
    if let Some(f) = api.set_window_position {
      unsafe { f(api.backend_data, self.id, x, y) };
    }
  }

  pub fn get_position(&self) -> (i32, i32) {
    let api = api();
    let mut x: c_int = 0;
    let mut y: c_int = 0;
    if let Some(f) = api.get_window_position {
      unsafe { f(api.backend_data, self.id, &mut x, &mut y) };
    }
    (x, y)
  }

  /// Top-left of the content view in screen DIP. Falls back to
  /// [`Window::get_position`] when the backend does not report it.
  pub fn get_inner_position(&self) -> (i32, i32) {
    let api = api();
    if let Some(f) = api.get_window_inner_position {
      let mut x: c_int = 0;
      let mut y: c_int = 0;
      unsafe { f(api.backend_data, self.id, &mut x, &mut y) };
      (x, y)
    } else {
      self.get_position()
    }
  }

  pub fn resizable(self, resizable: bool) -> Self {
    self.set_resizable(resizable);
    self
  }

  pub fn set_resizable(&self, resizable: bool) {
    let api = api();
    if let Some(f) = api.set_resizable {
      unsafe { f(api.backend_data, self.id, resizable) };
    }
  }

  pub fn get_resizable(&self) -> bool {
    let api = api();
    if let Some(f) = api.is_resizable {
      unsafe { f(api.backend_data, self.id) }
    } else {
      true
    }
  }

  pub fn always_on_top(self, always_on_top: bool) -> Self {
    self.set_always_on_top(always_on_top);
    self
  }

  pub fn set_always_on_top(&self, always_on_top: bool) {
    let api = api();
    if let Some(f) = api.set_always_on_top {
      unsafe { f(api.backend_data, self.id, always_on_top) };
    }
  }

  pub fn get_always_on_top(&self) -> bool {
    let api = api();
    if let Some(f) = api.is_always_on_top {
      unsafe { f(api.backend_data, self.id) }
    } else {
      false
    }
  }

  /// Builder form of [`Window::set_opacity`].
  pub fn opacity(self, opacity: f64) -> Self {
    self.set_opacity(opacity);
    self
  }

  /// Set the window's overall opacity, a uniform factor in `[0.0, 1.0]` where
  /// `1.0` is fully opaque (the default) and `0.0` is fully transparent. This
  /// fades the entire window — web content and native chrome alike — like CSS
  /// `opacity`. It is distinct from [`WindowOptions::transparent`], which makes
  /// the background transparent while honoring the page's own per-pixel alpha.
  /// Out-of-range values are clamped. No-op on backends without opacity support
  /// (e.g. the Winit backend).
  pub fn set_opacity(&self, opacity: f64) {
    let api = api();
    if let Some(f) = api.set_window_opacity {
      unsafe { f(api.backend_data, self.id, opacity.clamp(0.0, 1.0)) };
    }
  }

  /// Get the window's current opacity in `[0.0, 1.0]`. Returns `1.0` when the
  /// backend does not report opacity.
  pub fn get_opacity(&self) -> f64 {
    let api = api();
    if let Some(f) = api.get_window_opacity {
      unsafe { f(api.backend_data, self.id) }
    } else {
      1.0
    }
  }

  /// Builder form of [`Window::set_click_passthrough`].
  pub fn click_passthrough(self, passthrough: bool) -> Self {
    self.set_click_passthrough(passthrough);
    self
  }

  /// Enable or disable click passthrough. While enabled the window ignores
  /// all mouse input — clicks, moves, wheel — and every event falls through
  /// to whatever window is beneath it, like Electron's
  /// `setIgnoreMouseEvents(true)`. Keyboard input is unaffected. Intended for
  /// frameless/transparent overlay windows (HUDs, notification toasts, screen
  /// annotations); can be toggled at any time. No-op on backends without
  /// passthrough support.
  pub fn set_click_passthrough(&self, passthrough: bool) {
    let api = api();
    if let Some(f) = api.set_click_passthrough {
      unsafe { f(api.backend_data, self.id, passthrough) };
    }
  }

  /// Get whether click passthrough is currently enabled. Returns `false`
  /// when the backend does not report it.
  pub fn get_click_passthrough(&self) -> bool {
    let api = api();
    if let Some(f) = api.is_click_passthrough {
      unsafe { f(api.backend_data, self.id) }
    } else {
      false
    }
  }

  /// Builder form of [`Window::set_click_passthrough_forward`].
  pub fn click_passthrough_forward(self, forward: bool) -> Self {
    self.set_click_passthrough_forward(forward);
    self
  }

  /// While click passthrough is enabled, keep this window's mouse events
  /// flowing to the registered [`Window::on_mouse_click`] /
  /// [`Window::on_mouse_move`] / [`Window::on_wheel`] handlers even though
  /// the OS delivers them to whatever is
  /// beneath the window — like Electron's
  /// `setIgnoreMouseEvents(true, { forward: true })`. Observation only: the
  /// events cannot be consumed, and they are reported only while passthrough
  /// is active and the cursor is over the visible window. This enables the
  /// standard interactive-overlay pattern: watch mouse moves and call
  /// [`Window::set_click_passthrough`]`(false)` when the cursor enters an
  /// interactive region.
  ///
  /// Currently implemented on macOS; other platforms ignore the flag (see
  /// the click-passthrough section of the window-management docs).
  pub fn set_click_passthrough_forward(&self, forward: bool) {
    let api = api();
    if let Some(f) = api.set_click_passthrough_forward {
      unsafe { f(api.backend_data, self.id, forward) };
    }
  }

  /// Get whether click-passthrough forwarding is currently enabled. Returns
  /// `false` when the backend does not support forwarding.
  pub fn get_click_passthrough_forward(&self) -> bool {
    let api = api();
    if let Some(f) = api.is_click_passthrough_forward {
      unsafe { f(api.backend_data, self.id) }
    } else {
      false
    }
  }

  pub fn get_visible(&self) -> bool {
    let api = api();
    if let Some(f) = api.is_visible {
      unsafe { f(api.backend_data, self.id) }
    } else {
      true
    }
  }

  pub fn show(&self) {
    let api = api();
    if let Some(f) = api.show {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  pub fn hide(&self) {
    let api = api();
    if let Some(f) = api.hide {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  pub fn focus(&self) {
    let api = api();
    if let Some(f) = api.focus {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  pub fn close(&self) {
    let api = api();
    if let Some(f) = api.close_window {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  /// A `script` containing a NUL byte cannot cross the C ABI: it is not
  /// run, and `callback` receives an `Err` saying so.
  pub fn execute_js<F>(&self, script: &str, callback: Option<F>)
  where
    F: FnOnce(Result<Value, Value>) + Send + 'static,
  {
    let api = api();
    if let Some(f) = api.execute_js {
      let Ok(c_script) = CString::new(script) else {
        if let Some(cb) = callback {
          cb(Err(Value::String("script contains a NUL byte".into())));
        }
        return;
      };

      match callback {
        Some(cb_fn) => {
          unsafe extern "C" fn trampoline(
            result: *mut LaufeyValue,
            error: *mut LaufeyValue,
            user_data: *mut c_void,
          ) {
            let cb = Box::from_raw(
              user_data as *mut Box<dyn FnOnce(Result<Value, Value>) + Send>,
            );
            if !error.is_null() {
              if let Some(e) = Value::from_raw(error) {
                cb(Err(e));
                return;
              }
            }
            let val = Value::from_raw(result).unwrap_or(Value::Null);
            cb(Ok(val));
          }

          let cb: Box<Box<dyn FnOnce(Result<Value, Value>) + Send>> =
            Box::new(Box::new(cb_fn));
          let user_data = Box::into_raw(cb) as *mut c_void;

          unsafe {
            f(
              api.backend_data,
              self.id,
              c_script.as_ptr(),
              Some(trampoline),
              user_data,
            )
          };
        }
        None => {
          unsafe {
            f(
              api.backend_data,
              self.id,
              c_script.as_ptr(),
              None,
              std::ptr::null_mut(),
            )
          };
        }
      }
    }
  }

  /// Render this window's current page to a PDF document.
  ///
  /// The callback always receives the PDF bytes on success. When `path` is
  /// `Some`, those bytes are also written to that filesystem path before the
  /// callback is invoked; a write failure surfaces as `Err`. Backends that
  /// cannot produce a PDF (or that are older than API version 32) invoke the
  /// callback with an `Err` describing the unsupported operation.
  ///
  /// On success the callback fires on the UI thread once rendering completes.
  /// The single early-validation error — an unsupported backend — is instead
  /// delivered synchronously on the calling thread, before any rendering is
  /// scheduled.
  ///
  /// The callback is guaranteed to be invoked exactly once. If the backend's
  /// completion handler never fires (a platform edge case: navigation
  /// mid-render, a completion handler the OS never delivers), a watchdog
  /// resolves the callback with a timeout `Err` after 60 seconds, delivered
  /// on a background thread.
  pub fn print_to_pdf<F>(&self, path: Option<&str>, callback: F)
  where
    F: FnOnce(Result<Vec<u8>, String>) + Send + 'static,
  {
    self.print_to_pdf_with_timeout(path, PRINT_TO_PDF_TIMEOUT, callback)
  }

  // Timeout-injectable body of `print_to_pdf`, split out so unit tests can
  // exercise the watchdog without waiting the production 60 seconds.
  fn print_to_pdf_with_timeout<F>(
    &self,
    path: Option<&str>,
    timeout: std::time::Duration,
    callback: F,
  ) where
    F: FnOnce(Result<Vec<u8>, String>) + Send + 'static,
  {
    let api = api();
    let Some(f) = api.print_to_pdf else {
      callback(Err("print_to_pdf is not supported by this backend".into()));
      return;
    };

    unsafe extern "C" fn trampoline(
      data: *const u8,
      len: usize,
      error: *const c_char,
      user_data: *mut c_void,
    ) {
      // Reclaim the slot's raw reference. `take()` makes delivery
      // exactly-once: if the watchdog already resolved the callback, a late
      // completion finds `None` and is dropped instead of double-invoking.
      let slot = Arc::from_raw(user_data as *const PdfSlot);
      let cb = {
        let mut guard = slot.callback.lock().unwrap();
        let cb = guard.take();
        // Wake the watchdog so its thread exits now rather than waiting out
        // the remainder of the timeout.
        slot.done.notify_all();
        cb
      };
      let Some(cb) = cb else {
        return;
      };
      if !error.is_null() {
        let msg = CStr::from_ptr(error).to_string_lossy().into_owned();
        cb(Err(msg));
        return;
      }
      let bytes = if data.is_null() || len == 0 {
        Vec::new()
      } else {
        std::slice::from_raw_parts(data, len).to_vec()
      };
      cb(Ok(bytes));
    }

    // Backends only ever deliver bytes; writing the caller's requested path
    // happens here, once, rather than divergently in every backend. The write
    // runs on whichever thread delivers the success result (normally the UI
    // thread — the same place backends used to do the write).
    let path = path.map(str::to_owned);
    let callback = move |result: Result<Vec<u8>, String>| {
      let result = match (result, path) {
        (Ok(bytes), Some(p)) => match std::fs::write(&p, &bytes) {
          Ok(()) => Ok(bytes),
          Err(e) => Err(format!("failed to write PDF to {p}: {e}")),
        },
        (result, _) => result,
      };
      callback(result)
    };

    let slot = Arc::new(PdfSlot {
      callback: Mutex::new(Some(Box::new(callback) as PdfCallback)),
      done: Condvar::new(),
    });

    // Watchdog: guarantee the callback resolves even when the backend's
    // completion handler never fires. One short-lived thread per call --
    // print_to_pdf is a low-frequency API and a plain thread works before
    // any async runtime is up. The trampoline signals `done` on delivery, so
    // this thread exits as soon as the result arrives.
    let watchdog = slot.clone();
    std::thread::spawn(move || {
      let guard = watchdog.callback.lock().unwrap();
      let (mut guard, _) = watchdog
        .done
        .wait_timeout_while(guard, timeout, |cb| cb.is_some())
        .unwrap();
      // Take the callback and release the slot's lock before the user
      // callback runs; holding it during `cb` would block a
      // concurrently-arriving completion inside the trampoline on the UI
      // thread (and deadlock if `cb` synchronously waits on that thread).
      let cb = guard.take();
      drop(guard);
      if let Some(cb) = cb {
        cb(Err(format!(
          "print_to_pdf timed out after {}s waiting for the backend",
          timeout.as_secs_f64()
        )));
      }
    });

    // The trampoline reclaims this raw reference. If the backend never
    // invokes the callback, the reference is leaked deliberately: it holds
    // only the small slot (the watchdog has already consumed and resolved
    // the caller's callback), and freeing it here could race a late
    // completion.
    let user_data = Arc::into_raw(slot) as *mut c_void;

    unsafe { f(api.backend_data, self.id, Some(trampoline), user_data) };
  }

  pub fn get_window_handle(&self) -> *mut c_void {
    let api = api();
    if let Some(f) = api.get_window_handle {
      unsafe { f(api.backend_data, self.id) }
    } else {
      std::ptr::null_mut()
    }
  }

  pub fn get_display_handle(&self) -> *mut c_void {
    let api = api();
    if let Some(f) = api.get_display_handle {
      unsafe { f(api.backend_data, self.id) }
    } else {
      std::ptr::null_mut()
    }
  }

  pub fn get_window_handle_type(&self) -> i32 {
    let api = api();
    if let Some(f) = api.get_window_handle_type {
      unsafe { f(api.backend_data, self.id) }
    } else {
      LAUFEY_WINDOW_HANDLE_UNKNOWN
    }
  }

  pub fn on_keyboard_event<F>(self, handler: F) -> Self
  where
    F: Fn(KeyboardEvent) + Send + Sync + 'static,
  {
    on_keyboard_event(self.id, handler);
    self
  }

  pub fn on_mouse_click<F>(self, handler: F) -> Self
  where
    F: Fn(MouseClickEvent) + Send + Sync + 'static,
  {
    on_mouse_click(self.id, handler);
    self
  }

  pub fn on_mouse_move<F>(self, handler: F) -> Self
  where
    F: Fn(MouseMoveEvent) + Send + Sync + 'static,
  {
    on_mouse_move(self.id, handler);
    self
  }

  pub fn on_wheel<F>(self, handler: F) -> Self
  where
    F: Fn(WheelEvent) + Send + Sync + 'static,
  {
    on_wheel(self.id, handler);
    self
  }

  pub fn on_cursor_enter_leave<F>(self, handler: F) -> Self
  where
    F: Fn(CursorEnterLeaveEvent) + Send + Sync + 'static,
  {
    on_cursor_enter_leave(self.id, handler);
    self
  }

  pub fn on_focused<F>(self, handler: F) -> Self
  where
    F: Fn(FocusedEvent) + Send + Sync + 'static,
  {
    on_focused(self.id, handler);
    self
  }

  pub fn on_resize<F>(self, handler: F) -> Self
  where
    F: Fn(ResizeEvent) + Send + Sync + 'static,
  {
    on_resize(self.id, handler);
    self
  }

  pub fn on_move<F>(self, handler: F) -> Self
  where
    F: Fn(MoveEvent) + Send + Sync + 'static,
  {
    on_move(self.id, handler);
    self
  }

  /// Registering this holds the window open: it won't close on its own
  /// once this fires. Doing nothing in the handler leaves it open; call
  /// `Window::close()` (synchronously in the handler, or later from any
  /// thread, e.g. after a confirm dialog) to actually close it.
  ///
  /// Per-window: only *this* window is held open. Other windows without
  /// their own handler keep closing immediately, even though the backend's
  /// C-level defer contract is process-wide (the capi completes the close
  /// on their behalf -- see `close_requested_trampoline`).
  ///
  /// Fires only for the window's own close control (title bar button,
  /// `Alt+F4`, a window manager's close action) -- never `Cmd+Q`, a "Quit"
  /// menu/tray item, or any other app-level termination. See `docs/c-abi.md`
  /// for why that's deliberate.
  ///
  /// The handler fires synchronously on the platform's native UI thread
  /// (e.g. AppKit's `windowShouldClose:`), which has **no ambient Tokio
  /// reactor** -- a bare `tokio::spawn` inside it panics. To resolve
  /// asynchronously, capture a `tokio::runtime::Handle` from inside your
  /// runtime beforehand and call `handle.spawn(...)` from the handler
  /// instead; a `Handle` works from any thread, including this one. For a
  /// synchronous confirm dialog, `Window::confirm()` can be called directly
  /// in the handler instead.
  pub fn on_close_requested<F>(self, handler: F) -> Self
  where
    F: Fn(CloseRequestedEvent) + Send + Sync + 'static,
  {
    on_close_requested(self.id, handler);
    self
  }

  pub fn on_page_load<F>(self, handler: F) -> Self
  where
    F: Fn(PageLoadEvent) + Send + Sync + 'static,
  {
    on_page_load(self.id, handler);
    self
  }

  pub fn add_binding<F>(&self, name: &str, handler: F)
  where
    F: Fn(JsCall) + Send + Sync + 'static,
  {
    ensure_js_handler();
    bindings()
      .lock()
      .unwrap()
      .entry(self.id)
      .or_default()
      .insert(
        name.to_string(),
        Arc::new(BindingHandler::Sync(Box::new(handler))),
      );
  }

  pub fn add_binding_async<F, Fut>(&self, name: &str, handler: F)
  where
    F: Fn(JsCall) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = ()> + Send + 'static,
  {
    ensure_js_handler();
    bindings()
      .lock()
      .unwrap()
      .entry(self.id)
      .or_default()
      .insert(
        name.to_string(),
        Arc::new(BindingHandler::Async(Box::new(move |call| {
          Box::pin(handler(call))
        }))),
      );
  }

  pub fn bind<F>(self, name: &str, handler: F) -> Self
  where
    F: Fn(JsCall) + Send + Sync + 'static,
  {
    self.add_binding(name, handler);
    self
  }

  pub fn bind_async<F, Fut>(self, name: &str, handler: F) -> Self
  where
    F: Fn(JsCall) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = ()> + Send + 'static,
  {
    self.add_binding_async(name, handler);
    self
  }

  pub fn unbind(&self, name: &str) {
    let mut bindings = bindings().lock().unwrap();
    if let Some(window_bindings) = bindings.get_mut(&self.id) {
      window_bindings.remove(name);
    }
  }

  /// Set the application menu for this window.
  /// On macOS, the menu is applied to the global menu bar and swapped when this window gains focus.
  /// On Windows/Linux, the menu is attached directly to this window.
  /// `on_click` is called with the `id` of the clicked menu item.
  pub fn set_menu<F>(&self, template: &[MenuItem], on_click: F)
  where
    F: Fn(&str) + Send + Sync + 'static,
  {
    let value = Value::List(template.iter().map(|i| i.to_value()).collect());

    {
      let mut handlers = menu_click_handlers().lock().unwrap();
      handlers.insert(self.id, Arc::new(on_click));
    }

    let api = api();
    if let Some(f) = api.set_application_menu {
      let raw = value.to_raw();
      unsafe {
        f(
          api.backend_data,
          self.id,
          raw,
          Some(menu_click_callback),
          std::ptr::null_mut(),
        );
      }
    }
  }

  /// Show a context menu at the given position (in window coordinates).
  /// Uses the same `MenuItem` template as `set_menu`.
  /// `on_click` is called with the `id` of the clicked menu item.
  pub fn show_context_menu<F>(
    &self,
    x: i32,
    y: i32,
    template: &[MenuItem],
    on_click: F,
  ) where
    F: Fn(&str) + Send + Sync + 'static,
  {
    let value = Value::List(template.iter().map(|i| i.to_value()).collect());

    {
      let mut handlers = context_menu_handlers().lock().unwrap();
      handlers.insert(self.id, Arc::new(on_click));
    }

    let api = api();
    if let Some(f) = api.show_context_menu {
      let raw = value.to_raw();
      unsafe {
        f(
          api.backend_data,
          self.id,
          x,
          y,
          raw,
          Some(context_menu_click_callback),
          std::ptr::null_mut(),
        );
      }
    }
  }

  /// Open the DevTools inspector for this window. A no-op when DevTools are
  /// disabled ([`devtools_enabled`]). See also [`Window::close_devtools`],
  /// [`Window::toggle_devtools`] and [`Window::is_devtools_open`].
  pub fn open_devtools(&self) {
    let api = api();
    if let Some(f) = api.open_devtools {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  /// Show an alert dialog. Blocks until dismissed.
  pub fn alert(&self, title: &str, message: &str) {
    show_dialog_blocking(self.id, LAUFEY_DIALOG_ALERT, title, message, "");
  }

  /// Show a confirm dialog. Returns `true` if OK was pressed. Blocks
  /// until dismissed; while the modal is up the platform's event loop is
  /// pumped so other LAUFEY windows continue to render and respond.
  pub fn confirm(&self, title: &str, message: &str) -> bool {
    let (confirmed, _) =
      show_dialog_blocking(self.id, LAUFEY_DIALOG_CONFIRM, title, message, "");
    confirmed
  }

  /// Show a prompt dialog with a text input. Returns `Some(text)` if OK
  /// was pressed, `None` if cancelled. Blocking semantics as `confirm`.
  pub fn prompt(
    &self,
    title: &str,
    message: &str,
    default_value: &str,
  ) -> Option<String> {
    let (confirmed, input) = show_dialog_blocking(
      self.id,
      LAUFEY_DIALOG_PROMPT,
      title,
      message,
      default_value,
    );
    if confirmed {
      input
    } else {
      None
    }
  }
}

/// Shared dialog implementation. `window_id == 0` ⇒ app-wide modal.
/// Returns `(confirmed, input_value)`. `input_value` is `Some` only when
/// the dialog was a prompt and the user confirmed.
fn show_dialog_blocking(
  window_id: u32,
  dialog_type: i32,
  title: &str,
  message: &str,
  default_value: &str,
) -> (bool, Option<String>) {
  let api = api();
  let Some(f) = api.show_dialog else {
    return (false, None);
  };
  // Text with a NUL byte cannot cross the C ABI: no dialog, as if cancelled.
  let (Ok(c_title), Ok(c_message), Ok(c_default)) = (
    CString::new(title),
    CString::new(message),
    CString::new(default_value),
  ) else {
    return (false, None);
  };
  let mut out_input: *mut c_char = std::ptr::null_mut();
  let want_input = dialog_type == LAUFEY_DIALOG_PROMPT;
  // SAFETY: All pointers are valid for the duration of the call. The
  // backend may write a heap-allocated string into `out_input`; we hand it
  // back to the backend's deallocator below.
  let confirmed = unsafe {
    f(
      api.backend_data,
      window_id,
      dialog_type as c_int,
      c_title.as_ptr(),
      c_message.as_ptr(),
      c_default.as_ptr(),
      if want_input {
        &mut out_input as *mut *mut c_char
      } else {
        std::ptr::null_mut()
      },
    )
  } != 0;
  let input = if !out_input.is_null() {
    // SAFETY: backend just wrote a NUL-terminated UTF-8 string here.
    let s = unsafe { CStr::from_ptr(out_input) }
      .to_string_lossy()
      .into_owned();
    if let Some(free) = api.string_free {
      // SAFETY: pointer originated from `f`, freed via the matching
      // backend allocator.
      unsafe { free(api.backend_data, out_input) };
    }
    Some(s)
  } else {
    None
  };
  (confirmed, input)
}

/// Read the system clipboard's plain-text content.
///
/// Returns `None` if the clipboard is empty, holds no text representation, or
/// the backend does not support clipboard access. Mirrors the web
/// `navigator.clipboard.readText()` API. Any thread on API 39 backends (they
/// hop to their UI thread where the platform needs it); older backends
/// expected the UI thread.
pub fn read_clipboard_text() -> Option<String> {
  let api = api();
  let f = api.read_clipboard_text?;
  // SAFETY: backend returns either NULL or a heap-allocated NUL-terminated
  // UTF-8 string owned by us; we copy it out and hand the pointer back to the
  // backend's matching deallocator (`string_free`).
  let ptr = unsafe { f(api.backend_data) };
  if ptr.is_null() {
    return None;
  }
  let text = unsafe { CStr::from_ptr(ptr) }
    .to_string_lossy()
    .into_owned();
  if let Some(free) = api.string_free {
    unsafe { free(api.backend_data, ptr) };
  }
  Some(text)
}

/// Replace the system clipboard's content with `text`.
///
/// Passing an empty string clears the clipboard. Mirrors the web
/// `navigator.clipboard.writeText()` API. No-op if the backend does not
/// support clipboard access, or if `text` contains a NUL byte (which cannot
/// cross the C ABI). Any thread on API 39 backends.
pub fn write_clipboard_text(text: &str) {
  let api = api();
  if let (Some(f), Ok(c_text)) = (api.write_clipboard_text, CString::new(text))
  {
    // SAFETY: `c_text` outlives the call; the backend copies the bytes.
    unsafe { f(api.backend_data, c_text.as_ptr()) };
  }
}

/// Test-only. Synthesizes a click on the menu/tray item with the given id by
/// invoking the same click-dispatch path a real click uses. Returns `true` if
/// an item with that id was registered (via [`Window::set_menu`],
/// [`TrayIcon::set_menu`], etc.) and its handler ran, `false` if not found or
/// the backend does not implement the test hook (API < 30).
///
/// Intended for automated e2e tests to exercise click round-trips without OS
/// input injection. See `examples/native_e2e` and `docs/e2e-testing.md`.
pub fn test_click_menu_item(item_id: &str) -> bool {
  let api = api();
  let Some(f) = api.test_click_menu_item else {
    return false;
  };
  let Ok(c_id) = CString::new(item_id) else {
    return false;
  };
  // SAFETY: `c_id` outlives the call; the backend only reads the string.
  unsafe { f(api.backend_data, c_id.as_ptr()) }
}

/// Test-only. Synthesizes a close-requested event on `window_id` through the
/// same dispatch code a real OS close click runs. Returns `true` if an
/// [`Window::on_close_requested`] handler deferred the close (the window is
/// still open), `false` if the close proceeded (no handler was registered,
/// or the backend does not implement the test hook -- indistinguishable
/// cases). "Proceeded" means the close was initiated, not necessarily
/// completed: some backends (winit, CEF) queue the actual close, so poll
/// window state rather than asserting immediately after a `false` return.
///
/// Two deliberate differences from a real close click: the handler runs
/// synchronously on the *calling* thread, not the backend UI thread, and
/// `window_id` is not validated -- an unknown id still reaches a registered
/// handler.
///
/// Intended for automated e2e tests. See `examples/native_e2e` and
/// `docs/e2e-testing.md`.
pub fn test_trigger_close_requested(window_id: u32) -> bool {
  let api = api();
  let Some(f) = api.test_trigger_close_requested else {
    return false;
  };
  unsafe { f(api.backend_data, window_id) }
}

/// Test-only. Synthesizes a deep-link delivery of `url` through the same
/// dispatch path a real OS-routed URL takes, buffer included: called before
/// any [`on_open_url`] handler is registered, the URL is replayed on
/// registration exactly like a cold-start link. Returns `true` if a handler
/// consumed it, `false` if it was buffered — or if the backend does not
/// implement the test hook (API < 35, or any non-macOS backend).
///
/// Intended for automated e2e tests, so a deep-link round-trip can be covered
/// without registering a URL scheme with the OS. See `examples/native_e2e`
/// and `docs/e2e-testing.md`.
pub fn test_trigger_open_url(url: &str) -> bool {
  let api = api();
  let Some(f) = api.test_trigger_open_url else {
    return false;
  };
  let Ok(c_url) = CString::new(url) else {
    return false;
  };
  // SAFETY: `c_url` outlives the call; the backend only reads the string.
  unsafe { f(api.backend_data, c_url.as_ptr()) }
}

pub const LAUFEY_TEST_INPUT_KEY: i32 = 0;
pub const LAUFEY_TEST_INPUT_MOUSE_MOVE: i32 = 1;
pub const LAUFEY_TEST_INPUT_MOUSE_BUTTON: i32 = 2;
pub const LAUFEY_TEST_INPUT_WHEEL: i32 = 3;
pub const LAUFEY_TEST_INPUT_CURSOR_ENTER: i32 = 4;
pub const LAUFEY_TEST_INPUT_CURSOR_LEAVE: i32 = 5;
pub const LAUFEY_TEST_INPUT_MODIFIERS: i32 = 6;

/// A synthetic input event for [`test_inject_input`].
#[derive(Clone, Debug)]
pub enum TestInput {
  Key {
    key: String,
    code: String,
    pressed: bool,
    repeat: bool,
    modifiers: u32,
  },
  MouseMove {
    x: f64,
    y: f64,
    modifiers: u32,
  },
  MouseButton {
    button: i32,
    pressed: bool,
    x: f64,
    y: f64,
    modifiers: u32,
  },
  Wheel {
    delta_x: f64,
    delta_y: f64,
    delta_mode: i32,
    x: f64,
    y: f64,
    modifiers: u32,
  },
  CursorEnter {
    x: f64,
    y: f64,
    modifiers: u32,
  },
  CursorLeave {
    x: f64,
    y: f64,
    modifiers: u32,
  },
  Modifiers {
    modifiers: u32,
  },
}

/// Test-only. Posts `event` through the same dispatch a real OS event uses.
/// Returns `false` if the backend has no hook, the window is unknown (winit),
/// or the event was rejected (unknown kind / modifier sent as `Key`).
///
/// Wheel deltas are DOM-signed (positive Y is scroll down).
pub fn test_inject_input(window_id: u32, event: &TestInput) -> bool {
  let api = api();
  let Some(f) = api.test_inject_input else {
    return false;
  };
  let mut key = None;
  let mut code = None;
  let mut raw = ffi::laufey_test_input {
    kind: 0,
    modifiers: 0,
    key: std::ptr::null(),
    code: std::ptr::null(),
    pressed: false,
    repeat: false,
    button: 0,
    x: 0.0,
    y: 0.0,
    delta_x: 0.0,
    delta_y: 0.0,
    delta_mode: 0,
  };
  match event {
    TestInput::Key {
      key: k,
      code: c,
      pressed,
      repeat,
      modifiers,
    } => {
      raw.kind = LAUFEY_TEST_INPUT_KEY;
      raw.modifiers = *modifiers;
      raw.pressed = *pressed;
      raw.repeat = *repeat;
      key = CString::new(k.as_str()).ok();
      code = CString::new(c.as_str()).ok();
      raw.key = key.as_ref().map(|s| s.as_ptr()).unwrap_or(std::ptr::null());
      raw.code = code
        .as_ref()
        .map(|s| s.as_ptr())
        .unwrap_or(std::ptr::null());
    }
    TestInput::MouseMove { x, y, modifiers } => {
      raw.kind = LAUFEY_TEST_INPUT_MOUSE_MOVE;
      raw.modifiers = *modifiers;
      raw.x = *x;
      raw.y = *y;
    }
    TestInput::MouseButton {
      button,
      pressed,
      x,
      y,
      modifiers,
    } => {
      raw.kind = LAUFEY_TEST_INPUT_MOUSE_BUTTON;
      raw.modifiers = *modifiers;
      raw.pressed = *pressed;
      raw.button = *button;
      raw.x = *x;
      raw.y = *y;
    }
    TestInput::Wheel {
      delta_x,
      delta_y,
      delta_mode,
      x,
      y,
      modifiers,
    } => {
      raw.kind = LAUFEY_TEST_INPUT_WHEEL;
      raw.modifiers = *modifiers;
      raw.delta_x = *delta_x;
      raw.delta_y = *delta_y;
      raw.delta_mode = *delta_mode;
      raw.x = *x;
      raw.y = *y;
    }
    TestInput::CursorEnter { x, y, modifiers } => {
      raw.kind = LAUFEY_TEST_INPUT_CURSOR_ENTER;
      raw.modifiers = *modifiers;
      raw.x = *x;
      raw.y = *y;
    }
    TestInput::CursorLeave { x, y, modifiers } => {
      raw.kind = LAUFEY_TEST_INPUT_CURSOR_LEAVE;
      raw.modifiers = *modifiers;
      raw.x = *x;
      raw.y = *y;
    }
    TestInput::Modifiers { modifiers } => {
      raw.kind = LAUFEY_TEST_INPUT_MODIFIERS;
      raw.modifiers = *modifiers;
    }
  }
  let _ = (&key, &code);
  unsafe { f(api.backend_data, window_id, &raw) }
}

/// A menu item in an application menu template.
#[derive(Clone, Debug)]
pub enum MenuItem {
  /// A regular menu item with label and optional properties.
  Item {
    label: String,
    id: Option<String>,
    accelerator: Option<String>,
    enabled: bool,
    /// Checkmark next to the item (`NSMenuItem.state` / `MFS_CHECKED` /
    /// `GtkCheckMenuItem`). All platforms.
    checked: bool,
    /// Item icon: PNG-encoded image bytes, matching the tray and
    /// notification icon APIs. macOS and Windows (Linux unsupported —
    /// GtkMenuItem has no image slot). On macOS a monochrome black+alpha
    /// PNG is rendered as a template so it tints to white on selection;
    /// Windows renders the image as-is.
    icon: Option<Vec<u8>>,
    /// Tooltip shown on hover. macOS only (matches Electron's `toolTip`).
    tooltip: Option<String>,
  },
  /// A submenu containing child items.
  Submenu { label: String, items: Vec<MenuItem> },
  /// A separator line.
  Separator,
  /// A standard role-based item (quit, copy, paste, etc.)
  Role { role: String },
}

impl MenuItem {
  fn to_value(&self) -> Value {
    match self {
      MenuItem::Item {
        label,
        id,
        accelerator,
        enabled,
        checked,
        icon,
        tooltip,
      } => {
        let mut dict = HashMap::new();
        dict.insert("label".to_string(), Value::String(label.clone()));
        if let Some(id) = id {
          dict.insert("id".to_string(), Value::String(id.clone()));
        }
        if let Some(accel) = accelerator {
          dict.insert("accelerator".to_string(), Value::String(accel.clone()));
        }
        if !enabled {
          dict.insert("enabled".to_string(), Value::Bool(false));
        }
        if *checked {
          dict.insert("checked".to_string(), Value::Bool(true));
        }
        if let Some(icon) = icon {
          dict.insert("icon".to_string(), Value::Binary(icon.clone()));
        }
        if let Some(tooltip) = tooltip {
          dict.insert("tooltip".to_string(), Value::String(tooltip.clone()));
        }
        Value::Dict(dict)
      }
      MenuItem::Submenu { label, items } => {
        let mut dict = HashMap::new();
        dict.insert("label".to_string(), Value::String(label.clone()));
        dict.insert(
          "submenu".to_string(),
          Value::List(items.iter().map(|i| i.to_value()).collect()),
        );
        Value::Dict(dict)
      }
      MenuItem::Separator => {
        let mut dict = HashMap::new();
        dict.insert("type".to_string(), Value::String("separator".to_string()));
        Value::Dict(dict)
      }
      MenuItem::Role { role } => {
        let mut dict = HashMap::new();
        dict.insert("role".to_string(), Value::String(role.clone()));
        Value::Dict(dict)
      }
    }
  }
}

unsafe extern "C" fn menu_click_callback(
  _user_data: *mut c_void,
  window_id: u32,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = CStr::from_ptr(item_id).to_string_lossy();
  // Cloned out: the handler runs without the lock held (it may replace
  // itself).
  let handler = menu_click_handlers()
    .lock()
    .unwrap()
    .get(&window_id)
    .cloned();
  if let Some(handler) = handler {
    handler(&id);
  }
}

fn menu_click_handlers(
) -> &'static Mutex<HashMap<u32, Arc<dyn Fn(&str) + Send + Sync>>> {
  MENU_CLICK_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

unsafe extern "C" fn context_menu_click_callback(
  _user_data: *mut c_void,
  window_id: u32,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = CStr::from_ptr(item_id).to_string_lossy();
  // Cloned out: the handler runs without the lock held (it may replace
  // itself).
  let handler = context_menu_handlers()
    .lock()
    .unwrap()
    .get(&window_id)
    .cloned();
  if let Some(handler) = handler {
    handler(&id);
  }
}

fn context_menu_handlers(
) -> &'static Mutex<HashMap<u32, Arc<dyn Fn(&str) + Send + Sync>>> {
  CONTEXT_MENU_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

// --- Dock / taskbar ---

/// How urgently to request the user's attention when calling [`bounce_dock`].
///
/// On macOS, `Informational` triggers a single bounce and `Critical` bounces
/// continuously until the app is focused. Behavior on Windows/Linux is the
/// closest native analog (`FlashWindowEx` / urgency hint).
#[derive(Clone, Copy, Debug)]
pub enum DockBounceType {
  Informational,
  Critical,
}

fn dock_menu_handler() -> &'static Mutex<Option<Arc<dyn Fn(&str) + Send + Sync>>>
{
  DOCK_MENU_HANDLER.get_or_init(|| Mutex::new(None))
}

fn dock_reopen_handler(
) -> &'static Mutex<Option<Arc<dyn Fn(bool) + Send + Sync>>> {
  DOCK_REOPEN_HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn dock_menu_click_callback(
  _user_data: *mut c_void,
  _window_id: u32,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = CStr::from_ptr(item_id).to_string_lossy();
  // Cloned out: the handler runs without the lock held.
  let handler = dock_menu_handler().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(&id);
  }
}

unsafe extern "C" fn dock_reopen_callback(
  _user_data: *mut c_void,
  has_visible_windows: bool,
) {
  // Cloned out: the handler runs without the lock held.
  let handler = dock_reopen_handler().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(has_visible_windows);
  }
}

/// Set a short text badge on the app's dock icon (macOS) or taskbar icon
/// (Windows), or prefix the focused window's title with `"(text) "` (Linux).
/// Pass `None` or an empty string to clear the badge. Text containing a NUL
/// byte cannot cross the C ABI; the call is then a no-op.
pub fn set_dock_badge(text: Option<&str>) {
  let api = api();
  if let Some(f) = api.set_dock_badge {
    match text {
      Some(t) if !t.is_empty() => {
        if let Ok(c_text) = CString::new(t) {
          unsafe { f(api.backend_data, c_text.as_ptr()) };
        }
      }
      _ => unsafe { f(api.backend_data, std::ptr::null()) },
    }
  }
}

/// Bounce the dock icon (macOS), flash the focused window's taskbar button
/// (Windows), or set the urgency hint on the focused window (Linux).
pub fn bounce_dock(kind: DockBounceType) {
  let api = api();
  if let Some(f) = api.bounce_dock {
    let ty = match kind {
      DockBounceType::Informational => {
        ffi::LAUFEY_DOCK_BOUNCE_INFORMATIONAL as c_int
      }
      DockBounceType::Critical => ffi::LAUFEY_DOCK_BOUNCE_CRITICAL as c_int,
    };
    unsafe { f(api.backend_data, ty) };
  }
}

/// Set a custom right-click menu on the app's dock icon (macOS only).
/// `on_click` is called with the `id` of the clicked item.
/// Windows and Linux: no-op.
pub fn set_dock_menu<F>(template: &[MenuItem], on_click: F)
where
  F: Fn(&str) + Send + Sync + 'static,
{
  let value = Value::List(template.iter().map(|i| i.to_value()).collect());

  {
    let mut handler = dock_menu_handler().lock().unwrap();
    *handler = Some(Arc::new(on_click));
  }

  let api = api();
  if let Some(f) = api.set_dock_menu {
    let raw = value.to_raw();
    unsafe {
      f(
        api.backend_data,
        raw,
        Some(dock_menu_click_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

/// Remove the custom dock menu set by [`set_dock_menu`] (macOS only).
pub fn clear_dock_menu() {
  {
    let mut handler = dock_menu_handler().lock().unwrap();
    *handler = None;
  }

  let api = api();
  if let Some(f) = api.set_dock_menu {
    unsafe {
      f(
        api.backend_data,
        std::ptr::null_mut(),
        None,
        std::ptr::null_mut(),
      );
    }
  }
}

/// Show or hide the app's dock icon (macOS activation policy).
/// Windows and Linux: no-op (no app-level equivalent).
pub fn set_dock_visible(visible: bool) {
  let api = api();
  if let Some(f) = api.set_dock_visible {
    unsafe { f(api.backend_data, visible) };
  }
}

/// Register a callback invoked when the user clicks the dock icon while the
/// app has no visible windows (macOS only). The callback receives whether
/// any windows are currently visible.
///
/// The default "show last hidden window" behavior is always swallowed — the
/// callback is purely informational; user code decides what (if anything) to
/// do (e.g. call `window.show()`).
///
/// Windows and Linux: no-op (no equivalent event).
pub fn on_dock_reopen<F>(handler: F)
where
  F: Fn(bool) + Send + Sync + 'static,
{
  {
    let mut slot = dock_reopen_handler().lock().unwrap();
    *slot = Some(Arc::new(handler));
  }

  let api = api();
  if let Some(f) = api.set_dock_reopen_handler {
    unsafe {
      f(
        api.backend_data,
        Some(dock_reopen_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

// --- Deep links / custom URL schemes ---

fn open_url_handler() -> &'static Mutex<Option<Arc<dyn Fn(&str) + Send + Sync>>>
{
  OPEN_URL_HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn open_url_callback(
  _user_data: *mut c_void,
  url: *const c_char,
) {
  if url.is_null() {
    return;
  }
  // The OS is the source here, so don't assume well-formed UTF-8.
  let url = CStr::from_ptr(url).to_string_lossy();
  // Cloned out: the handler runs without the lock held.
  let handler = open_url_handler().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(&url);
  }
}

/// Register a callback invoked when the OS routes a custom URL scheme this app
/// has registered — `acme://open/document/42` — to the app, either at launch
/// or while it is already running.
///
/// Registering the scheme with the OS is *not* laufey's job: the embedder
/// declares it in the bundle it ships (macOS `CFBundleURLTypes`, Linux
/// `.desktop` `x-scheme-handler/<scheme>`, Windows
/// `HKCU\Software\Classes\<scheme>`). See `docs/deep-links.md`.
///
/// URLs that arrive before this is called — which a launch URL always does,
/// since the runtime is still coming up — are buffered by the backend and
/// delivered as soon as the handler is registered.
///
/// The URL is whatever the OS handed over, unvalidated: check the scheme
/// against the ones you registered before acting on it.
///
/// macOS only, for the same reason as [`on_dock_reopen`]: AppKit delivers the
/// URL to the running app as an Apple Event, so one process handles every
/// link. Windows and Linux spawn a new process with the URL in argv instead,
/// which needs a single-instance lock and an app identity that only the
/// embedder has — read `std::env::args` there. No-op on those platforms.
pub fn on_open_url<F>(handler: F)
where
  F: Fn(&str) + Send + Sync + 'static,
{
  // Install the Rust-side handler first: the backend flushes buffered URLs
  // synchronously inside the call below, and they'd be dropped if the slot
  // were still empty.
  {
    let mut slot = open_url_handler().lock().unwrap();
    *slot = Some(Arc::new(handler));
  }

  let api = api();
  if let Some(f) = api.set_open_url_handler {
    unsafe {
      f(
        api.backend_data,
        Some(open_url_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

// --- Single instance ---

fn second_instance_handler() -> &'static Mutex<Option<SecondInstanceHandler>> {
  SECOND_INSTANCE_HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn second_instance_callback(
  _user_data: *mut c_void,
  argv: *const *const c_char,
  argc: usize,
  cwd: *const c_char,
) {
  // The backend validated the strings as UTF-8; convert lossily anyway, the
  // source is another process.
  let mut args = Vec::with_capacity(argc);
  if !argv.is_null() {
    for i in 0..argc {
      let arg = *argv.add(i);
      if !arg.is_null() {
        args.push(CStr::from_ptr(arg).to_string_lossy().into_owned());
      }
    }
  }
  let cwd = if cwd.is_null() {
    String::new()
  } else {
    CStr::from_ptr(cwd).to_string_lossy().into_owned()
  };
  // Cloned out: the handler runs without the lock held.
  let handler = second_instance_handler().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(&args, &cwd);
  }
}

/// Register a callback invoked in the running instance when the app is
/// launched again, with the new launch's arguments (after the executable
/// name) and working directory — like Electron's `second-instance` event.
///
/// Single-instance mode is opt-in and decided by the backend before the
/// runtime loads: `"singleInstance": true` in `laufey-launch.json` (or
/// `LAUFEY_SINGLE_INSTANCE=1`) together with an app id (`"appId"` /
/// `LAUFEY_APP_ID`). The second launch then forwards its arguments to this
/// process and exits without starting; this process brings its window to the
/// front and calls `handler` on the UI thread. Launches that arrive before
/// the handler is registered are buffered and delivered when it is.
///
/// This is how a deep link or a file reaches an already-running app on
/// Windows and Linux (the OS starts `app "<url>"`); on macOS, LaunchServices
/// uses [`on_open_url`] instead. laufey doesn't interpret the arguments: they
/// come from another process of the same user, so treat them as untrusted
/// input, as you would your own `std::env::args()` at a cold start.
///
/// No-op on backends without single-instance support (Winit) and on
/// backends older than API 36. See `docs/deep-links.md`.
pub fn on_second_instance<F>(handler: F)
where
  F: Fn(&[String], &str) + Send + Sync + 'static,
{
  // As in on_open_url: install first, the backend flushes buffered launches
  // synchronously inside the call below.
  {
    let mut slot = second_instance_handler().lock().unwrap();
    *slot = Some(Arc::new(handler));
  }

  let api = api();
  if let Some(f) = api.set_second_instance_handler {
    unsafe {
      f(
        api.backend_data,
        Some(second_instance_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

// --- Passkeys ---

/// `passkey_request` kind: a registration (`navigator.credentials.create`).
/// Mirrors `LAUFEY_PASSKEY_CREATE` in `laufey.h`.
pub const LAUFEY_PASSKEY_CREATE: u32 = 0;
/// `passkey_request` kind: an authentication (`navigator.credentials.get`).
/// Mirrors `LAUFEY_PASSKEY_GET` in `laufey.h`.
pub const LAUFEY_PASSKEY_GET: u32 = 1;
/// Capability flag: a platform authenticator (Touch ID / iCloud Keychain,
/// Windows Hello) can serve requests.
pub const LAUFEY_PASSKEY_PLATFORM_AUTHENTICATOR: u32 = 1 << 0;
/// Capability flag: roaming security keys can serve requests.
pub const LAUFEY_PASSKEY_SECURITY_KEYS: u32 = 1 << 1;

/// What [`passkey_create`] / [`passkey_get`] can use right now — the shape of
/// `@clerk/electron-passkeys`' `capabilities()`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct PasskeyCapabilities {
  pub platform_authenticator: bool,
  pub security_keys: bool,
}

/// The passkey capabilities of this backend: both on macOS 12+, security keys
/// (and Windows Hello when set up) on Windows 10 1903+, none on Linux, on the
/// Winit backend and on backends older than API 37. Any thread.
pub fn passkey_capabilities() -> PasskeyCapabilities {
  passkey_capabilities_with(api())
}

fn passkey_capabilities_with(api: &LaufeyBackendApi) -> PasskeyCapabilities {
  let flags = match api.passkey_capabilities {
    Some(f) => unsafe { f(api.backend_data) },
    None => 0,
  };
  PasskeyCapabilities {
    platform_authenticator: flags & LAUFEY_PASSKEY_PLATFORM_AUTHENTICATOR != 0,
    security_keys: flags & LAUFEY_PASSKEY_SECURITY_KEYS != 0,
  }
}

/// Run a WebAuthn registration ceremony through the OS platform
/// authenticator.
///
/// `options_json` is `PublicKeyCredentialCreationOptions` as JSON with
/// base64url binary fields — exactly what `@clerk/electron` sends to
/// `@clerk/electron-passkeys` — and the future resolves with its JSON
/// envelope, `{"ok":true,"credential":{...}}` or
/// `{"ok":false,"error":{"code","message"}}` with `code` one of `cancelled`,
/// `invalid_rp`, `not_supported`, `timeout`, `unknown`. It never fails
/// otherwise. `window_id` anchors the OS sheet / dialog (0: the focused
/// window).
///
/// The options are untrusted input as far as the backend is concerned: it
/// parses them strictly and lets the OS decide whether the app may use the
/// RP ID (macOS: the `webcredentials:` associated domain). Only call this from
/// the app's own trusted code. One ceremony runs at a time per app; another
/// request meanwhile resolves with `unknown` ("a passkey request is already in
/// progress").
///
/// The request is made when this function is called, not when the future is
/// first polled. See `docs/passkeys.md`.
pub fn passkey_create(
  window_id: u32,
  options_json: &str,
) -> impl Future<Output = String> + Send + 'static {
  passkey_request_with(api(), window_id, LAUFEY_PASSKEY_CREATE, options_json)
}

/// Run a WebAuthn authentication ceremony through the OS platform
/// authenticator. `options_json` is `PublicKeyCredentialRequestOptions` as
/// JSON with base64url binary fields; otherwise as [`passkey_create`].
pub fn passkey_get(
  window_id: u32,
  options_json: &str,
) -> impl Future<Output = String> + Send + 'static {
  passkey_request_with(api(), window_id, LAUFEY_PASSKEY_GET, options_json)
}

/// The error envelope the backends write, for the answers the capi gives
/// itself. `message` must not need JSON escaping.
fn passkey_error_envelope(code: &str, message: &str) -> String {
  format!(r#"{{"ok":false,"error":{{"code":"{code}","message":"{message}"}}}}"#)
}

unsafe extern "C" fn passkey_result_trampoline(
  user_data: *mut c_void,
  result_json: *const c_char,
) {
  // The backend calls this exactly once per request (laufey.h), so the box
  // is reclaimed exactly once.
  let tx =
    Box::from_raw(user_data as *mut tokio::sync::oneshot::Sender<String>);
  let result = if result_json.is_null() {
    passkey_error_envelope("unknown", "the backend returned no result")
  } else {
    CStr::from_ptr(result_json).to_string_lossy().into_owned()
  };
  // The receiver may be gone (the future was dropped); nothing to do then.
  let _ = tx.send(result);
}

fn passkey_request_with(
  api: &LaufeyBackendApi,
  window_id: u32,
  kind: u32,
  options_json: &str,
) -> impl Future<Output = String> + Send + 'static {
  let pending: Result<tokio::sync::oneshot::Receiver<String>, String> =
    match (api.passkey_request, CString::new(options_json)) {
      (None, _) => Err(passkey_error_envelope(
        "not_supported",
        "Native passkeys are not supported by this backend.",
      )),
      (Some(_), Err(_)) => Err(passkey_error_envelope(
        "unknown",
        "invalid passkey options: contains a NUL byte",
      )),
      (Some(f), Ok(options)) => {
        let (tx, rx) = tokio::sync::oneshot::channel::<String>();
        let user_data = Box::into_raw(Box::new(tx)) as *mut c_void;
        unsafe {
          f(
            api.backend_data,
            window_id,
            kind,
            options.as_ptr(),
            Some(passkey_result_trampoline),
            user_data,
          );
        }
        Ok(rx)
      }
    };
  async move {
    match pending {
      Err(envelope) => envelope,
      Ok(rx) => rx.await.unwrap_or_else(|_| {
        passkey_error_envelope(
          "unknown",
          "the passkey request ended without a result",
        )
      }),
    }
  }
}

// --- Tray / status-bar icon ---

fn tray_menu_handlers(
) -> &'static Mutex<HashMap<u32, Arc<dyn Fn(&str) + Send + Sync>>> {
  TRAY_MENU_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn tray_click_handlers(
) -> &'static Mutex<HashMap<u32, Arc<dyn Fn() + Send + Sync>>> {
  TRAY_CLICK_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn tray_dblclick_handlers(
) -> &'static Mutex<HashMap<u32, Arc<dyn Fn() + Send + Sync>>> {
  TRAY_DBLCLICK_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

unsafe extern "C" fn tray_menu_click_callback(
  _user_data: *mut c_void,
  tray_id: u32,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = CStr::from_ptr(item_id).to_string_lossy();
  // Cloned out: the handler runs without the lock held (it may replace
  // itself).
  let handler = tray_menu_handlers().lock().unwrap().get(&tray_id).cloned();
  if let Some(handler) = handler {
    handler(&id);
  }
}

unsafe extern "C" fn tray_click_callback(
  _user_data: *mut c_void,
  tray_id: u32,
) {
  // Cloned out: the handler runs without the lock held (it may replace
  // itself).
  let handler = tray_click_handlers().lock().unwrap().get(&tray_id).cloned();
  if let Some(handler) = handler {
    handler();
  }
}

unsafe extern "C" fn tray_dblclick_callback(
  _user_data: *mut c_void,
  tray_id: u32,
) {
  // Cloned out: the handler runs without the lock held (it may replace
  // itself).
  let handler = tray_dblclick_handlers()
    .lock()
    .unwrap()
    .get(&tray_id)
    .cloned();
  if let Some(handler) = handler {
    handler();
  }
}

/// A persistent icon in the OS status area (macOS menu bar extras, Windows
/// system tray, Linux AppIndicator). Multiple icons may be created.
///
/// The native icon is destroyed when the `TrayIcon` is dropped. Clone via
/// [`TrayIcon::id`] + [`TrayIcon::from_id`] if you need multiple handles.
pub struct TrayIcon {
  id: u32,
  owned: bool,
}

impl TrayIcon {
  /// Create a new tray icon. Returns a `TrayIcon` with `id == 0` if the
  /// backend doesn't support tray icons (e.g. CEF on Linux).
  pub fn new() -> Self {
    let api = api();
    let id = if let Some(f) = api.create_tray_icon {
      unsafe { f(api.backend_data) }
    } else {
      0
    };
    TrayIcon { id, owned: true }
  }

  /// Wrap an existing tray id (doesn't create a new native icon; doesn't
  /// destroy on drop).
  pub fn from_id(id: u32) -> Self {
    TrayIcon { id, owned: false }
  }

  pub fn id(&self) -> u32 {
    self.id
  }

  /// The tray icon's bounding rectangle `(x, y, width, height)` in screen
  /// coordinates, in the same top-left-origin space as
  /// [`Window::set_position`], or `None` if the position isn't known yet or
  /// the backend/platform can't report it. Use this to anchor a popover
  /// window under the icon.
  pub fn get_bounds(&self) -> Option<(i32, i32, i32, i32)> {
    let api = api();
    let f = api.get_tray_icon_bounds?;
    let mut x: c_int = 0;
    let mut y: c_int = 0;
    let mut width: c_int = 0;
    let mut height: c_int = 0;
    let ok = unsafe {
      f(
        api.backend_data,
        self.id,
        &mut x,
        &mut y,
        &mut width,
        &mut height,
      )
    };
    if ok {
      Some((x, y, width, height))
    } else {
      None
    }
  }

  /// Set the icon image from PNG-encoded bytes.
  pub fn icon(self, png_bytes: &[u8]) -> Self {
    self.set_icon(png_bytes);
    self
  }

  /// Set the tooltip shown on hover.
  pub fn tooltip(self, text: &str) -> Self {
    self.set_tooltip(Some(text));
    self
  }

  /// Set the right-click context menu.
  pub fn menu<F>(self, template: &[MenuItem], on_click: F) -> Self
  where
    F: Fn(&str) + Send + Sync + 'static,
  {
    self.set_menu(template, on_click);
    self
  }

  /// Register a left-click handler. Right-click is reserved for the menu.
  pub fn on_click<F>(self, handler: F) -> Self
  where
    F: Fn() + Send + Sync + 'static,
  {
    {
      let mut handlers = tray_click_handlers().lock().unwrap();
      handlers.insert(self.id, Arc::new(handler));
    }
    let api = api();
    if let Some(f) = api.set_tray_click_handler {
      unsafe {
        f(
          api.backend_data,
          self.id,
          Some(tray_click_callback),
          std::ptr::null_mut(),
        );
      }
    }
    self
  }

  /// Register a left-double-click handler. Fires in addition to `on_click`
  /// for the first of the two clicks. No-op on Linux.
  pub fn on_double_click<F>(self, handler: F) -> Self
  where
    F: Fn() + Send + Sync + 'static,
  {
    self.set_double_click_handler(handler);
    self
  }

  /// Set the icon used when the OS is in dark mode. Cleared when passed an
  /// empty slice.
  pub fn icon_dark(self, png_bytes: &[u8]) -> Self {
    self.set_icon_dark(png_bytes);
    self
  }

  pub fn set_icon(&self, png_bytes: &[u8]) {
    if self.id == 0 {
      return;
    }
    let api = api();
    if let Some(f) = api.set_tray_icon {
      unsafe {
        f(
          api.backend_data,
          self.id,
          png_bytes.as_ptr() as *const c_void,
          png_bytes.len(),
        );
      }
    }
  }

  pub fn set_icon_dark(&self, png_bytes: &[u8]) {
    if self.id == 0 {
      return;
    }
    let api = api();
    if let Some(f) = api.set_tray_icon_dark {
      unsafe {
        f(
          api.backend_data,
          self.id,
          if png_bytes.is_empty() {
            std::ptr::null()
          } else {
            png_bytes.as_ptr() as *const c_void
          },
          png_bytes.len(),
        );
      }
    }
  }

  pub fn set_double_click_handler<F>(&self, handler: F)
  where
    F: Fn() + Send + Sync + 'static,
  {
    if self.id == 0 {
      return;
    }
    {
      let mut handlers = tray_dblclick_handlers().lock().unwrap();
      handlers.insert(self.id, Arc::new(handler));
    }
    let api = api();
    if let Some(f) = api.set_tray_double_click_handler {
      unsafe {
        f(
          api.backend_data,
          self.id,
          Some(tray_dblclick_callback),
          std::ptr::null_mut(),
        );
      }
    }
  }

  pub fn set_tooltip(&self, text: Option<&str>) {
    if self.id == 0 {
      return;
    }
    let api = api();
    if let Some(f) = api.set_tray_tooltip {
      match text {
        // Text with a NUL byte cannot cross the C ABI: a no-op.
        Some(t) if !t.is_empty() => {
          if let Ok(c_text) = CString::new(t) {
            unsafe { f(api.backend_data, self.id, c_text.as_ptr()) };
          }
        }
        _ => unsafe { f(api.backend_data, self.id, std::ptr::null()) },
      }
    }
  }

  pub fn set_menu<F>(&self, template: &[MenuItem], on_click: F)
  where
    F: Fn(&str) + Send + Sync + 'static,
  {
    if self.id == 0 {
      return;
    }
    let value = Value::List(template.iter().map(|i| i.to_value()).collect());
    {
      let mut handlers = tray_menu_handlers().lock().unwrap();
      handlers.insert(self.id, Arc::new(on_click));
    }
    let api = api();
    if let Some(f) = api.set_tray_menu {
      let raw = value.to_raw();
      unsafe {
        f(
          api.backend_data,
          self.id,
          raw,
          Some(tray_menu_click_callback),
          std::ptr::null_mut(),
        );
      }
    }
  }

  pub fn clear_menu(&self) {
    if self.id == 0 {
      return;
    }
    {
      let mut handlers = tray_menu_handlers().lock().unwrap();
      handlers.remove(&self.id);
    }
    let api = api();
    if let Some(f) = api.set_tray_menu {
      unsafe {
        f(
          api.backend_data,
          self.id,
          std::ptr::null_mut(),
          None,
          std::ptr::null_mut(),
        );
      }
    }
  }
}

impl Default for TrayIcon {
  fn default() -> Self {
    Self::new()
  }
}

impl Drop for TrayIcon {
  fn drop(&mut self) {
    if !self.owned || self.id == 0 {
      return;
    }
    // Clean up handler maps so we don't hold stale closures.
    if let Some(m) = TRAY_MENU_HANDLERS.get() {
      m.lock().unwrap().remove(&self.id);
    }
    if let Some(m) = TRAY_CLICK_HANDLERS.get() {
      m.lock().unwrap().remove(&self.id);
    }
    if let Some(m) = TRAY_DBLCLICK_HANDLERS.get() {
      m.lock().unwrap().remove(&self.id);
    }
    let api = api();
    if let Some(f) = api.destroy_tray_icon {
      unsafe { f(api.backend_data, self.id) };
    }
  }
}

/// Set the global JS namespace name for bindings (default: `"Laufey"`).
/// Must be called before creating any windows.
/// ```no_run
/// laufey::set_js_namespace("MyApp");
/// // JS code can now use: window.MyApp.greet("world")
/// ```
///
/// A `name` containing a NUL byte is no JS identifier; the call is then a
/// no-op.
pub fn set_js_namespace(name: &str) {
  let api = api();
  if let (Some(f), Ok(c_name)) = (api.set_js_namespace, CString::new(name)) {
    unsafe { f(api.backend_data, c_name.as_ptr()) };
  }
}

pub const LAUFEY_DIALOG_ALERT: i32 = 0;
pub const LAUFEY_DIALOG_CONFIRM: i32 = 1;
pub const LAUFEY_DIALOG_PROMPT: i32 = 2;

/// Show an alert dialog (app-wide, no parent window). Blocks until
/// dismissed; the platform's modal run loop pumps OS events while the
/// dialog is up so other LAUFEY windows continue to render and respond.
pub fn alert(title: &str, message: &str) {
  show_dialog_blocking(0, LAUFEY_DIALOG_ALERT, title, message, "");
}

/// Show a confirm dialog (app-wide). Returns `true` if OK was pressed.
/// Blocking semantics as `alert`.
pub fn confirm(title: &str, message: &str) -> bool {
  let (confirmed, _) =
    show_dialog_blocking(0, LAUFEY_DIALOG_CONFIRM, title, message, "");
  confirmed
}

/// Show a prompt dialog (app-wide). Returns `Some(text)` if OK, `None`
/// if cancelled. Blocking semantics as `alert`.
pub fn prompt(
  title: &str,
  message: &str,
  default_value: &str,
) -> Option<String> {
  let (confirmed, input) = show_dialog_blocking(
    0,
    LAUFEY_DIALOG_PROMPT,
    title,
    message,
    default_value,
  );
  if confirmed {
    input
  } else {
    None
  }
}

// --- Notifications ---

pub const LAUFEY_NOTIFICATION_SHOWN: i32 = 0;
pub const LAUFEY_NOTIFICATION_CLICKED: i32 = 1;
pub const LAUFEY_NOTIFICATION_CLOSED: i32 = 2;
pub const LAUFEY_NOTIFICATION_ACTION: i32 = 3;

/// What happened to a notification.
#[derive(Debug, Clone)]
pub enum NotificationEvent {
  /// The OS displayed the banner.
  Shown,
  /// The user clicked the notification body.
  Clicked,
  /// The user dismissed the notification or it expired.
  Closed,
  /// The user clicked an action button. The string is the action `id`.
  Action(String),
}

/// An action button on a notification.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NotificationAction {
  pub id: String,
  pub title: String,
}

/// Builder for a system notification. Mirrors a subset of the Web
/// Notifications API constructor options. Construct with
/// [`Notification::new`] / [`Notification::builder`], chain options, then
/// [`Notification::show`].
#[derive(Clone, Debug, Default)]
pub struct Notification {
  title: String,
  body: Option<String>,
  icon: Option<Vec<u8>>,
  tag: Option<String>,
  silent: Option<bool>,
  require_interaction: Option<bool>,
  actions: Vec<NotificationAction>,
  schedule_at_ms: Option<i64>,
  data: Option<String>,
}

/// Handle to a shown notification. Use [`NotificationHandle::close`] to
/// dismiss it programmatically. Dropping the handle does NOT close the
/// notification — they're fire-and-forget on the OS side.
#[derive(Debug, Clone, Copy)]
pub struct NotificationHandle {
  id: u32,
}

impl NotificationHandle {
  pub fn id(&self) -> u32 {
    self.id
  }

  pub fn close(&self) {
    if self.id == 0 {
      return;
    }
    let api = api();
    if let Some(f) = api.close_notification {
      unsafe { f(api.backend_data, self.id) };
    }
    if let Some(m) = NOTIFICATION_HANDLERS.get() {
      m.lock().unwrap().remove(&self.id);
    }
  }
}

impl Notification {
  /// Create a new notification with the given title (required field).
  pub fn new(title: impl Into<String>) -> Self {
    Self {
      title: title.into(),
      ..Default::default()
    }
  }

  /// Alias for [`Notification::new`].
  pub fn builder(title: impl Into<String>) -> Self {
    Self::new(title)
  }

  pub fn body(mut self, body: impl Into<String>) -> Self {
    self.body = Some(body.into());
    self
  }

  /// Set the notification icon (PNG bytes).
  pub fn icon(mut self, png_bytes: impl Into<Vec<u8>>) -> Self {
    self.icon = Some(png_bytes.into());
    self
  }

  /// Replace any existing notification with the same tag instead of
  /// stacking a new one (Web Notifications spec semantics).
  pub fn tag(mut self, tag: impl Into<String>) -> Self {
    self.tag = Some(tag.into());
    self
  }

  /// Suppress the system notification sound.
  pub fn silent(mut self, silent: bool) -> Self {
    self.silent = Some(silent);
    self
  }

  /// Keep the notification visible until the user dismisses it (instead
  /// of auto-expiring). Honored where the platform supports it.
  pub fn require_interaction(mut self, require: bool) -> Self {
    self.require_interaction = Some(require);
    self
  }

  /// Add an action button. Multiple calls add multiple buttons.
  /// Ignored on platforms that don't surface action buttons.
  pub fn action(
    mut self,
    id: impl Into<String>,
    title: impl Into<String>,
  ) -> Self {
    self.actions.push(NotificationAction {
      id: id.into(),
      title: title.into(),
    });
    self
  }

  /// Deliver the notification at `at` instead of now (API 41; see
  /// [`notification_capabilities`]). A scheduled notification is identified
  /// by its [`tag`](Notification::tag): give it one to cancel it
  /// ([`cancel_notification`]) or recognize its clicks
  /// ([`set_notification_response_handler`]); without one, `show` makes one
  /// up. A time in the past shows it now.
  pub fn schedule_at(self, at: std::time::SystemTime) -> Self {
    let ms = at
      .duration_since(std::time::UNIX_EPOCH)
      .map(|d| d.as_millis() as i64)
      .unwrap_or(0);
    self.schedule_at_ms(ms)
  }

  /// [`Notification::schedule_at`] with a Unix time in milliseconds.
  pub fn schedule_at_ms(mut self, unix_ms: i64) -> Self {
    self.schedule_at_ms = if unix_ms > 0 { Some(unix_ms) } else { None };
    self
  }

  /// Opaque data handed back with the notification's clicks (API 41), at
  /// most [`LAUFEY_NOTIFICATION_MAX_DATA_BYTES`].
  pub fn data(mut self, data: impl Into<String>) -> Self {
    self.data = Some(data.into());
    self
  }

  fn to_value(&self) -> Value {
    let mut dict = HashMap::new();
    dict.insert("title".to_string(), Value::String(self.title.clone()));
    if let Some(at) = self.schedule_at_ms {
      dict.insert("schedule_at".to_string(), Value::Double(at as f64));
      if self.tag.is_none() {
        dict.insert("tag".to_string(), Value::String(generated_tag()));
      }
    }
    if let Some(data) = &self.data {
      dict.insert("data".to_string(), Value::String(data.clone()));
    }
    if let Some(body) = &self.body {
      dict.insert("body".to_string(), Value::String(body.clone()));
    }
    if let Some(icon) = &self.icon {
      dict.insert("icon".to_string(), Value::Binary(icon.clone()));
    }
    if let Some(tag) = &self.tag {
      dict.insert("tag".to_string(), Value::String(tag.clone()));
    }
    if let Some(silent) = self.silent {
      dict.insert("silent".to_string(), Value::Bool(silent));
    }
    if let Some(require) = self.require_interaction {
      dict.insert("require_interaction".to_string(), Value::Bool(require));
    }
    if !self.actions.is_empty() {
      let actions = self
        .actions
        .iter()
        .map(|a| {
          let mut d = HashMap::new();
          d.insert("id".to_string(), Value::String(a.id.clone()));
          d.insert("title".to_string(), Value::String(a.title.clone()));
          Value::Dict(d)
        })
        .collect();
      dict.insert("actions".to_string(), Value::List(actions));
    }
    Value::Dict(dict)
  }

  /// Show the notification. Returns a handle that can be used to close
  /// it programmatically. Returns a handle with id 0 if the backend
  /// doesn't support notifications.
  pub fn show(self) -> NotificationHandle {
    self.show_with_handler(None)
  }

  /// Show the notification and register a callback for events
  /// (shown / clicked / closed / action).
  pub fn on_event<F>(self, handler: F) -> NotificationHandle
  where
    F: Fn(NotificationEvent) + Send + Sync + 'static,
  {
    self.show_with_handler(Some(Arc::new(move |_id, event| handler(event))))
  }

  /// [`Notification::on_event`] with the notification id passed to the
  /// callback, so an event that arrives before `on_event_with_id` returned
  /// (the shown event, on a backend thread) can still be attributed.
  pub fn on_event_with_id<F>(self, handler: F) -> NotificationHandle
  where
    F: Fn(u32, NotificationEvent) + Send + Sync + 'static,
  {
    self.show_with_handler(Some(Arc::new(handler)))
  }

  fn show_with_handler(
    self,
    handler: Option<NotificationHandler>,
  ) -> NotificationHandle {
    let api = api();
    let Some(show_fn) = api.show_notification else {
      return NotificationHandle { id: 0 };
    };
    let raw = self.to_value().to_raw();
    let (cb, user_data): (
      Option<unsafe extern "C" fn(*mut c_void, u32, c_int, *const c_char)>,
      *mut c_void,
    ) = if handler.is_some() {
      (Some(notification_event_callback), std::ptr::null_mut())
    } else {
      (None, std::ptr::null_mut())
    };
    let id = unsafe { show_fn(api.backend_data, raw, cb, user_data) };
    if id != 0 {
      if let Some(h) = handler {
        // An event can arrive (on a backend thread) before the handler is
        // in the map: the callback holds it in the pending map, under the
        // same lock, and it is delivered here.
        let early = {
          let mut state = notification_handlers().lock().unwrap();
          state.insert(id, h.clone());
          early_notification_events().lock().unwrap().remove(&id)
        };
        for event in early.unwrap_or_default() {
          let terminal = matches!(event, NotificationEvent::Closed);
          h(id, event);
          if terminal {
            notification_handlers().lock().unwrap().remove(&id);
          }
        }
      }
    }
    NotificationHandle { id }
  }
}

fn early_notification_events(
) -> &'static Mutex<HashMap<u32, Vec<NotificationEvent>>> {
  static EARLY: OnceLock<Mutex<HashMap<u32, Vec<NotificationEvent>>>> =
    OnceLock::new();
  EARLY.get_or_init(|| Mutex::new(HashMap::new()))
}

fn notification_handlers() -> &'static Mutex<HashMap<u32, NotificationHandler>>
{
  NOTIFICATION_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

unsafe extern "C" fn notification_event_callback(
  _user_data: *mut c_void,
  notification_id: u32,
  reason: c_int,
  action_id_or_null: *const c_char,
) {
  let event = match reason {
    LAUFEY_NOTIFICATION_SHOWN => NotificationEvent::Shown,
    LAUFEY_NOTIFICATION_CLICKED => NotificationEvent::Clicked,
    LAUFEY_NOTIFICATION_CLOSED => NotificationEvent::Closed,
    LAUFEY_NOTIFICATION_ACTION => {
      let id = if action_id_or_null.is_null() {
        String::new()
      } else {
        CStr::from_ptr(action_id_or_null)
          .to_string_lossy()
          .into_owned()
      };
      NotificationEvent::Action(id)
    }
    _ => return,
  };
  let is_terminal = matches!(event, NotificationEvent::Closed);
  // Clone the Arc out of the map so the handler runs without the lock
  // held — handlers may legitimately call back into the laufey API. An
  // event for a notification whose `show` hasn't returned yet (so its
  // handler isn't in the map) waits in the early-event map, decided under
  // the handlers lock so `show_with_handler` can't miss it.
  let handler = {
    let handlers = notification_handlers().lock().unwrap();
    match handlers.get(&notification_id).cloned() {
      Some(h) => Some(h),
      None => {
        let mut early = early_notification_events().lock().unwrap();
        // Bounded: ids that never get a handler (another caller's) can't
        // grow it without limit.
        if early.len() >= 256 && !early.contains_key(&notification_id) {
          if let Some(&oldest) = early.keys().min() {
            early.remove(&oldest);
          }
        }
        early
          .entry(notification_id)
          .or_default()
          .push(event.clone());
        None
      }
    }
  };
  if let Some(h) = handler {
    h(notification_id, event);
  } else {
    return;
  }
  if is_terminal {
    notification_handlers()
      .lock()
      .unwrap()
      .remove(&notification_id);
  }
}

// --- Permissions / runtime authorization ---

pub const LAUFEY_PERMISSION_INVALID: i32 = 0;
pub const LAUFEY_PERMISSION_NOTIFICATIONS: i32 = 1;
pub const LAUFEY_PERMISSION_NOTIFICATIONS_PROVISIONAL: i32 = 2;

pub const LAUFEY_PERMISSION_STATUS_GRANTED: i32 = 0;
pub const LAUFEY_PERMISSION_STATUS_DENIED: i32 = 1;
pub const LAUFEY_PERMISSION_STATUS_PROMPT: i32 = 2;
pub const LAUFEY_PERMISSION_STATUS_UNSUPPORTED: i32 = 3;

/// Capability for which authorization can be requested.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum PermissionKind {
  Notifications = LAUFEY_PERMISSION_NOTIFICATIONS,
  /// (API 41) Request only: quiet ("provisional") notification
  /// authorization, which macOS grants without a prompt; elsewhere the same
  /// as `Notifications`.
  NotificationsProvisional = LAUFEY_PERMISSION_NOTIFICATIONS_PROVISIONAL,
}

/// Result of [`request_permission`] / [`query_permission`]. Mirrors the
/// Web Permissions API state set with an extra `Unsupported` variant for
/// environments where the capability cannot be authorized at all (e.g.
/// an unbundled macOS process, or a backend that has no concept of the
/// kind on this platform).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum PermissionStatus {
  Granted = LAUFEY_PERMISSION_STATUS_GRANTED,
  Denied = LAUFEY_PERMISSION_STATUS_DENIED,
  Prompt = LAUFEY_PERMISSION_STATUS_PROMPT,
  Unsupported = LAUFEY_PERMISSION_STATUS_UNSUPPORTED,
}

impl PermissionStatus {
  fn from_raw(v: c_int) -> Self {
    match v {
      LAUFEY_PERMISSION_STATUS_GRANTED => Self::Granted,
      LAUFEY_PERMISSION_STATUS_DENIED => Self::Denied,
      LAUFEY_PERMISSION_STATUS_PROMPT => Self::Prompt,
      _ => Self::Unsupported,
    }
  }
}

unsafe extern "C" fn permission_trampoline(
  user_data: *mut c_void,
  status: c_int,
) {
  let cb =
    Box::from_raw(user_data as *mut Box<dyn FnOnce(PermissionStatus) + Send>);
  cb(PermissionStatus::from_raw(status));
}

fn dispatch_permission<F>(
  f: Option<
    unsafe extern "C" fn(
      *mut c_void,
      c_int,
      Option<unsafe extern "C" fn(*mut c_void, c_int)>,
      *mut c_void,
    ),
  >,
  kind: PermissionKind,
  callback: F,
) where
  F: FnOnce(PermissionStatus) + Send + 'static,
{
  let api = api();
  let Some(f) = f else {
    callback(PermissionStatus::Unsupported);
    return;
  };
  let boxed: Box<Box<dyn FnOnce(PermissionStatus) + Send>> =
    Box::new(Box::new(callback));
  let user_data = Box::into_raw(boxed) as *mut c_void;
  unsafe {
    f(
      api.backend_data,
      kind as c_int,
      Some(permission_trampoline),
      user_data,
    )
  };
}

/// Query the current authorization status of `kind` without prompting
/// the user. The callback runs on the UI thread.
pub fn query_permission<F>(kind: PermissionKind, callback: F)
where
  F: FnOnce(PermissionStatus) + Send + 'static,
{
  dispatch_permission(api().query_permission, kind, callback);
}

/// Request authorization for `kind`. If the current status is
/// [`PermissionStatus::Prompt`] the OS displays a system prompt;
/// otherwise the cached decision is returned without re-prompting (the
/// OS does not show a second prompt once the user has decided). The
/// callback runs on the UI thread.
pub fn request_permission<F>(kind: PermissionKind, callback: F)
where
  F: FnOnce(PermissionStatus) + Send + 'static,
{
  dispatch_permission(api().request_permission, kind, callback);
}

pub const LAUFEY_KEY_PRESSED: i32 = 0;
pub const LAUFEY_KEY_RELEASED: i32 = 1;

pub const LAUFEY_MOD_SHIFT: u32 = 1 << 0;
pub const LAUFEY_MOD_CONTROL: u32 = 1 << 1;
pub const LAUFEY_MOD_ALT: u32 = 1 << 2;
pub const LAUFEY_MOD_META: u32 = 1 << 3;

#[derive(Debug, Clone, Copy, Default)]
pub struct KeyModifiers {
  pub shift: bool,
  pub control: bool,
  pub alt: bool,
  pub meta: bool,
}

impl KeyModifiers {
  pub(crate) fn from_raw(flags: u32) -> Self {
    Self {
      shift: flags & LAUFEY_MOD_SHIFT != 0,
      control: flags & LAUFEY_MOD_CONTROL != 0,
      alt: flags & LAUFEY_MOD_ALT != 0,
      meta: flags & LAUFEY_MOD_META != 0,
    }
  }
}

#[macro_export]
macro_rules! main {
  ($main_fn:expr) => {
    #[no_mangle]
    /// # Safety
    /// `api` must be either null or a valid pointer to a `LaufeyBackendApi`
    /// with static lifetime supplied by the host runtime.
    pub unsafe extern "C" fn laufey_runtime_init(
      api: *const $crate::LaufeyBackendApi,
    ) -> std::ffi::c_int {
      unsafe { $crate::init_api(api) }
    }

    #[no_mangle]
    pub extern "C" fn laufey_runtime_start() -> std::ffi::c_int {
      let main_fn: fn() = $main_fn;
      main_fn();
      0
    }

    #[no_mangle]
    pub extern "C" fn laufey_runtime_shutdown() {
      $crate::shutdown();
    }
  };
}

#[cfg(test)]
mod tests {
  use super::*;

  // --- KeyModifiers ---

  #[test]
  fn key_modifiers_empty() {
    let m = KeyModifiers::from_raw(0);
    assert!(!m.shift && !m.control && !m.alt && !m.meta);
  }

  #[test]
  fn key_modifiers_single_flags() {
    assert!(KeyModifiers::from_raw(LAUFEY_MOD_SHIFT).shift);
    assert!(KeyModifiers::from_raw(LAUFEY_MOD_CONTROL).control);
    assert!(KeyModifiers::from_raw(LAUFEY_MOD_ALT).alt);
    assert!(KeyModifiers::from_raw(LAUFEY_MOD_META).meta);
  }

  #[test]
  fn key_modifiers_combinations() {
    // All four bits set.
    let all = KeyModifiers::from_raw(
      LAUFEY_MOD_SHIFT | LAUFEY_MOD_CONTROL | LAUFEY_MOD_ALT | LAUFEY_MOD_META,
    );
    assert!(all.shift && all.control && all.alt && all.meta);

    // Unknown high bits are ignored, known low bits still decode.
    let mixed = KeyModifiers::from_raw(LAUFEY_MOD_SHIFT | 0xF000_0000);
    assert!(mixed.shift && !mixed.control && !mixed.alt && !mixed.meta);
  }

  // --- PermissionStatus ---

  #[test]
  fn permission_status_from_raw_known() {
    assert_eq!(
      PermissionStatus::from_raw(LAUFEY_PERMISSION_STATUS_GRANTED),
      PermissionStatus::Granted
    );
    assert_eq!(
      PermissionStatus::from_raw(LAUFEY_PERMISSION_STATUS_DENIED),
      PermissionStatus::Denied
    );
    assert_eq!(
      PermissionStatus::from_raw(LAUFEY_PERMISSION_STATUS_PROMPT),
      PermissionStatus::Prompt
    );
    assert_eq!(
      PermissionStatus::from_raw(LAUFEY_PERMISSION_STATUS_UNSUPPORTED),
      PermissionStatus::Unsupported
    );
  }

  #[test]
  fn permission_status_from_raw_unknown_is_unsupported() {
    // Anything outside the LAUFEY_PERMISSION_STATUS_* range must map to
    // Unsupported so a future backend can't silently mean "Granted" by
    // returning, say, 99.
    assert_eq!(
      PermissionStatus::from_raw(99),
      PermissionStatus::Unsupported
    );
    assert_eq!(
      PermissionStatus::from_raw(-1),
      PermissionStatus::Unsupported
    );
  }

  // --- Value accessors ---

  #[test]
  fn value_accessors() {
    assert_eq!(Value::String("hi".into()).as_string(), Some("hi"));
    assert_eq!(Value::Int(42).as_int(), Some(42));
    assert_eq!(Value::Bool(true).as_bool(), Some(true));
    assert!(Value::List(vec![Value::Int(1)]).as_list().is_some());
    assert!(Value::Dict(HashMap::new()).as_dict().is_some());

    // Type mismatch must return None — these accessors are used in
    // binding handlers where the wrong type from JS is a soft error.
    assert!(Value::Int(1).as_string().is_none());
    assert!(Value::String("x".into()).as_int().is_none());
    assert!(Value::Null.as_bool().is_none());
  }

  // --- MenuItem::to_value ---

  fn dict_get<'a>(v: &'a Value, key: &str) -> Option<&'a Value> {
    v.as_dict().and_then(|d| d.get(key))
  }

  #[test]
  fn menu_item_to_value_item_minimal() {
    let item = MenuItem::Item {
      label: "Quit".into(),
      id: None,
      accelerator: None,
      enabled: true,
      checked: false,
      icon: None,
      tooltip: None,
    };
    let v = item.to_value();
    assert_eq!(
      dict_get(&v, "label").and_then(|v| v.as_string()),
      Some("Quit")
    );
    // Absent keys when their option is None / enabled is true (default).
    assert!(dict_get(&v, "id").is_none());
    assert!(dict_get(&v, "accelerator").is_none());
    assert!(dict_get(&v, "enabled").is_none());
  }

  #[test]
  fn menu_item_to_value_item_full() {
    let item = MenuItem::Item {
      label: "Open…".into(),
      id: Some("file.open".into()),
      accelerator: Some("CmdOrCtrl+O".into()),
      enabled: false,
      checked: true,
      icon: Some(vec![0x89, b'P', b'N', b'G']),
      tooltip: Some("Open a file".into()),
    };
    let v = item.to_value();
    assert_eq!(
      dict_get(&v, "id").and_then(|v| v.as_string()),
      Some("file.open")
    );
    assert_eq!(
      dict_get(&v, "accelerator").and_then(|v| v.as_string()),
      Some("CmdOrCtrl+O")
    );
    // enabled=false must serialize; enabled=true must NOT (the backend
    // defaults to enabled, so omitting it keeps the wire payload small).
    assert_eq!(
      dict_get(&v, "enabled").and_then(|v| v.as_bool()),
      Some(false)
    );
    // checked=true serializes; icon/tooltip serialize when Some.
    assert_eq!(
      dict_get(&v, "checked").and_then(|v| v.as_bool()),
      Some(true)
    );
    // Icon comes through as binary PNG bytes, matching the tray and
    // notification icon wire format.
    match dict_get(&v, "icon") {
      Some(Value::Binary(b)) => assert_eq!(b, &vec![0x89, b'P', b'N', b'G']),
      _ => panic!("icon must be Value::Binary"),
    }
    assert_eq!(
      dict_get(&v, "tooltip").and_then(|v| v.as_string()),
      Some("Open a file")
    );
  }

  #[test]
  fn menu_item_to_value_submenu_separator_role() {
    let sub = MenuItem::Submenu {
      label: "File".into(),
      items: vec![MenuItem::Separator],
    };
    let v = sub.to_value();
    assert_eq!(
      dict_get(&v, "label").and_then(|v| v.as_string()),
      Some("File")
    );
    let items = dict_get(&v, "submenu").and_then(|v| v.as_list()).unwrap();
    assert_eq!(items.len(), 1);
    assert_eq!(
      dict_get(&items[0], "type").and_then(|v| v.as_string()),
      Some("separator")
    );

    let role = MenuItem::Role {
      role: "copy".into(),
    };
    let rv = role.to_value();
    assert_eq!(
      dict_get(&rv, "role").and_then(|v| v.as_string()),
      Some("copy")
    );
  }

  // --- Notification::to_value ---

  #[test]
  fn notification_to_value_title_only() {
    let v = Notification::new("hi").to_value();
    assert_eq!(
      dict_get(&v, "title").and_then(|v| v.as_string()),
      Some("hi")
    );
    // Absent options should not appear.
    for k in [
      "body",
      "icon",
      "tag",
      "silent",
      "require_interaction",
      "actions",
    ] {
      assert!(
        dict_get(&v, k).is_none(),
        "key {k} should be absent when not set"
      );
    }
  }

  #[test]
  fn notification_to_value_full() {
    let v = Notification::new("t")
      .body("b")
      .icon(vec![1u8, 2, 3])
      .tag("g")
      .silent(true)
      .require_interaction(true)
      .action("ok", "OK")
      .action("dismiss", "Dismiss")
      .to_value();
    assert_eq!(dict_get(&v, "body").and_then(|v| v.as_string()), Some("b"));
    assert_eq!(dict_get(&v, "tag").and_then(|v| v.as_string()), Some("g"));
    assert_eq!(dict_get(&v, "silent").and_then(|v| v.as_bool()), Some(true));
    assert_eq!(
      dict_get(&v, "require_interaction").and_then(|v| v.as_bool()),
      Some(true)
    );
    let actions = dict_get(&v, "actions").and_then(|v| v.as_list()).unwrap();
    assert_eq!(actions.len(), 2);
    assert_eq!(
      dict_get(&actions[0], "id").and_then(|v| v.as_string()),
      Some("ok")
    );
    assert_eq!(
      dict_get(&actions[1], "title").and_then(|v| v.as_string()),
      Some("Dismiss")
    );
    // Icon comes through as binary, not string.
    match dict_get(&v, "icon") {
      Some(Value::Binary(b)) => assert_eq!(b, &vec![1u8, 2, 3]),
      _ => panic!("icon must be Value::Binary"),
    }
  }

  // --- Exhaustive KeyModifiers bit combinations ---

  #[test]
  fn key_modifiers_every_combination() {
    // Walk all 16 combinations of the 4 modifier flags. Catches a
    // regression where a flag mask was renumbered (e.g. ALT and META
    // swapped) — every other-numbered subset would still pass.
    for bits in 0..16u32 {
      let raw = (if bits & 1 != 0 { LAUFEY_MOD_SHIFT } else { 0 })
        | (if bits & 2 != 0 { LAUFEY_MOD_CONTROL } else { 0 })
        | (if bits & 4 != 0 { LAUFEY_MOD_ALT } else { 0 })
        | (if bits & 8 != 0 { LAUFEY_MOD_META } else { 0 });
      let m = KeyModifiers::from_raw(raw);
      assert_eq!(m.shift, bits & 1 != 0, "shift bit @ {bits:04b}");
      assert_eq!(m.control, bits & 2 != 0, "control bit @ {bits:04b}");
      assert_eq!(m.alt, bits & 4 != 0, "alt bit @ {bits:04b}");
      assert_eq!(m.meta, bits & 8 != 0, "meta bit @ {bits:04b}");
    }
  }

  // --- Value: cases not covered by value_accessors ---

  #[test]
  fn value_accessors_for_null_and_double_and_binary() {
    // Null doesn't satisfy any of as_string/as_int/as_bool/as_list/as_dict.
    let n = Value::Null;
    assert!(n.as_string().is_none());
    assert!(n.as_int().is_none());
    assert!(n.as_bool().is_none());
    assert!(n.as_list().is_none());
    assert!(n.as_dict().is_none());

    // Doubles aren't ints — feature parity with JS where 1.5 isn't a 1.
    assert!(Value::Double(1.5).as_int().is_none());

    // No public accessor for Binary, but pattern-matching still works
    // and the variant must round-trip through the public API surface
    // (it's how Notification icons cross the boundary).
    match Value::Binary(vec![0xDE, 0xAD]) {
      Value::Binary(b) => assert_eq!(b, vec![0xDE, 0xAD]),
      _ => unreachable!(),
    }
  }

  #[test]
  fn value_list_and_dict_nest_arbitrarily() {
    let inner = Value::Dict({
      let mut m = HashMap::new();
      m.insert("k".to_string(), Value::Int(1));
      m
    });
    let outer = Value::List(vec![Value::Null, inner]);
    let list = outer.as_list().unwrap();
    assert_eq!(list.len(), 2);
    assert!(matches!(list[0], Value::Null));
    let inner_dict = list[1].as_dict().unwrap();
    assert_eq!(inner_dict.get("k").and_then(|v| v.as_int()), Some(1));
  }

  // --- MenuItem: corner cases the basic tests didn't reach ---

  #[test]
  fn menu_item_role_does_not_carry_label_or_id() {
    // Role items are wholly defined by the role name. A regression that
    // started attaching `label` or `id` would let user code masquerade
    // as a role-bound system item.
    let v = MenuItem::Role {
      role: "quit".into(),
    }
    .to_value();
    let dict = v.as_dict().unwrap();
    assert!(dict.get("role").is_some());
    assert!(dict.get("label").is_none());
    assert!(dict.get("id").is_none());
    assert!(dict.get("submenu").is_none());
  }

  #[test]
  fn menu_item_nested_submenus_recursively_serialize() {
    let menu = MenuItem::Submenu {
      label: "File".into(),
      items: vec![MenuItem::Submenu {
        label: "Recent".into(),
        items: vec![MenuItem::Item {
          label: "Open project.toml".into(),
          id: Some("recent.0".into()),
          accelerator: None,
          enabled: true,
          checked: false,
          icon: None,
          tooltip: None,
        }],
      }],
    };
    let v = menu.to_value();
    let outer = v.as_dict().unwrap();
    let outer_items = outer.get("submenu").and_then(|v| v.as_list()).unwrap();
    assert_eq!(outer_items.len(), 1);
    let inner = outer_items[0]
      .as_dict()
      .expect("nested submenu must be dict");
    let inner_items = inner.get("submenu").and_then(|v| v.as_list()).unwrap();
    assert_eq!(inner_items.len(), 1);
    assert_eq!(
      inner_items[0]
        .as_dict()
        .and_then(|d| d.get("id"))
        .and_then(|v| v.as_string()),
      Some("recent.0")
    );
  }

  // --- Notification: argument boundary cases ---

  #[test]
  fn notification_empty_actions_list_is_omitted() {
    // Adding an `actions: []` key for a notification that opted out
    // would force the backend to allocate an empty list pointer on
    // every dispatch. The builder must elide it.
    let v = Notification::new("t").body("b").to_value();
    assert!(v.as_dict().unwrap().get("actions").is_none());
  }

  #[test]
  fn notification_partial_options_serialize_only_set_fields() {
    let v = Notification::new("t").body("b").silent(false).to_value();
    let d = v.as_dict().unwrap();
    assert_eq!(d.get("body").and_then(|v| v.as_string()), Some("b"));
    // silent=false is explicit, must serialize (the backend default
    // varies by platform).
    assert_eq!(d.get("silent").and_then(|v| v.as_bool()), Some(false));
    // Other keys remain absent.
    assert!(d.get("tag").is_none());
    assert!(d.get("require_interaction").is_none());
    assert!(d.get("icon").is_none());
    assert!(d.get("actions").is_none());
  }

  #[test]
  fn notification_handle_id_zero_is_noop_close() {
    // A zero-id handle indicates the backend didn't support
    // notifications. `close` on it must be a no-op (no panic, no
    // api() call). This is the failure mode for hello_runtime on CEF
    // Linux where notifications aren't wired up.
    let h = NotificationHandle { id: 0 };
    assert_eq!(h.id(), 0);
    // Doesn't touch api() because we never installed one.
    h.close();
  }

  // --- DockBounceType / PermissionKind: simple enum sanity ---

  #[test]
  fn window_options_map_to_flags() {
    // The flag bits cross the C ABI to create_window_ex, so a regression
    // that swapped or dropped a bit would silently mis-style windows.
    assert_eq!(WindowOptions::default().to_flags(), 0);
    assert_eq!(
      WindowOptions {
        frameless: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_FRAMELESS
    );
    assert_eq!(
      WindowOptions {
        no_activate: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_NO_ACTIVATE
    );
    assert_eq!(
      WindowOptions {
        transparent_titlebar: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR
    );
    assert_eq!(
      WindowOptions {
        transparent: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_TRANSPARENT
    );
    assert_eq!(
      WindowOptions {
        frameless: true,
        no_activate: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_FRAMELESS | LAUFEY_WINDOW_FLAG_NO_ACTIVATE
    );
    assert_eq!(
      WindowOptions {
        frameless: true,
        transparent: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_FRAMELESS | LAUFEY_WINDOW_FLAG_TRANSPARENT
    );
  }

  #[test]
  fn dock_bounce_type_is_copy_and_distinct() {
    // The enum is `#[derive(Copy)]` because it's an i32-shaped tag —
    // accidentally dropping Copy would silently force a move semantics
    // change on callers. Confirm copy + distinct discriminants.
    let a = DockBounceType::Informational;
    let b = a;
    let _ = a; // still usable after copy
    let c = DockBounceType::Critical;
    // We can't compare without PartialEq, but matches! works.
    assert!(matches!(a, DockBounceType::Informational));
    assert!(matches!(b, DockBounceType::Informational));
    assert!(matches!(c, DockBounceType::Critical));
  }

  #[test]
  fn permission_kind_repr_i32_matches_capi_constants() {
    // PermissionKind is `#[repr(i32)]` so its enum value is the same
    // integer the C ABI sends. A regression that changed the repr or
    // reordered variants would invert which capability is being asked
    // about.
    assert_eq!(
      PermissionKind::Notifications as i32,
      LAUFEY_PERMISSION_NOTIFICATIONS
    );
  }

  unsafe extern "C" fn fake_register_scheme_handler(
    _backend_data: *mut c_void,
    _scheme: *const std::os::raw::c_char,
    _handler: ffi::laufey_scheme_request_fn,
    _on_cancel: ffi::laufey_scheme_cancel_fn,
    _user_data: *mut c_void,
  ) {
  }

  // scheme_handlers_supported() reports whether the backend filled in
  // register_scheme_handler: NULL on engine-less backends (Winit) and on
  // backends predating API 26, set on every web-engine backend. Checked
  // against local vtables because BACKEND_API is a set-once global owned by
  // the pdf tests below.
  #[test]
  fn scheme_handlers_supported_follows_the_vtable() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert!(!supports_scheme_handlers(&fake));
    fake.register_scheme_handler = Some(fake_register_scheme_handler);
    assert!(supports_scheme_handlers(&fake));
  }

  // --- Passkeys ---
  //
  // Local fake vtables (BACKEND_API belongs to the pdf tests), driven through
  // the `_with` bodies of passkey_capabilities / passkey_create / passkey_get.

  fn block_on<F: Future>(fut: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
      .build()
      .unwrap()
      .block_on(fut)
  }

  static PASSKEY_CALLS: Mutex<Vec<(u32, u32, String)>> = Mutex::new(Vec::new());

  unsafe extern "C" fn fake_passkey_capabilities(
    _backend_data: *mut c_void,
  ) -> u32 {
    // Unknown bits are ignored.
    LAUFEY_PASSKEY_PLATFORM_AUTHENTICATOR | 0x80
  }

  // window_id 1: answers synchronously; 2: from another thread, later; 3: a
  // NULL result; 4: never (the backend broke its contract).
  unsafe extern "C" fn fake_passkey_request(
    _backend_data: *mut c_void,
    window_id: u32,
    kind: u32,
    options_json: *const c_char,
    callback: ffi::laufey_passkey_result_fn,
    user_data: *mut c_void,
  ) {
    let options = CStr::from_ptr(options_json).to_string_lossy().into_owned();
    PASSKEY_CALLS
      .lock()
      .unwrap()
      .push((window_id, kind, options));
    let cb = callback.expect("callback must be set");
    match window_id {
      1 => cb(
        user_data,
        c"{\"ok\":true,\"credential\":{\"id\":\"AQ\"}}".as_ptr(),
      ),
      2 => {
        let ud = user_data as usize;
        std::thread::spawn(move || {
          std::thread::sleep(std::time::Duration::from_millis(50));
          unsafe {
            cb(
              ud as *mut c_void,
              c"{\"ok\":false,\"error\":{\"code\":\"cancelled\",\"message\":\"m\"}}"
                .as_ptr(),
            )
          };
        });
      }
      3 => cb(user_data, std::ptr::null()),
      _ => {
        // Never answers: drop the sender so the future still resolves.
        drop(Box::from_raw(
          user_data as *mut tokio::sync::oneshot::Sender<String>,
        ));
      }
    }
  }

  fn assert_send_static<T: Send + 'static>(_: &T) {}

  #[test]
  fn passkeys_without_backend_support() {
    let fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(
      passkey_capabilities_with(&fake),
      PasskeyCapabilities::default()
    );
    let fut = passkey_request_with(&fake, 0, LAUFEY_PASSKEY_GET, "{}");
    assert_send_static(&fut);
    assert_eq!(
      block_on(fut),
      r#"{"ok":false,"error":{"code":"not_supported","message":"Native passkeys are not supported by this backend."}}"#
    );
  }

  #[test]
  fn passkey_capabilities_follow_the_flags() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    fake.passkey_capabilities = Some(fake_passkey_capabilities);
    assert_eq!(
      passkey_capabilities_with(&fake),
      PasskeyCapabilities {
        platform_authenticator: true,
        security_keys: false,
      }
    );
  }

  #[test]
  fn passkey_requests_pass_through_and_resolve() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    fake.passkey_request = Some(fake_passkey_request);
    let opts = r#"{"challenge":"AAAA","rpId":"example.com","x":"\u00e9 é"}"#;

    // Synchronous answer, create kind, options passed byte for byte.
    let out =
      block_on(passkey_request_with(&fake, 1, LAUFEY_PASSKEY_CREATE, opts));
    assert_eq!(out, r#"{"ok":true,"credential":{"id":"AQ"}}"#);
    // Later answer from another thread, get kind.
    let out =
      block_on(passkey_request_with(&fake, 2, LAUFEY_PASSKEY_GET, opts));
    assert!(out.contains(r#""code":"cancelled""#));
    // A NULL result is an error envelope, not a crash.
    let out =
      block_on(passkey_request_with(&fake, 3, LAUFEY_PASSKEY_GET, opts));
    assert!(out.contains(r#""code":"unknown""#));
    // A backend that never answers (and drops its sender) still resolves.
    let out =
      block_on(passkey_request_with(&fake, 4, LAUFEY_PASSKEY_GET, opts));
    assert!(out.contains("ended without a result"));

    let calls = PASSKEY_CALLS.lock().unwrap();
    let mine: Vec<_> = calls.iter().filter(|c| c.2 == opts).collect();
    assert_eq!(mine.len(), 4);
    assert_eq!((mine[0].0, mine[0].1), (1, LAUFEY_PASSKEY_CREATE));
    assert_eq!((mine[1].0, mine[1].1), (2, LAUFEY_PASSKEY_GET));
  }

  #[test]
  fn passkey_options_with_nul_never_reach_the_backend() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    fake.passkey_request = Some(fake_passkey_request);
    let opts = "{\"marker\":\"nul-test\0\"}";
    let out =
      block_on(passkey_request_with(&fake, 1, LAUFEY_PASSKEY_GET, opts));
    assert!(out.contains(r#""code":"unknown""#));
    assert!(out.contains("NUL"));
    assert!(PASSKEY_CALLS
      .lock()
      .unwrap()
      .iter()
      .all(|c| !c.2.contains("nul-test")));
  }

  // Fake print_to_pdf backend shared by the pdf tests, dispatching on
  // window_id: 999 completes with empty bytes, 777 never completes (watchdog
  // path), 778 completes late from another thread (after the test watchdog's
  // deadline), anything else completes immediately with fake PDF bytes.
  unsafe extern "C" fn fake_print_to_pdf(
    _backend_data: *mut c_void,
    window_id: u32,
    callback: ffi::laufey_pdf_result_fn,
    user_data: *mut c_void,
  ) {
    let cb = callback.expect("callback must be set");
    match window_id {
      999 => cb(std::ptr::null(), 0, std::ptr::null(), user_data),
      777 => {}
      778 => {
        let ud = user_data as usize;
        std::thread::spawn(move || {
          std::thread::sleep(std::time::Duration::from_millis(800));
          static LATE: &[u8] = b"%PDF-late";
          unsafe {
            cb(
              LATE.as_ptr(),
              LATE.len(),
              std::ptr::null(),
              ud as *mut c_void,
            )
          };
        });
      }
      _ => {
        let bytes: &[u8] = b"%PDF-1.4 fake";
        cb(bytes.as_ptr(), bytes.len(), std::ptr::null(), user_data);
      }
    }
  }

  // Mimic real backends: deliver the task on another thread so
  // `run_on_ui_thread` must actually wait (inline delivery would not
  // exercise the channel rendezvous).
  unsafe extern "C" fn fake_post_ui_task(
    _backend_data: *mut c_void,
    task: Option<unsafe extern "C" fn(*mut c_void)>,
    data: *mut c_void,
  ) {
    let Some(task) = task else {
      return;
    };
    let data = data as usize;
    std::thread::spawn(move || {
      // SAFETY: caller of run_on_ui_thread keeps `data` live until the
      // rendezvous send inside the trampoline.
      unsafe { task(data as *mut c_void) };
    });
  }

  // Install the shared fake backend. BACKEND_API is a set-once global and the
  // pdf / run_on_ui_thread tests run concurrently, so every caller installs
  // the *same* fake and the first one wins.
  fn install_pdf_fake() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    fake.print_to_pdf = Some(fake_print_to_pdf);
    fake.post_ui_task = Some(fake_post_ui_task);
    nul::install(&mut fake);
    let _ = BACKEND_API.set(Box::leak(Box::new(fake)));
  }

  // on_cancel marks the exchange it names, and only that one: is_cancelled
  // turns true for it, an exchange finished before is forgotten, and a
  // later exchange at the same address starts uncancelled.
  #[test]
  fn scheme_cancel_marks_only_its_exchange() {
    install_pdf_fake();
    let ptr = 0x5ce0_usize as *mut ffi::laufey_scheme_exchange_t;
    let other = 0x5ce8_usize as *mut ffi::laufey_scheme_exchange_t;
    let held: Arc<Mutex<Vec<SchemeRequest>>> = Arc::new(Mutex::new(Vec::new()));
    // A store of its own would race the other tests' registrations; the
    // trampoline reads the process-wide one, so set it directly.
    *scheme_handler_store().lock().unwrap() = Some(Arc::new({
      let held = held.clone();
      move |req: SchemeRequest| held.lock().unwrap().push(req)
    }));
    let request = |p| unsafe {
      scheme_request_trampoline(
        std::ptr::null_mut(),
        1,
        p,
        c"GET".as_ptr(),
        c"app://x/".as_ptr(),
        std::ptr::null(),
        0,
      )
    };
    request(ptr);
    request(other);
    let mut reqs = std::mem::take(&mut *held.lock().unwrap());
    assert_eq!(reqs.len(), 2);
    assert!(!reqs[0].exchange.is_cancelled());
    unsafe { scheme_cancel_trampoline(std::ptr::null_mut(), ptr) };
    assert!(reqs[0].exchange.is_cancelled());
    assert!(
      !reqs[1].exchange.is_cancelled(),
      "another exchange was marked"
    );
    for r in reqs.drain(..) {
      r.exchange.finish();
    }
    // Finished: the address is free, and a new exchange there is fresh.
    unsafe { scheme_cancel_trampoline(std::ptr::null_mut(), ptr) };
    request(ptr);
    let again = held.lock().unwrap().pop().unwrap();
    assert!(!again.exchange.is_cancelled());
    again.exchange.finish();
    *scheme_handler_store().lock().unwrap() = None;
  }

  // Handlers run after their slot's lock is released: one that replaces
  // itself (or calls back into the laufey API, which takes the same locks)
  // must not deadlock.
  #[test]
  fn handlers_run_without_their_slot_lock() {
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
      *dock_reopen_handler().lock().unwrap() = Some(Arc::new(|_| {
        *dock_reopen_handler().lock().unwrap() = None;
      }));
      unsafe { dock_reopen_callback(std::ptr::null_mut(), false) };
      *open_url_handler().lock().unwrap() = Some(Arc::new(|_: &str| {
        *open_url_handler().lock().unwrap() = None;
      }));
      unsafe { open_url_callback(std::ptr::null_mut(), c"x://y".as_ptr()) };
      menu_click_handlers().lock().unwrap().insert(
        77,
        Arc::new(|_: &str| {
          menu_click_handlers().lock().unwrap().remove(&77);
        }),
      );
      unsafe {
        menu_click_callback(std::ptr::null_mut(), 77, c"item".as_ptr())
      };
      let _ = tx.send(
        dock_reopen_handler().lock().unwrap().is_none()
          && open_url_handler().lock().unwrap().is_none()
          && !menu_click_handlers().lock().unwrap().contains_key(&77),
      );
    });
    let cleared = rx
      .recv_timeout(std::time::Duration::from_secs(10))
      .expect("a handler deadlocked on its own slot");
    assert!(cleared);
  }

  // Strings reach the backend as C strings, which end at the first NUL. A
  // caller-supplied string with a NUL byte (arbitrary JS can produce one)
  // must make the wrapper fail safely — no backend call, the function's
  // failure value — and never panic (the process aborts on a panic across
  // the runtime boundary).
  mod nul {
    use super::super::*;
    use std::sync::Mutex;

    // What reached the fake backend, by entry point.
    pub static SEEN: Mutex<Vec<String>> = Mutex::new(Vec::new());
    // (result, error) pointers of the last js_call_respond.
    pub static RESPONDED: Mutex<Vec<(usize, usize)>> = Mutex::new(Vec::new());

    // Sentinel "values": the tests only need to tell constructors apart.
    pub const NULL_VALUE: usize = 0x10;
    pub const STRING_VALUE: usize = 0x20;
    pub const DICT_VALUE: usize = 0x30;

    fn seen(what: &str, s: *const c_char) {
      let text = if s.is_null() {
        "<null>".to_string()
      } else {
        unsafe { CStr::from_ptr(s) }.to_string_lossy().into_owned()
      };
      SEEN.lock().unwrap().push(format!("{what}:{text}"));
    }

    unsafe extern "C" fn set_title(_: *mut c_void, _: u32, t: *const c_char) {
      seen("set_title", t);
    }
    unsafe extern "C" fn navigate(_: *mut c_void, _: u32, u: *const c_char) {
      seen("navigate", u);
    }
    unsafe extern "C" fn execute_js(
      _: *mut c_void,
      _: u32,
      script: *const c_char,
      _: ffi::laufey_js_result_fn,
      _: *mut c_void,
    ) {
      seen("execute_js", script);
    }
    unsafe extern "C" fn show_dialog(
      _: *mut c_void,
      _: u32,
      _: c_int,
      title: *const c_char,
      _: *const c_char,
      _: *const c_char,
      _: *mut *mut c_char,
    ) -> c_int {
      seen("show_dialog", title);
      1
    }
    unsafe extern "C" fn write_clipboard_text(
      _: *mut c_void,
      t: *const c_char,
    ) {
      seen("write_clipboard_text", t);
    }
    unsafe extern "C" fn set_dock_badge(_: *mut c_void, t: *const c_char) {
      seen("set_dock_badge", t);
    }
    unsafe extern "C" fn set_tray_tooltip(
      _: *mut c_void,
      _: u32,
      t: *const c_char,
    ) {
      seen("set_tray_tooltip", t);
    }
    unsafe extern "C" fn set_js_namespace(_: *mut c_void, n: *const c_char) {
      seen("set_js_namespace", n);
    }
    unsafe extern "C" fn register_scheme_handler(
      _: *mut c_void,
      scheme: *const c_char,
      _: ffi::laufey_scheme_request_fn,
      _: ffi::laufey_scheme_cancel_fn,
      _: *mut c_void,
    ) {
      seen("register_scheme_handler", scheme);
    }
    unsafe extern "C" fn value_null(_: *mut c_void) -> *mut LaufeyValue {
      NULL_VALUE as *mut LaufeyValue
    }
    unsafe extern "C" fn value_string(
      _: *mut c_void,
      v: *const c_char,
    ) -> *mut LaufeyValue {
      seen("value_string", v);
      STRING_VALUE as *mut LaufeyValue
    }
    unsafe extern "C" fn value_dict(_: *mut c_void) -> *mut LaufeyValue {
      DICT_VALUE as *mut LaufeyValue
    }
    unsafe extern "C" fn value_dict_set(
      _: *mut LaufeyValue,
      key: *const c_char,
      _: *mut LaufeyValue,
    ) -> bool {
      seen("value_dict_set", key);
      true
    }
    unsafe extern "C" fn js_call_respond(
      _: *mut c_void,
      _: u64,
      result: *mut LaufeyValue,
      error: *mut LaufeyValue,
    ) {
      RESPONDED
        .lock()
        .unwrap()
        .push((result as usize, error as usize));
    }

    pub fn install(fake: &mut LaufeyBackendApi) {
      fake.set_title = Some(set_title);
      fake.navigate = Some(navigate);
      fake.execute_js = Some(execute_js);
      fake.show_dialog = Some(show_dialog);
      fake.write_clipboard_text = Some(write_clipboard_text);
      fake.set_dock_badge = Some(set_dock_badge);
      fake.set_tray_tooltip = Some(set_tray_tooltip);
      fake.set_js_namespace = Some(set_js_namespace);
      fake.register_scheme_handler = Some(register_scheme_handler);
      fake.value_null = Some(value_null);
      fake.value_string = Some(value_string);
      fake.value_dict = Some(value_dict);
      fake.value_dict_set = Some(value_dict_set);
      fake.js_call_respond = Some(js_call_respond);
    }
  }

  #[test]
  fn strings_with_nul_bytes_fail_safely_instead_of_panicking() {
    use std::sync::mpsc;
    install_pdf_fake();
    let seen = || nul::SEEN.lock().unwrap().clone();
    let w = Window::from_id(5);

    // Unit-returning wrappers: a no-op, the backend is not called.
    w.set_title("a\0b");
    w.navigate("app://x/\0");
    write_clipboard_text("x\0y");
    set_dock_badge(Some("1\0"));
    TrayIcon::from_id(3).set_tooltip(Some("t\0"));
    set_js_namespace("Na\0me");
    register_scheme_handler("sch\0eme", |_req| {});
    assert!(seen().is_empty(), "reached the backend: {:?}", seen());

    // execute_js: not run, the callback hears why.
    let (tx, rx) = mpsc::channel();
    w.execute_js("1\0", Some(move |r| tx.send(r).unwrap()));
    let Err(err) = rx.recv().unwrap() else {
      panic!("execute_js with a NUL ran");
    };
    assert!(
      err.as_string().unwrap().contains("NUL"),
      "{:?}",
      err.as_string()
    );

    // Dialogs: as if cancelled.
    assert!(!w.confirm("t\0", "m"));
    assert_eq!(w.prompt("t", "m\0", "d"), None);
    assert!(!confirm("t", "m\0"));

    // Values: a string becomes null, a key is left out.
    assert_eq!(
      Value::String("a\0b".into()).to_raw() as usize,
      nul::NULL_VALUE
    );
    let mut map = HashMap::new();
    map.insert("k\0".to_string(), Value::Null);
    assert_eq!(Value::Dict(map).to_raw() as usize, nul::DICT_VALUE);
    assert!(seen().is_empty(), "reached the backend: {:?}", seen());

    // A JS call answered with a NUL-bearing value is rejected instead.
    JsCall {
      window_id: 5,
      call_id: 9,
      method: "m".into(),
      args: vec![],
    }
    .resolve(Value::List(vec![Value::String("x\0".into())]));
    let (result, error) = *nul::RESPONDED.lock().unwrap().last().unwrap();
    assert_eq!(result, 0, "resolved despite the NUL");
    assert_eq!(error, nul::STRING_VALUE);
    assert!(seen()
      .iter()
      .any(|s| s.starts_with("value_string:the result")));

    // The same calls without a NUL do reach the backend.
    nul::SEEN.lock().unwrap().clear();
    w.set_title("ok");
    w.navigate("app://x/");
    set_js_namespace("Name");
    assert!(w.confirm("t", "m"));
    let got = seen();
    for want in [
      "set_title:ok",
      "navigate:app://x/",
      "set_js_namespace:Name",
      "show_dialog:t",
    ] {
      assert!(got.iter().any(|s| s == want), "missing {want}: {got:?}");
    }
  }

  // Regression guard for print_to_pdf's marshaling and file handling,
  // exercised against a fake backend so it needs no window or display: a
  // successful backend call must hand the PDF bytes back through the callback,
  // a `Some(path)` must make the capi (not the backend) write those bytes to
  // disk, empty output must resolve to empty bytes (not an error), and an
  // unwritable path (e.g. an interior NUL, which can arrive from arbitrary JS)
  // must surface through the callback's Err rather than panicking.
  #[test]
  fn print_to_pdf_marshals_bytes_and_writes_path() {
    use std::sync::mpsc;

    install_pdf_fake();

    // Success without a path: bytes flow back through the trampoline.
    let (tx, rx) = mpsc::channel();
    Window::from_id(1).print_to_pdf(None, move |r| tx.send(r).unwrap());
    assert_eq!(rx.recv().unwrap().unwrap(), b"%PDF-1.4 fake");

    // A path makes the capi write the bytes to disk and still return them.
    let out = std::env::temp_dir().join("laufey_print_to_pdf_test.pdf");
    let (tx, rx) = mpsc::channel();
    Window::from_id(1)
      .print_to_pdf(Some(out.to_str().unwrap()), move |r| tx.send(r).unwrap());
    assert_eq!(rx.recv().unwrap().unwrap(), b"%PDF-1.4 fake");
    assert_eq!(std::fs::read(&out).unwrap(), b"%PDF-1.4 fake");
    let _ = std::fs::remove_file(&out);

    // Empty backend output resolves to empty bytes, not an error.
    let (tx, rx) = mpsc::channel();
    Window::from_id(999).print_to_pdf(None, move |r| tx.send(r).unwrap());
    assert!(rx.recv().unwrap().unwrap().is_empty());

    // An unwritable path (interior NUL) is reported through the callback's
    // Err, never a panic.
    let (tx, rx) = mpsc::channel();
    Window::from_id(1)
      .print_to_pdf(Some("bad\0path.pdf"), move |r| tx.send(r).unwrap());
    let err = rx.recv().unwrap().unwrap_err();
    assert!(
      err.contains("failed to write PDF"),
      "unexpected error: {err}"
    );
  }

  // The watchdog must resolve the callback when the backend's completion
  // handler never fires, and a completion arriving after the timeout must be
  // dropped instead of double-invoking the already-consumed callback.
  #[test]
  fn print_to_pdf_times_out_and_ignores_late_completion() {
    use std::sync::mpsc;
    use std::time::Duration;

    install_pdf_fake();

    // Backend never calls back -> the watchdog fires with a timeout Err.
    let (tx, rx) = mpsc::channel();
    Window::from_id(777).print_to_pdf_with_timeout(
      None,
      Duration::from_millis(100),
      move |r| tx.send(r).unwrap(),
    );
    let err = rx
      .recv_timeout(Duration::from_secs(10))
      .expect("watchdog should resolve the callback")
      .unwrap_err();
    assert!(err.contains("timed out"), "unexpected error: {err}");

    // Backend calls back *after* the timeout -> the caller sees exactly one
    // (timeout) result; the late completion is discarded safely.
    let (tx, rx) = mpsc::channel();
    Window::from_id(778).print_to_pdf_with_timeout(
      None,
      Duration::from_millis(100),
      move |r| tx.send(r).unwrap(),
    );
    let first = rx
      .recv_timeout(Duration::from_secs(10))
      .expect("watchdog should resolve the callback");
    assert!(first.unwrap_err().contains("timed out"));
    // Let the late completion arrive and be discarded; a second delivery
    // would double-invoke a consumed FnOnce (crash) or send on this channel.
    std::thread::sleep(Duration::from_millis(1200));
    assert!(
      rx.try_recv().is_err(),
      "late completion must not invoke the callback a second time"
    );
  }

  // run_on_ui_thread returns the closure's value. On Apple the hop path is
  // exercised from a worker (the test runner is main and would inline);
  // elsewhere the function is a plain call.
  #[test]
  fn run_on_ui_thread_hops_and_returns_value() {
    install_pdf_fake();

    let run = || {
      assert_eq!(run_on_ui_thread(|| 42u32), 42);
      let err: Result<(), &str> = run_on_ui_thread(|| Err("surface failed"));
      assert_eq!(err, Err("surface failed"));
      let flag = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
      let flag2 = flag.clone();
      run_on_ui_thread(move || {
        flag2.store(true, std::sync::atomic::Ordering::SeqCst);
      });
      assert!(flag.load(std::sync::atomic::Ordering::SeqCst));
    };

    #[cfg(any(target_os = "macos", target_os = "ios"))]
    std::thread::spawn(run)
      .join()
      .expect("run_on_ui_thread worker panicked");
    #[cfg(not(any(target_os = "macos", target_os = "ios")))]
    run();
  }
}
