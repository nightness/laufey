// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Global shortcuts, launch at login and DevTools control (API 40).
//!
//! See `docs/global-shortcuts.md`, `docs/launch-at-login.md` and
//! `docs/devtools.md`. Every function here may be called from any thread.
//! Backends older than API 40, and the Winit backend, report all of it as
//! unsupported.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::future::Future;
use std::sync::{Arc, Mutex, OnceLock};

use crate::io::take_backend_string;
use crate::{api, LaufeyBackendApi, Window};

// --- Constants (mirror laufey.h) -------------------------------------------

pub const LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS: u32 = 1 << 0;
pub const LAUFEY_SYSTEM_CAP_SHORTCUTS_USER_BINDS: u32 = 1 << 1;
pub const LAUFEY_SYSTEM_CAP_LAUNCH_AT_LOGIN: u32 = 1 << 2;
pub const LAUFEY_SYSTEM_CAP_DEVTOOLS: u32 = 1 << 3;

pub const LAUFEY_SHORTCUT_OK: i32 = 0;
pub const LAUFEY_SHORTCUT_INVALID: i32 = 1;
pub const LAUFEY_SHORTCUT_CONFLICT: i32 = 2;
pub const LAUFEY_SHORTCUT_ALREADY_REGISTERED: i32 = 3;
pub const LAUFEY_SHORTCUT_NOT_SUPPORTED: i32 = 4;
pub const LAUFEY_SHORTCUT_DENIED: i32 = 5;
pub const LAUFEY_SHORTCUT_FAILED: i32 = 6;
pub const LAUFEY_MAX_SHORTCUTS: usize = 256;

pub const LAUFEY_LOGIN_ITEM_DISABLED: i32 = 0;
pub const LAUFEY_LOGIN_ITEM_ENABLED: i32 = 1;
pub const LAUFEY_LOGIN_ITEM_REQUIRES_APPROVAL: i32 = 2;
pub const LAUFEY_LOGIN_ITEM_NOT_SUPPORTED: i32 = 3;
pub const LAUFEY_LOGIN_ITEM_FAILED: i32 = 4;

// ===========================================================================
// Capabilities
// ===========================================================================

/// What [`system_capabilities`] reports for this backend, OS and session.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct SystemCapabilities {
  pub bits: u32,
}

impl SystemCapabilities {
  /// [`register_shortcut`] can bind system-wide shortcuts.
  pub fn global_shortcuts(&self) -> bool {
    self.bits & LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS != 0
  }
  /// The user approves each shortcut and may pick another trigger (the XDG
  /// GlobalShortcuts portal on Wayland).
  pub fn shortcuts_user_binds(&self) -> bool {
    self.bits & LAUFEY_SYSTEM_CAP_SHORTCUTS_USER_BINDS != 0
  }
  /// [`launch_at_login`] / [`set_launch_at_login`] work.
  pub fn launch_at_login(&self) -> bool {
    self.bits & LAUFEY_SYSTEM_CAP_LAUNCH_AT_LOGIN != 0
  }
  /// The DevTools controls on [`Window`] work.
  pub fn devtools(&self) -> bool {
    self.bits & LAUFEY_SYSTEM_CAP_DEVTOOLS != 0
  }
}

/// The global-shortcut, launch-at-login and DevTools capabilities.
pub fn system_capabilities() -> SystemCapabilities {
  system_capabilities_with(api())
}

fn system_capabilities_with(api: &LaufeyBackendApi) -> SystemCapabilities {
  let bits = match api.system_capabilities {
    Some(f) => unsafe { f(api.backend_data) },
    None => 0,
  };
  SystemCapabilities { bits }
}

// ===========================================================================
// Global shortcuts
// ===========================================================================

/// Why [`register_shortcut`] failed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShortcutError {
  /// The accelerator doesn't parse, or names a printable key without a
  /// modifier other than Shift.
  Invalid,
  /// The OS refused it: another app holds that combination.
  Conflict,
  /// This app already registered it.
  AlreadyRegistered,
  /// No global shortcuts here (Wayland without the GlobalShortcuts portal,
  /// the Winit backend, an older backend).
  NotSupported,
  /// The user declined it (the portal's dialog).
  Denied,
  /// Any other OS failure.
  Failed,
}

