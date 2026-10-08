// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Auth sessions (API 42): a browser sign-in the OS runs for the app and
//! ends at a callback URL (RFC 8252 "native app" OAuth). macOS runs it in
//! `ASWebAuthenticationSession`, a sheet on the app's window, with a real
//! "cancelled" when the user closes it. Windows and Linux have no OS
//! equivalent; there [`auth_session_capabilities`] is empty and
//! [`auth_session_start`] answers [`AuthSessionErrorKind::NotSupported`], so
//! the embedder opens the system browser and receives the redirect itself
//! (a loopback listener or a claimed URL scheme).
//!
//! See `docs/auth-session.md`. Every function here may be called from any
//! thread.

use std::ffi::{c_char, c_void, CStr, CString};
use std::future::Future;

use crate::{api, LaufeyBackendApi};

// --- Constants (mirror laufey.h) -------------------------------------------

pub const LAUFEY_AUTH_SESSION_OK: i32 = 0;
pub const LAUFEY_AUTH_SESSION_CANCELLED: i32 = 1;
pub const LAUFEY_AUTH_SESSION_NOT_SUPPORTED: i32 = 2;
pub const LAUFEY_AUTH_SESSION_INVALID: i32 = 3;
pub const LAUFEY_AUTH_SESSION_BUSY: i32 = 4;
pub const LAUFEY_AUTH_SESSION_FAILED: i32 = 5;

pub const LAUFEY_AUTH_SESSION_CAP_SUPPORTED: u32 = 1 << 0;
pub const LAUFEY_AUTH_SESSION_CAP_EPHEMERAL: u32 = 1 << 1;
pub const LAUFEY_AUTH_SESSION_CAP_HTTPS_CALLBACK: u32 = 1 << 2;

pub const LAUFEY_AUTH_SESSION_EPHEMERAL: u32 = 1 << 0;

pub const LAUFEY_AUTH_SESSION_MAX_URL_BYTES: usize = 8192;

/// What [`auth_session_start`] can do on this backend and OS.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct AuthSessionCapabilities {
  /// An OS auth session exists (macOS 10.15+, WKWebView and CEF).
  pub supported: bool,
  /// `ephemeral` is honored (no shared browser cookies, no consent prompt).
  pub ephemeral: bool,
  /// An `https://` callback works (macOS 14.4+; the app needs an associated
  /// domain for the host).
  pub https_callback: bool,
}

/// The auth session capabilities of this backend. All false off macOS, on
/// the Winit backend and on backends older than API 42.
pub fn auth_session_capabilities() -> AuthSessionCapabilities {
  auth_session_capabilities_with(api())
}

fn auth_session_capabilities_with(
  api: &LaufeyBackendApi,
) -> AuthSessionCapabilities {
  let flags = match api.auth_session_capabilities {
    // SAFETY: a backend vtable entry, callable from any thread.
    Some(f) => unsafe { f(api.backend_data) },
    None => 0,
  };
  AuthSessionCapabilities {
    supported: flags & LAUFEY_AUTH_SESSION_CAP_SUPPORTED != 0,
    ephemeral: flags & LAUFEY_AUTH_SESSION_CAP_EPHEMERAL != 0,
    https_callback: flags & LAUFEY_AUTH_SESSION_CAP_HTTPS_CALLBACK != 0,
  }
}

/// How an auth session ended without a callback URL.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AuthSessionErrorKind {
  /// The user closed the sheet or declined the prompt, the anchor window
  /// closed, the app is quitting, or [`test_cancel_auth_session`].
  Cancelled,
  /// No OS auth session on this platform / backend: use the system browser
  /// (RFC 8252).
  NotSupported,
  /// A bad url, callback or window.
  Invalid,
  /// Another auth session is in progress (one at a time per app).
  Busy,
  /// The OS refused or failed.
  Failed,
}

