// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Platform features (API 45): what this session provides, probed by the
//! backend instead of guessed from the desktop's name. See
//! `docs/platform-features.md` for the keys. May be called from any thread.

use std::ffi::c_void;
use std::sync::{Arc, Mutex, OnceLock};

use crate::io::take_backend_string;
use crate::{api, LaufeyBackendApi};

/// The backend's platform-features JSON object, or `None` when the backend
/// can't say (older than API 45, or an allocation failure).
///
/// On Linux it says whether a tray icon can be seen (`"trayHost"`, and
/// `"trayReason"` when not), what the Secret Service can do without a
/// prompt (and KWallet's state where Chromium would use it), the session type
/// and the xdg-desktop-portal versions; CEF adds the cookie store it chose
/// (`"cookieEncryption"`) and, when it waits for the OS key, why
/// (`"cookieEncryptionWait"`).
pub fn platform_features() -> Option<String> {
  platform_features_with(api())
}

fn platform_features_with(api: &LaufeyBackendApi) -> Option<String> {
  let f = api.platform_features?;
  unsafe { take_backend_string(api, f(api.backend_data)) }
}

/// Why a tray icon can't be shown here (the probe's `"trayReason"`), or
/// `None` when one can, or when the backend can't say (older than API 45).
/// Only the tray part of the probe: it never waits for xdg-desktop-portal.
/// Any thread.
pub fn tray_unavailable_reason() -> Option<String> {
  tray_unavailable_reason_with(api())
}

fn tray_unavailable_reason_with(api: &LaufeyBackendApi) -> Option<String> {
  let f = api.tray_unavailable_reason?;
  unsafe { take_backend_string(api, f(api.backend_data)) }
}

type ChangedHandler = Arc<dyn Fn() + Send + Sync>;

fn changed_handler() -> &'static Mutex<Option<ChangedHandler>> {
  static STORE: OnceLock<Mutex<Option<ChangedHandler>>> = OnceLock::new();
  STORE.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn changed_trampoline(_user_data: *mut c_void) {
  let handler = changed_handler().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler();
  }
}

/// Register the (single, process-wide) handler fired when
/// [`platform_features`] may answer differently (API 45): on Linux, when a
/// tray host (StatusNotifierWatcher) appears or goes away. A tray refused
/// for want of a host can be created again once `"trayHost"` is true. Fires
/// on the backend's UI thread (CEF, WebView) or a watcher thread (Winit);
/// never on macOS and Windows. Replaces any previous handler.
pub fn on_platform_features_changed<F>(handler: F)
where
  F: Fn() + Send + Sync + 'static,
{
  *changed_handler().lock().unwrap() = Some(Arc::new(handler));
  let api = api();
  if let Some(f) = api.set_platform_features_changed_handler {
    unsafe {
      f(
        api.backend_data,
        Some(changed_trampoline),
        std::ptr::null_mut(),
      )
    };
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::ffi::{c_char, c_void, CString};

  unsafe extern "C" fn fake_features(_: *mut c_void) -> *mut c_char {
    CString::new("{\"os\":\"linux\",\"trayHost\":false}")
      .unwrap()
      .into_raw()
  }
  unsafe extern "C" fn fake_string_free(_: *mut c_void, s: *mut c_char) {
    drop(unsafe { CString::from_raw(s) });
  }

  unsafe extern "C" fn fake_tray_refused(_: *mut c_void) -> *mut c_char {
    CString::new("no tray host").unwrap().into_raw()
  }
  unsafe extern "C" fn fake_tray_ok(_: *mut c_void) -> *mut c_char {
    std::ptr::null_mut()
  }

  #[test]
  fn tray_reason() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(tray_unavailable_reason_with(&fake), None);
    fake.string_free = Some(fake_string_free);
    fake.tray_unavailable_reason = Some(fake_tray_refused);
    assert_eq!(
      tray_unavailable_reason_with(&fake).as_deref(),
      Some("no tray host")
    );
    fake.tray_unavailable_reason = Some(fake_tray_ok);
    assert_eq!(tray_unavailable_reason_with(&fake), None);
  }

  #[test]
  fn changed_handler_runs_through_the_trampoline() {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static FIRED: AtomicUsize = AtomicUsize::new(0);
    *changed_handler().lock().unwrap() = Some(Arc::new(|| {
      FIRED.fetch_add(1, Ordering::SeqCst);
    }));
    unsafe { changed_trampoline(std::ptr::null_mut()) };
    assert_eq!(FIRED.load(Ordering::SeqCst), 1);
  }

  #[test]
  fn features() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(platform_features_with(&fake), None);
    fake.platform_features = Some(fake_features);
    fake.string_free = Some(fake_string_free);
    assert_eq!(
      platform_features_with(&fake).as_deref(),
      Some("{\"os\":\"linux\",\"trayHost\":false}")
    );
  }
}
