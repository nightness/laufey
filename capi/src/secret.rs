// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The secure store (API 47): a small secret per (service, account) in the
//! OS's secret store. On Linux the CEF and WebView backends use the Secret
//! Service through libsecret; on macOS they use the Keychain (items only the
//! app may read); elsewhere (Windows, and Winit) the entry points are NULL
//! and every call answers [`SecretError::NotSupported`]. Each call
//! blocks the calling thread for at most about `timeout` (`Duration::ZERO`:
//! the backend's default, 20 s); call it off the UI thread and off an async
//! runtime's workers. See `docs/secure-store.md`.

use std::ffi::{c_char, CString};
use std::time::Duration;

use crate::io::take_backend_string;
use crate::{api, LaufeyBackendApi};

/// Why a secure-store call didn't do what was asked.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SecretError {
  /// This backend has no secure store (Windows, Winit, older than API 47).
  NotSupported,
  /// The store can't answer: no provider, a locked keyring no one unlocked,
  /// no session bus, libsecret missing (Linux); a locked keychain, access
  /// refused, an item another program wrote in the way (macOS). The reason
  /// says which, and what to do.
  Unavailable(String),
  /// Bad arguments (empty strings, a NUL byte).
  Failed(String),
}

impl std::fmt::Display for SecretError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      SecretError::NotSupported => {
        f.write_str("this backend has no secure store")
      }
      SecretError::Unavailable(r) | SecretError::Failed(r) => f.write_str(r),
    }
  }
}

impl std::error::Error for SecretError {}

const OK: i32 = 0;
const NOT_FOUND: i32 = 1;
const UNAVAILABLE: i32 = 2;

fn c(s: &str, what: &str) -> Result<CString, SecretError> {
  CString::new(s)
    .map_err(|_| SecretError::Failed(format!("the {what} contains a NUL byte")))
}

/// The timeout in milliseconds; `Duration::ZERO` asks the backend for its
/// default (20 s).
fn millis(timeout: Duration) -> u32 {
  timeout.as_millis().min(u32::MAX as u128) as u32
}

fn error_of(
  api: &LaufeyBackendApi,
  status: i32,
  reason: *mut c_char,
) -> SecretError {
  let reason = unsafe { take_backend_string(api, reason) }
    .unwrap_or_else(|| "the secure store failed".to_string());
  if status == UNAVAILABLE {
    SecretError::Unavailable(reason)
  } else {
    SecretError::Failed(reason)
  }
}

/// Whether this backend has a secure store (API 47, Linux and macOS CEF /
/// WebView).
pub fn secret_store_supported() -> bool {
  let api = api();
  api.secret_lookup.is_some()
    && api.secret_store.is_some()
    && api.secret_delete.is_some()
}

/// The secret stored for (`service`, `account`), `None` when there is none.
pub fn secret_lookup(
  service: &str,
  account: &str,
  timeout: Duration,
) -> Result<Option<String>, SecretError> {
  secret_lookup_with(api(), service, account, timeout)
}

fn secret_lookup_with(
  api: &LaufeyBackendApi,
  service: &str,
  account: &str,
  timeout: Duration,
) -> Result<Option<String>, SecretError> {
  let f = api.secret_lookup.ok_or(SecretError::NotSupported)?;
  let (s, a) = (c(service, "service")?, c(account, "account")?);
  let mut value: *mut c_char = std::ptr::null_mut();
  let mut reason: *mut c_char = std::ptr::null_mut();
  let status = unsafe {
    f(
      api.backend_data,
      s.as_ptr(),
      a.as_ptr(),
      millis(timeout),
      &mut value,
      &mut reason,
    )
  };
  match status {
    OK => {
      unsafe { take_backend_string(api, reason) };
      Ok(Some(
        unsafe { take_backend_string(api, value) }.unwrap_or_default(),
      ))
    }
    NOT_FOUND => {
      unsafe { take_backend_string(api, reason) };
      Ok(None)
    }
    _ => Err(error_of(api, status, reason)),
  }
}

/// Store `value` for (`service`, `account`), replacing what is there.
/// `label` is what a keyring manager shows (the service when empty).
pub fn secret_store(
  service: &str,
  account: &str,
  label: &str,
  value: &str,
  timeout: Duration,
) -> Result<(), SecretError> {
  secret_store_with(api(), service, account, label, value, timeout)
}

fn secret_store_with(
  api: &LaufeyBackendApi,
  service: &str,
  account: &str,
  label: &str,
  value: &str,
  timeout: Duration,
) -> Result<(), SecretError> {
  let f = api.secret_store.ok_or(SecretError::NotSupported)?;
  let (s, a) = (c(service, "service")?, c(account, "account")?);
  let (l, v) = (c(label, "label")?, c(value, "value")?);
  let mut reason: *mut c_char = std::ptr::null_mut();
  let status = unsafe {
    f(
      api.backend_data,
      s.as_ptr(),
      a.as_ptr(),
      l.as_ptr(),
      v.as_ptr(),
      millis(timeout),
      &mut reason,
    )
  };
  if status == OK {
    unsafe { take_backend_string(api, reason) };
    return Ok(());
  }
  Err(error_of(api, status, reason))
}