impl AuthSessionErrorKind {
  /// The lowercase code (`cancelled`, `not_supported`, `invalid`, `busy`,
  /// `failed`).
  pub fn code(self) -> &'static str {
    match self {
      AuthSessionErrorKind::Cancelled => "cancelled",
      AuthSessionErrorKind::NotSupported => "not_supported",
      AuthSessionErrorKind::Invalid => "invalid",
      AuthSessionErrorKind::Busy => "busy",
      AuthSessionErrorKind::Failed => "failed",
    }
  }
}

/// An auth session that ended without a callback URL.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthSessionError {
  pub kind: AuthSessionErrorKind,
  pub message: String,
}

impl std::fmt::Display for AuthSessionError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "{}: {}", self.kind.code(), self.message)
  }
}

impl std::error::Error for AuthSessionError {}

fn error(kind: AuthSessionErrorKind, message: &str) -> AuthSessionError {
  AuthSessionError {
    kind,
    message: message.to_string(),
  }
}

/// Map a C ABI outcome to the Rust result.
fn outcome(status: i32, value: String) -> Result<String, AuthSessionError> {
  let kind = match status {
    LAUFEY_AUTH_SESSION_OK => return Ok(value),
    LAUFEY_AUTH_SESSION_CANCELLED => AuthSessionErrorKind::Cancelled,
    LAUFEY_AUTH_SESSION_NOT_SUPPORTED => AuthSessionErrorKind::NotSupported,
    LAUFEY_AUTH_SESSION_INVALID => AuthSessionErrorKind::Invalid,
    LAUFEY_AUTH_SESSION_BUSY => AuthSessionErrorKind::Busy,
    _ => AuthSessionErrorKind::Failed,
  };
  Err(AuthSessionError {
    kind,
    message: value,
  })
}

type ResultSender = tokio::sync::oneshot::Sender<(i32, String)>;

unsafe extern "C" fn auth_session_trampoline(
  user_data: *mut c_void,
  status: i32,
  value: *const c_char,
) {
  // The backend calls this exactly once per request (laufey.h), so the box
  // is reclaimed exactly once.
  let tx = unsafe { Box::from_raw(user_data as *mut ResultSender) };
  let value = if value.is_null() {
    String::new()
  } else {
    unsafe { CStr::from_ptr(value) }
      .to_string_lossy()
      .into_owned()
  };
  // The receiver may be gone (the future was dropped); nothing to do then.
  let _ = tx.send((status, value));
}

/// Run a sign-in at `url` (http or https) that ends when the browser
/// reaches `callback`: a custom scheme (`"myapp"`: a redirect to
/// `myapp:...` completes it) or, with
/// [`AuthSessionCapabilities::https_callback`], an `https://host/path` URL.
/// Resolves with the full callback URL (parse the code / state out of it
/// yourself: PKCE and `state` are the caller's).
///
/// `window_id` anchors the sheet (0: the key window, or the app's first
/// visible one). `ephemeral` asks for a private browser session: no shared
/// cookies and no "wants to use ... to sign in" prompt.
///
/// One session runs at a time per app ([`AuthSessionErrorKind::Busy`]). The
/// session starts when this function is called, not when the future is
/// first polled; dropping the future doesn't end it. Off macOS this resolves
/// with [`AuthSessionErrorKind::NotSupported`] at once. See
/// `docs/auth-session.md`.
pub fn auth_session_start(
  window_id: u32,
  url: &str,
  callback: &str,
  ephemeral: bool,
) -> impl Future<Output = Result<String, AuthSessionError>> + Send + 'static {
  auth_session_start_with(api(), window_id, url, callback, ephemeral)
}