impl ShortcutError {
  pub fn from_raw(raw: i32) -> Option<Self> {
    match raw {
      LAUFEY_SHORTCUT_OK => None,
      LAUFEY_SHORTCUT_INVALID => Some(Self::Invalid),
      LAUFEY_SHORTCUT_CONFLICT => Some(Self::Conflict),
      LAUFEY_SHORTCUT_ALREADY_REGISTERED => Some(Self::AlreadyRegistered),
      LAUFEY_SHORTCUT_NOT_SUPPORTED => Some(Self::NotSupported),
      LAUFEY_SHORTCUT_DENIED => Some(Self::Denied),
      _ => Some(Self::Failed),
    }
  }

  /// A stable lower-case name ("invalid", "conflict", "already_registered",
  /// "not_supported", "denied", "failed").
  pub fn code(&self) -> &'static str {
    match self {
      Self::Invalid => "invalid",
      Self::Conflict => "conflict",
      Self::AlreadyRegistered => "already_registered",
      Self::NotSupported => "not_supported",
      Self::Denied => "denied",
      Self::Failed => "failed",
    }
  }
}

impl std::fmt::Display for ShortcutError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    f.write_str(match self {
      Self::Invalid => "invalid accelerator",
      Self::Conflict => "the shortcut is already taken by another application",
      Self::AlreadyRegistered => "the shortcut is already registered",
      Self::NotSupported => "global shortcuts are not supported here",
      Self::Denied => "the user declined the shortcut",
      Self::Failed => "the shortcut could not be registered",
    })
  }
}

impl std::error::Error for ShortcutError {}

type ShortcutHandler = Arc<dyn Fn(&str) + Send + Sync>;

fn shortcut_handler() -> &'static Mutex<Option<ShortcutHandler>> {
  static SLOT: OnceLock<Mutex<Option<ShortcutHandler>>> = OnceLock::new();
  SLOT.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn shortcut_trampoline(
  _user_data: *mut c_void,
  accelerator: *const c_char,
) {
  if accelerator.is_null() {
    return;
  }
  let accel = unsafe { CStr::from_ptr(accelerator) }.to_string_lossy();
  // Cloned out: the handler runs without the lock held (it may replace
  // itself).
  let handler = shortcut_handler().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(&accel);
  }
}

/// Register the (process-wide) global-shortcut handler: it gets the
/// canonical accelerator of each registered shortcut the user presses,
/// whichever app has the focus. Runs on the backend UI thread; keep it short.
/// Replaces any previous handler.
pub fn on_shortcut<F>(handler: F)
where
  F: Fn(&str) + Send + Sync + 'static,
{
  *shortcut_handler().lock().unwrap() = Some(Arc::new(handler));
  let api = api();
  if let Some(f) = api.set_shortcut_handler {
    unsafe {
      f(
        api.backend_data,
        Some(shortcut_trampoline),
        std::ptr::null_mut(),
      )
    };
  }
}

/// Remove the global-shortcut handler (the shortcuts stay registered).
pub fn clear_shortcut_handler() {
  let api = api();
  if let Some(f) = api.set_shortcut_handler {
    unsafe { f(api.backend_data, None, std::ptr::null_mut()) };
  }
  *shortcut_handler().lock().unwrap() = None;
}

unsafe extern "C" fn shortcut_result_trampoline(
  user_data: *mut c_void,
  status: c_int,
  accelerator: *const c_char,
) {
  // Exactly once per request (laufey.h).
  let tx = unsafe {
    Box::from_raw(
      user_data
        as *mut tokio::sync::oneshot::Sender<Result<String, ShortcutError>>,
    )
  };
  let result = match ShortcutError::from_raw(status) {
    None if !accelerator.is_null() => Ok(
      unsafe { CStr::from_ptr(accelerator) }
        .to_string_lossy()
        .into_owned(),
    ),
    None => Err(ShortcutError::Failed),
    Some(e) => Err(e),
  };
  let _ = tx.send(result);
}

/// Bind `accelerator` ("CommandOrControl+Shift+K") system-wide. Resolves to
/// its canonical form ("Ctrl+Shift+K"; the handler and [`shortcuts`] use it)
/// once the OS accepted it. The request is made when this is called, not
/// when the future is first polled.
pub fn register_shortcut(
  accelerator: &str,
) -> impl Future<Output = Result<String, ShortcutError>> + Send + 'static {
  register_shortcut_with(api(), accelerator)
}