/// Delete the secret for (`service`, `account`); deleting nothing is `Ok`.
pub fn secret_delete(
  service: &str,
  account: &str,
  timeout: Duration,
) -> Result<(), SecretError> {
  secret_delete_with(api(), service, account, timeout)
}

fn secret_delete_with(
  api: &LaufeyBackendApi,
  service: &str,
  account: &str,
  timeout: Duration,
) -> Result<(), SecretError> {
  let f = api.secret_delete.ok_or(SecretError::NotSupported)?;
  let (s, a) = (c(service, "service")?, c(account, "account")?);
  let mut reason: *mut c_char = std::ptr::null_mut();
  let status = unsafe {
    f(
      api.backend_data,
      s.as_ptr(),
      a.as_ptr(),
      millis(timeout),
      &mut reason,
    )
  };
  if status == OK {
    unsafe { take_backend_string(api, reason) };
    return Ok(());
  }
  Err(error_of(api, status, reason))
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::ffi::{c_void, CStr};

  unsafe extern "C" fn fake_string_free(_: *mut c_void, s: *mut c_char) {
    drop(unsafe { CString::from_raw(s) });
  }

  unsafe extern "C" fn fake_lookup(
    _: *mut c_void,
    service: *const c_char,
    account: *const c_char,
    timeout_ms: u32,
    value: *mut *mut c_char,
    reason: *mut *mut c_char,
  ) -> i32 {
    let account = unsafe { CStr::from_ptr(account) }.to_str().unwrap();
    assert_eq!(unsafe { CStr::from_ptr(service) }.to_str(), Ok("svc"));
    assert_eq!(timeout_ms, 1500);
    match account {
      "found" => {
        unsafe { *value = CString::new("s3cret").unwrap().into_raw() };
        0
      }
      "missing" => 1,
      _ => {
        unsafe {
          *reason = CString::new("the keyring is locked").unwrap().into_raw()
        };
        2
      }
    }
  }

  unsafe extern "C" fn fake_store(
    _: *mut c_void,
    _: *const c_char,
    _: *const c_char,
    label: *const c_char,
    value: *const c_char,
    _: u32,
    reason: *mut *mut c_char,
  ) -> i32 {
    assert_eq!(unsafe { CStr::from_ptr(label) }.to_str(), Ok("My App"));
    if unsafe { CStr::from_ptr(value) }.to_bytes().is_empty() {
      unsafe { *reason = CString::new("empty").unwrap().into_raw() };
      return 3;
    }
    0
  }

  unsafe extern "C" fn fake_delete(
    _: *mut c_void,
    _: *const c_char,
    _: *const c_char,
    _: u32,
    _: *mut *mut c_char,
  ) -> i32 {
    0
  }

  fn fake() -> LaufeyBackendApi {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    fake.string_free = Some(fake_string_free);
    fake.secret_lookup = Some(fake_lookup);
    fake.secret_store = Some(fake_store);
    fake.secret_delete = Some(fake_delete);
    fake
  }

  const T: Duration = Duration::from_millis(1500);

  #[test]
  fn not_supported_without_the_entry_points() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(
      secret_lookup_with(&none, "svc", "a", T),
      Err(SecretError::NotSupported)
    );
    assert_eq!(
      secret_store_with(&none, "svc", "a", "", "v", T),
      Err(SecretError::NotSupported)
    );
    assert_eq!(
      secret_delete_with(&none, "svc", "a", T),
      Err(SecretError::NotSupported)
    );
  }

  #[test]
  fn lookup_outcomes() {
    let api = fake();
    assert_eq!(
      secret_lookup_with(&api, "svc", "found", T),
      Ok(Some("s3cret".into()))
    );
    assert_eq!(secret_lookup_with(&api, "svc", "missing", T), Ok(None));
    // A locked keyring is never "not found".
    assert_eq!(
      secret_lookup_with(&api, "svc", "locked", T),
      Err(SecretError::Unavailable("the keyring is locked".into()))
    );
    assert!(matches!(
      secret_lookup_with(&api, "svc", "nul\0", T),
      Err(SecretError::Failed(_))
    ));
  }

  #[test]
  fn store_and_delete() {
    let api = fake();
    assert_eq!(
      secret_store_with(&api, "svc", "a", "My App", "v", T),
      Ok(())
    );
    assert_eq!(
      secret_store_with(&api, "svc", "a", "My App", "", T),
      Err(SecretError::Failed("empty".into()))
    );
    assert_eq!(secret_delete_with(&api, "svc", "a", T), Ok(()));
  }
}