fn auth_session_start_with(
  api: &LaufeyBackendApi,
  window_id: u32,
  url: &str,
  callback: &str,
  ephemeral: bool,
) -> impl Future<Output = Result<String, AuthSessionError>> + Send + 'static {
  let pending: Result<
    tokio::sync::oneshot::Receiver<(i32, String)>,
    AuthSessionError,
  > = match (
    api.auth_session_start,
    CString::new(url),
    CString::new(callback),
  ) {
    (None, _, _) => Err(error(
      AuthSessionErrorKind::NotSupported,
      "this backend has no OS auth session; open the system browser and \
       receive the redirect through a loopback or custom-scheme listener \
       (RFC 8252)",
    )),
    (Some(_), Err(_), _) | (Some(_), _, Err(_)) => Err(error(
      AuthSessionErrorKind::Invalid,
      "the url and callback must not contain NUL bytes",
    )),
    (Some(f), Ok(c_url), Ok(c_callback)) => {
      let (tx, rx) = tokio::sync::oneshot::channel::<(i32, String)>();
      let user_data = Box::into_raw(Box::new(tx)) as *mut c_void;
      let flags = if ephemeral {
        LAUFEY_AUTH_SESSION_EPHEMERAL
      } else {
        0
      };
      // SAFETY: the trampoline reclaims `user_data` exactly once.
      unsafe {
        f(
          api.backend_data,
          window_id,
          c_url.as_ptr(),
          c_callback.as_ptr(),
          flags,
          Some(auth_session_trampoline),
          user_data,
        );
      }
      Ok(rx)
    }
  };
  async move {
    let rx = pending?;
    match rx.await {
      Ok((status, value)) => outcome(status, value),
      Err(_) => Err(error(
        AuthSessionErrorKind::Failed,
        "the auth session ended without a result",
      )),
    }
  }
}

/// Cancels the running auth session (API 43): the app gave up on it (the
/// page cancelled, a timeout). Its sheet closes and its
/// [`auth_session_start`] future resolves
/// [`AuthSessionErrorKind::Cancelled`], once; the next session can start.
/// Returns false, and does nothing, when no session is running (always so
/// where sessions are not supported) or the backend is older than API 43.
pub fn auth_session_cancel() -> bool {
  auth_session_cancel_with(api())
}

fn auth_session_cancel_with(api: &LaufeyBackendApi) -> bool {
  match api.auth_session_cancel {
    // SAFETY: a backend vtable entry, callable from any thread.
    Some(f) => unsafe { f(api.backend_data) },
    None => false,
  }
}