fn register_shortcut_with(
  api: &LaufeyBackendApi,
  accelerator: &str,
) -> impl Future<Output = Result<String, ShortcutError>> + Send + 'static {
  let rx = match (api.register_shortcut, CString::new(accelerator)) {
    (Some(f), Ok(accel)) => {
      let (tx, rx) =
        tokio::sync::oneshot::channel::<Result<String, ShortcutError>>();
      let user_data = Box::into_raw(Box::new(tx)) as *mut c_void;
      unsafe {
        f(
          api.backend_data,
          accel.as_ptr(),
          Some(shortcut_result_trampoline),
          user_data,
        )
      };
      Ok(rx)
    }
    (None, _) => Err(ShortcutError::NotSupported),
    (_, Err(_)) => Err(ShortcutError::Invalid),
  };
  async move {
    match rx {
      Ok(rx) => rx.await.unwrap_or(Err(ShortcutError::Failed)),
      Err(e) => Err(e),
    }
  }
}

/// Release a shortcut this app registered (any spelling). False if it
/// wasn't registered. Its handler stops firing before this returns.
pub fn unregister_shortcut(accelerator: &str) -> bool {
  let api = api();
  let (Some(f), Ok(accel)) =
    (api.unregister_shortcut, CString::new(accelerator))
  else {
    return false;
  };
  unsafe { f(api.backend_data, accel.as_ptr()) }
}

/// Release every shortcut this app registered.
pub fn unregister_all_shortcuts() {
  let api = api();
  if let Some(f) = api.unregister_all_shortcuts {
    unsafe { f(api.backend_data) };
  }
}

/// The canonical accelerators currently registered, in registration order.
pub fn shortcuts() -> Vec<String> {
  shortcuts_with(api())
}

fn shortcuts_with(api: &LaufeyBackendApi) -> Vec<String> {
  let Some(f) = api.list_shortcuts else {
    return Vec::new();
  };
  unsafe { take_backend_string(api, f(api.backend_data)) }
    .map(|s| {
      s.split('\n')
        .filter(|l| !l.is_empty())
        .map(str::to_owned)
        .collect()
    })
    .unwrap_or_default()
}

/// The canonical form of an accelerator ("shift+ctrl+k" -> "Ctrl+Shift+K"),
/// as [`register_shortcut`] reports it, or `None` when it doesn't parse (or
/// the backend has no global shortcuts).
pub fn canonical_accelerator(accelerator: &str) -> Option<String> {
  canonical_accelerator_with(api(), accelerator)
}

fn canonical_accelerator_with(
  api: &LaufeyBackendApi,
  accelerator: &str,
) -> Option<String> {
  let f = api.canonicalize_accelerator?;
  let accel = CString::new(accelerator).ok()?;
  unsafe { take_backend_string(api, f(api.backend_data, accel.as_ptr())) }
}

/// Test-only: fire the handler for a registered shortcut through the
/// backend's own dispatch. False when it isn't registered, no handler is set,
/// or the backend lacks the hook.
pub fn test_trigger_shortcut(accelerator: &str) -> bool {
  let api = api();
  let (Some(f), Ok(accel)) =
    (api.test_trigger_shortcut, CString::new(accelerator))
  else {
    return false;
  };
  unsafe { f(api.backend_data, accel.as_ptr()) }
}

// ===========================================================================
// Launch at login
// ===========================================================================

/// Whether the app starts when the user logs in.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoginItemState {
  Disabled,
  Enabled,
  /// Registered, but the user must allow it in the system settings first
  /// (macOS Login Items; a Windows startup entry the user turned off).
  RequiresApproval,
  NotSupported,
}

impl LoginItemState {
  fn from_raw(raw: i32) -> Option<Self> {
    match raw {
      LAUFEY_LOGIN_ITEM_DISABLED => Some(Self::Disabled),
      LAUFEY_LOGIN_ITEM_ENABLED => Some(Self::Enabled),
      LAUFEY_LOGIN_ITEM_REQUIRES_APPROVAL => Some(Self::RequiresApproval),
      LAUFEY_LOGIN_ITEM_NOT_SUPPORTED => Some(Self::NotSupported),
      _ => None,
    }
  }

  /// A stable lower-case name ("disabled", "enabled", "requires_approval",
  /// "not_supported").
  pub fn code(&self) -> &'static str {
    match self {
      Self::Disabled => "disabled",
      Self::Enabled => "enabled",
      Self::RequiresApproval => "requires_approval",
      Self::NotSupported => "not_supported",
    }
  }
}

/// The app's launch-at-login state (named after `LAUFEY_APP_ID`).
pub fn launch_at_login() -> LoginItemState {
  launch_at_login_with(api())
}

fn launch_at_login_with(api: &LaufeyBackendApi) -> LoginItemState {
  match api.get_launch_at_login {
    Some(f) => LoginItemState::from_raw(unsafe { f(api.backend_data) })
      .unwrap_or(LoginItemState::NotSupported),
    None => LoginItemState::NotSupported,
  }
}

