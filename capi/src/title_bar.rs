// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Title bar preferences (API 47): how the user set up title bars, for an
//! app that draws its own (a hidden title bar with a drag region): the window
//! buttons on each side, what a double click on a title bar does, the colour
//! scheme, the accent colour and the title bar font. See
//! `docs/title-bar.md` for the JSON object. May be called from any thread.

use std::ffi::c_void;
use std::sync::{Arc, Mutex, OnceLock};

use crate::io::take_backend_string;
use crate::{api, LaufeyBackendApi};

/// The backend's title-bar-preferences JSON object, or `None` when the
/// backend can't say (older than API 47, or an allocation failure). On Linux
/// the first call may wait a few seconds for xdg-desktop-portal to start.
pub fn title_bar_preferences() -> Option<String> {
  title_bar_preferences_with(api())
}

fn title_bar_preferences_with(api: &LaufeyBackendApi) -> Option<String> {
  let f = api.title_bar_preferences?;
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
/// [`title_bar_preferences`] answers differently (API 47): the user moved
/// the window buttons, or changed the double-click action, colour scheme or
/// accent colour. Fires on a watcher thread (Linux, Windows) or the main
/// thread (macOS). Replaces any previous handler. Returns `false` when the
/// backend can't report changes (older than API 47).
pub fn on_title_bar_preferences_changed<F>(handler: F) -> bool
where
  F: Fn() + Send + Sync + 'static,
{
  *changed_handler().lock().unwrap() = Some(Arc::new(handler));
  let api = api();
  match api.set_title_bar_preferences_changed_handler {
    Some(f) => {
      unsafe {
        f(
          api.backend_data,
          Some(changed_trampoline),
          std::ptr::null_mut(),
        )
      };
      true
    }
    None => false,
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::ffi::{c_char, CString};

  const JSON: &str = "{\"buttons\":{\"left\":[\"close\"],\"right\":[]},\
                      \"side\":\"left\"}";

  unsafe extern "C" fn fake_preferences(_: *mut c_void) -> *mut c_char {
    CString::new(JSON).unwrap().into_raw()
  }
  unsafe extern "C" fn fake_string_free(_: *mut c_void, s: *mut c_char) {
    drop(unsafe { CString::from_raw(s) });
  }

  #[test]
  fn preferences() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(title_bar_preferences_with(&fake), None);
    fake.title_bar_preferences = Some(fake_preferences);
    fake.string_free = Some(fake_string_free);
    assert_eq!(title_bar_preferences_with(&fake).as_deref(), Some(JSON));
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
}