/// Test-only. Ends the running auth session as the user closing its sheet
/// would (it resolves [`AuthSessionErrorKind::Cancelled`]). Returns false when
/// none is running or the backend has no hook.
pub fn test_cancel_auth_session() -> bool {
  let api = api();
  match api.test_cancel_auth_session {
    // SAFETY: a backend vtable entry, callable from any thread.
    Some(f) => unsafe { f(api.backend_data) },
    None => false,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  fn block_on<F: Future>(fut: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
      .build()
      .unwrap()
      .block_on(fut)
  }

  unsafe extern "C" fn fake_caps(_backend_data: *mut c_void) -> u32 {
    LAUFEY_AUTH_SESSION_CAP_SUPPORTED | LAUFEY_AUTH_SESSION_CAP_EPHEMERAL | 0x80
  }

  type ResultFn = unsafe extern "C" fn(*mut c_void, i32, *const c_char);

  // window 1: OK with the callback URL echoing the flags, synchronously;
  // 2: CANCELLED later from another thread; 3: a NULL value; 4: an unknown
  // status; 5: never answers (drops the sender).
  unsafe extern "C" fn fake_start(
    _backend_data: *mut c_void,
    window_id: u32,
    url: *const c_char,
    callback: *const c_char,
    flags: u32,
    on_result: Option<ResultFn>,
    user_data: *mut c_void,
  ) {
    let cb = on_result.unwrap();
    let url = unsafe { CStr::from_ptr(url) }.to_str().unwrap().to_string();
    let callback = unsafe { CStr::from_ptr(callback) }
      .to_str()
      .unwrap()
      .to_string();
    match window_id {
      1 => {
        let v =
          CString::new(format!("{callback}://done?f={flags}&u={url}")).unwrap();
        unsafe { cb(user_data, LAUFEY_AUTH_SESSION_OK, v.as_ptr()) };
      }
      2 => {
        let ud = user_data as usize;
        std::thread::spawn(move || {
          std::thread::sleep(std::time::Duration::from_millis(30));
          unsafe {
            cb(
              ud as *mut c_void,
              LAUFEY_AUTH_SESSION_CANCELLED,
              c"closed".as_ptr(),
            )
          };
        });
      }
      3 => unsafe { cb(user_data, LAUFEY_AUTH_SESSION_BUSY, std::ptr::null()) },
      4 => unsafe { cb(user_data, 99, c"odd".as_ptr()) },
      _ => drop(unsafe { Box::from_raw(user_data as *mut ResultSender) }),
    }
  }

  fn assert_send_static<T: Send + 'static>(_: &T) {}

  static CANCEL_CALLS: std::sync::atomic::AtomicU32 =
    std::sync::atomic::AtomicU32::new(0);

  unsafe extern "C" fn fake_cancel(backend_data: *mut c_void) -> bool {
    CANCEL_CALLS.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    // backend_data says whether a session is "running".
    !backend_data.is_null()
  }

  #[test]
  fn cancel_reaches_the_backend() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    // Older than API 43: nothing to call, nothing cancelled.
    assert!(!auth_session_cancel_with(&fake));
    fake.auth_session_cancel = Some(fake_cancel);
    let before = CANCEL_CALLS.load(std::sync::atomic::Ordering::SeqCst);
    assert!(!auth_session_cancel_with(&fake));
    fake.backend_data = 1 as *mut c_void;
    assert!(auth_session_cancel_with(&fake));
    assert_eq!(
      CANCEL_CALLS.load(std::sync::atomic::Ordering::SeqCst) - before,
      2
    );
  }

  #[test]
  fn without_backend_support() {
    let fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(
      auth_session_capabilities_with(&fake),
      AuthSessionCapabilities::default()
    );
    let fut =
      auth_session_start_with(&fake, 0, "https://a.com", "myapp", false);
    assert_send_static(&fut);
    let err = block_on(fut).unwrap_err();
    assert_eq!(err.kind, AuthSessionErrorKind::NotSupported);
    assert!(err.message.contains("RFC 8252"));
  }

  #[test]
  fn results_pass_through() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    fake.auth_session_capabilities = Some(fake_caps);
    fake.auth_session_start = Some(fake_start);
    assert_eq!(
      auth_session_capabilities_with(&fake),
      AuthSessionCapabilities {
        supported: true,
        ephemeral: true,
        https_callback: false,
      }
    );
    let ok = block_on(auth_session_start_with(
      &fake,
      1,
      "https://a.com/x",
      "myapp",
      true,
    ));
    assert_eq!(ok, Ok("myapp://done?f=1&u=https://a.com/x".to_string()));
    let ok = block_on(auth_session_start_with(
      &fake,
      1,
      "https://a.com/x",
      "myapp",
      false,
    ));
    assert_eq!(ok, Ok("myapp://done?f=0&u=https://a.com/x".to_string()));
    let err = block_on(auth_session_start_with(
      &fake,
      2,
      "https://a.com",
      "myapp",
      false,
    ))
    .unwrap_err();
    assert_eq!(err.kind, AuthSessionErrorKind::Cancelled);
    assert_eq!(err.message, "closed");
    assert_eq!(err.to_string(), "cancelled: closed");
    let err = block_on(auth_session_start_with(
      &fake,
      3,
      "https://a.com",
      "myapp",
      false,
    ))
    .unwrap_err();
    assert_eq!(err.kind, AuthSessionErrorKind::Busy);
    let err = block_on(auth_session_start_with(
      &fake,
      4,
      "https://a.com",
      "myapp",
      false,
    ))
    .unwrap_err();
    assert_eq!(err.kind, AuthSessionErrorKind::Failed);
    let err = block_on(auth_session_start_with(
      &fake,
      5,
      "https://a.com",
      "myapp",
      false,
    ))
    .unwrap_err();
    assert_eq!(err.kind, AuthSessionErrorKind::Failed);
    assert!(err.message.contains("without a result"));
    // NUL bytes never reach the backend.
    let err = block_on(auth_session_start_with(
      &fake,
      1,
      "https://a.com/\0",
      "myapp",
      false,
    ))
    .unwrap_err();
    assert_eq!(err.kind, AuthSessionErrorKind::Invalid);
  }
}