/// Turn launch at login on or off. Returns the state afterwards, or the OS's
/// error message.
pub fn set_launch_at_login(enabled: bool) -> Result<LoginItemState, String> {
  set_launch_at_login_with(api(), enabled)
}

fn set_launch_at_login_with(
  api: &LaufeyBackendApi,
  enabled: bool,
) -> Result<LoginItemState, String> {
  let Some(f) = api.set_launch_at_login else {
    return Ok(LoginItemState::NotSupported);
  };
  let mut error: *mut c_char = std::ptr::null_mut();
  let raw = unsafe { f(api.backend_data, enabled, &mut error) };
  let message = unsafe { take_backend_string(api, error) };
  match LoginItemState::from_raw(raw) {
    Some(state) => Ok(state),
    None => Err(message.unwrap_or_else(|| "launch at login failed".into())),
  }
}

// ===========================================================================
// DevTools
// ===========================================================================

/// Whether DevTools may open at all in this process: false when the app
/// launched with `LAUFEY_INSPECTABLE=0` (or `"inspectable": false` in
/// `laufey-launch.json`), and on a backend without a web engine (Winit).
pub fn devtools_enabled() -> bool {
  let api = api();
  match api.is_devtools_enabled {
    Some(f) => unsafe { f(api.backend_data, 0) },
    None => false,
  }
}

impl Window {
  /// Close this window's DevTools (opened with [`Window::open_devtools`] or
  /// by the user).
  pub fn close_devtools(&self) {
    let api = api();
    if let Some(f) = api.close_devtools {
      unsafe { f(api.backend_data, self.id()) };
    }
  }

  /// Whether this window's DevTools are open.
  pub fn is_devtools_open(&self) -> bool {
    let api = api();
    match api.is_devtools_open {
      Some(f) => unsafe { f(api.backend_data, self.id()) },
      None => false,
    }
  }

  /// Open this window's DevTools if they are closed, else close them.
  pub fn toggle_devtools(&self) {
    if self.is_devtools_open() {
      self.close_devtools();
    } else {
      self.open_devtools();
    }
  }

  /// Whether this window's engine lets DevTools open, read back from the
  /// engine (WKWebView `inspectable`, WebView2 `AreDevToolsEnabled`,
  /// WebKitGTK `enable-developer-extras`, CEF's DevTools availability).
  pub fn is_devtools_enabled(&self) -> bool {
    let api = api();
    match api.is_devtools_enabled {
      Some(f) => unsafe { f(api.backend_data, self.id()) },
      None => false,
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  // A handler that replaces itself (it takes the slot's lock) must not
  // deadlock: the trampoline calls it after releasing that lock.
  #[test]
  fn shortcut_handler_runs_without_the_slot_lock() {
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
      *shortcut_handler().lock().unwrap() = Some(Arc::new(move |_: &str| {
        *shortcut_handler().lock().unwrap() = None;
      }));
      unsafe { shortcut_trampoline(std::ptr::null_mut(), c"Ctrl+K".as_ptr()) };
      let _ = tx.send(shortcut_handler().lock().unwrap().is_none());
    });
    let cleared = rx
      .recv_timeout(std::time::Duration::from_secs(10))
      .expect("the handler deadlocked on its own slot");
    assert!(cleared);
  }

  fn block_on<F: Future>(fut: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
      .build()
      .unwrap()
      .block_on(fut)
  }

  unsafe extern "C" fn fake_register(
    _: *mut c_void,
    accel: *const c_char,
    cb: Option<unsafe extern "C" fn(*mut c_void, c_int, *const c_char)>,
    ud: *mut c_void,
  ) {
    let a = unsafe { CStr::from_ptr(accel) }.to_str().unwrap();
    let cb = cb.unwrap();
    match a {
      "Ctrl+K" => unsafe { cb(ud, LAUFEY_SHORTCUT_OK, c"Ctrl+K".as_ptr()) },
      "Alt+F1" => unsafe { cb(ud, LAUFEY_SHORTCUT_CONFLICT, std::ptr::null()) },
      "Alt+F2" => {
        // Answered later, from another thread.
        let ud = ud as usize;
        std::thread::spawn(move || unsafe {
          cb(ud as *mut c_void, LAUFEY_SHORTCUT_DENIED, std::ptr::null())
        });
      }
      _ => unsafe { cb(ud, LAUFEY_SHORTCUT_INVALID, std::ptr::null()) },
    }
  }

  #[test]
  fn register_outcomes() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(
      block_on(register_shortcut_with(&fake, "Ctrl+K")),
      Err(ShortcutError::NotSupported)
    );
    fake.register_shortcut = Some(fake_register);
    assert_eq!(
      block_on(register_shortcut_with(&fake, "Ctrl+K")),
      Ok("Ctrl+K".to_string())
    );
    assert_eq!(
      block_on(register_shortcut_with(&fake, "Alt+F1")),
      Err(ShortcutError::Conflict)
    );
    assert_eq!(
      block_on(register_shortcut_with(&fake, "Alt+F2")),
      Err(ShortcutError::Denied)
    );
    assert_eq!(
      block_on(register_shortcut_with(&fake, "nope")),
      Err(ShortcutError::Invalid)
    );
    // A NUL never reaches the backend.
    assert_eq!(
      block_on(register_shortcut_with(&fake, "Ctrl\0K")),
      Err(ShortcutError::Invalid)
    );
    assert_eq!(ShortcutError::Conflict.code(), "conflict");
    assert_eq!(ShortcutError::from_raw(99), Some(ShortcutError::Failed));
  }

  unsafe extern "C" fn fake_list(_: *mut c_void) -> *mut c_char {
    CString::new("Ctrl+K\nAlt+F3").unwrap().into_raw()
  }
  unsafe extern "C" fn fake_string_free(_: *mut c_void, s: *mut c_char) {
    drop(unsafe { CString::from_raw(s) });
  }

  unsafe extern "C" fn fake_canon(
    _: *mut c_void,
    accel: *const c_char,
  ) -> *mut c_char {
    match unsafe { CStr::from_ptr(accel) }.to_str().unwrap() {
      "shift+ctrl+k" => CString::new("Ctrl+Shift+K").unwrap().into_raw(),
      _ => std::ptr::null_mut(),
    }
  }

  #[test]
  fn canonical() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(canonical_accelerator_with(&fake, "shift+ctrl+k"), None);
    fake.canonicalize_accelerator = Some(fake_canon);
    fake.string_free = Some(fake_string_free);
    assert_eq!(
      canonical_accelerator_with(&fake, "shift+ctrl+k").as_deref(),
      Some("Ctrl+Shift+K")
    );
    assert_eq!(canonical_accelerator_with(&fake, "nope"), None);
  }

  #[test]
  fn list() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert!(shortcuts_with(&fake).is_empty());
    fake.list_shortcuts = Some(fake_list);
    fake.string_free = Some(fake_string_free);
    assert_eq!(shortcuts_with(&fake), vec!["Ctrl+K", "Alt+F3"]);
  }

  unsafe extern "C" fn fake_get_login(_: *mut c_void) -> c_int {
    LAUFEY_LOGIN_ITEM_REQUIRES_APPROVAL
  }
  unsafe extern "C" fn fake_set_login(
    _: *mut c_void,
    enabled: bool,
    error: *mut *mut c_char,
  ) -> c_int {
    if enabled {
      LAUFEY_LOGIN_ITEM_ENABLED
    } else {
      unsafe { *error = CString::new("denied").unwrap().into_raw() };
      LAUFEY_LOGIN_ITEM_FAILED
    }
  }

  #[test]
  fn login_items() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(launch_at_login_with(&fake), LoginItemState::NotSupported);
    assert_eq!(
      set_launch_at_login_with(&fake, true),
      Ok(LoginItemState::NotSupported)
    );
    fake.get_launch_at_login = Some(fake_get_login);
    fake.set_launch_at_login = Some(fake_set_login);
    fake.string_free = Some(fake_string_free);
    assert_eq!(
      launch_at_login_with(&fake),
      LoginItemState::RequiresApproval
    );
    assert_eq!(
      set_launch_at_login_with(&fake, true),
      Ok(LoginItemState::Enabled)
    );
    assert_eq!(set_launch_at_login_with(&fake, false), Err("denied".into()));
    assert_eq!(LoginItemState::RequiresApproval.code(), "requires_approval");
  }

  unsafe extern "C" fn fake_caps(_: *mut c_void) -> u32 {
    LAUFEY_SYSTEM_CAP_GLOBAL_SHORTCUTS | LAUFEY_SYSTEM_CAP_DEVTOOLS
  }

  #[test]
  fn capabilities() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(system_capabilities_with(&fake).bits, 0);
    fake.system_capabilities = Some(fake_caps);
    let caps = system_capabilities_with(&fake);
    assert!(caps.global_shortcuts() && caps.devtools());
    assert!(!caps.launch_at_login() && !caps.shortcuts_user_binds());
  }
}
