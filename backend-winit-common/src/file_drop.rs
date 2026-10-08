// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! File drops for the winit backends (API 39). See docs/drag-and-drop.md.
//!
//! winit 0.30 reports a file drag as one `HoveredFile` per file when it
//! enters a window, `HoveredFileCancelled` when it leaves, and one
//! `DroppedFile` per file on the drop, with no pointer position and no
//! movement events. The event loop feeds those in here ([`hovered`],
//! [`dropped`], [`cancelled`]); once it has drained its queue it calls
//! [`flush`], which turns each window's batch into one ENTER (with every
//! path), one LEAVE or one DROP for the embedder's handler. The position is
//! the last pointer position seen in that window, which is the drop point on
//! platforms that keep reporting pointer moves during a drag and stale (or
//! 0, 0) elsewhere. There is no OVER phase, no drag-out and no file dialog:
//! winit has no API for any of them.

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::path::Path;
use std::sync::Mutex;

/// `laufey_file_drop_fn`.
pub type LaufeyFileDropFn = unsafe extern "C" fn(
  *mut c_void,
  u32,
  c_int,
  f64,
  f64,
  *const *const c_char,
  usize,
);

pub const LAUFEY_DRAG_ENTER: c_int = 0;
pub const LAUFEY_DRAG_OVER: c_int = 1;
pub const LAUFEY_DRAG_LEAVE: c_int = 2;
pub const LAUFEY_DRAG_DROP: c_int = 3;
pub const LAUFEY_MAX_DROP_PATHS: usize = 4096;

/// LAUFEY_WINDOW_CAP_FILE_DROP | LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS.
pub const CAPABILITIES: u32 = (1 << 15) | (1 << 16);

/// LAUFEY_CLIPBOARD_CAP_TEXT: the winit backends shell out for plain text
/// only (see docs/clipboard.md).
pub const CLIPBOARD_CAPABILITIES: u32 = 1 << 0;

struct Handler {
  f: LaufeyFileDropFn,
  // The embedder's user data; only ever passed back to it.
  user_data: usize,
}

static HANDLER: Mutex<Option<Handler>> = Mutex::new(None);

#[derive(Default)]
struct Drag {
  hovered: Vec<String>,
  dropped: Vec<String>,
  entered: bool,
  cancelled: bool,
  x: f64,
  y: f64,
}

static DRAGS: Mutex<Option<HashMap<u32, Drag>>> = Mutex::new(None);

fn with_drag<R>(window_id: u32, f: impl FnOnce(&mut Drag) -> R) -> R {
  let mut guard = DRAGS.lock().unwrap();
  let map = guard.get_or_insert_with(HashMap::new);
  f(map.entry(window_id).or_default())
}

fn path_string(path: &Path) -> Option<String> {
  // Paths cross the ABI as UTF-8; a path that isn't valid Unicode can't be
  // named there.
  path.to_str().map(str::to_owned)
}

/// A `HoveredFile` for `window_id`, with the last pointer position seen.
pub fn hovered(window_id: u32, path: &Path, x: f64, y: f64) {
  let Some(p) = path_string(path) else { return };
  with_drag(window_id, |d| {
    if d.cancelled {
      // A new drag after a leave that wasn't flushed yet.
      *d = Drag::default();
    }
    if d.hovered.len() < LAUFEY_MAX_DROP_PATHS {
      d.hovered.push(p);
    }
    d.x = x;
    d.y = y;
  });
}

/// A `DroppedFile` for `window_id`.
pub fn dropped(window_id: u32, path: &Path, x: f64, y: f64) {
  let Some(p) = path_string(path) else { return };
  with_drag(window_id, |d| {
    if d.dropped.len() < LAUFEY_MAX_DROP_PATHS {
      d.dropped.push(p);
    }
    d.x = x;
    d.y = y;
  });
}

/// A `HoveredFileCancelled` for `window_id`.
pub fn cancelled(window_id: u32) {
  with_drag(window_id, |d| d.cancelled = true);
}

/// Forget a window's drag state (the window closed).
pub fn forget(window_id: u32) {
  if let Some(map) = DRAGS.lock().unwrap().as_mut() {
    map.remove(&window_id);
  }
}

/// Turn the batches gathered since the last call into handler calls. Call it
/// from the event loop once its queue is drained (`about_to_wait`).
pub fn flush() {
  let mut out: Vec<(u32, c_int, f64, f64, Vec<String>)> = Vec::new();
  {
    let mut guard = DRAGS.lock().unwrap();
    let Some(map) = guard.as_mut() else { return };
    let mut finished = Vec::new();
    for (&id, d) in map.iter_mut() {
      if !d.dropped.is_empty() {
        out.push((
          id,
          LAUFEY_DRAG_DROP,
          d.x,
          d.y,
          std::mem::take(&mut d.dropped),
        ));
        finished.push(id);
      } else if d.cancelled {
        if d.entered {
          out.push((id, LAUFEY_DRAG_LEAVE, d.x, d.y, Vec::new()));
        }
        finished.push(id);
      } else if !d.entered && !d.hovered.is_empty() {
        d.entered = true;
        out.push((id, LAUFEY_DRAG_ENTER, d.x, d.y, d.hovered.clone()));
      }
    }
    for id in finished {
      map.remove(&id);
    }
  }
  for (id, phase, x, y, paths) in out {
    dispatch(id, phase, x, y, &paths);
  }
}

/// Call the handler (if any) for one phase. Returns whether one was set.
pub fn dispatch(
  window_id: u32,
  phase: c_int,
  x: f64,
  y: f64,
  paths: &[String],
) -> bool {
  if !(LAUFEY_DRAG_ENTER..=LAUFEY_DRAG_DROP).contains(&phase) {
    return false;
  }
  let (f, user_data) = {
    let guard = HANDLER.lock().unwrap();
    match guard.as_ref() {
      Some(h) => (h.f, h.user_data),
      None => return false,
    }
  };
  if phase == LAUFEY_DRAG_LEAVE {
    unsafe {
      f(
        user_data as *mut c_void,
        window_id,
        phase,
        x,
        y,
        std::ptr::null(),
        0,
      )
    };
    return true;
  }
  let owned: Vec<CString> = paths
    .iter()
    .take(LAUFEY_MAX_DROP_PATHS)
    .filter_map(|p| CString::new(p.as_str()).ok())
    .collect();
  let ptrs: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
  // A drop always carries a (possibly empty) array; ENTER / OVER without
  // paths pass NULL.
  let empty: [*const c_char; 1] = [std::ptr::null()];
  let (arr, n) = if !ptrs.is_empty() {
    (ptrs.as_ptr(), ptrs.len())
  } else if phase == LAUFEY_DRAG_DROP {
    (empty.as_ptr(), 0)
  } else {
    (std::ptr::null(), 0)
  };
  unsafe { f(user_data as *mut c_void, window_id, phase, x, y, arr, n) };
  true
}

/// `set_file_drop_handler`.
///
/// # Safety
/// `handler` / `user_data` follow the laufey.h contract.
pub unsafe extern "C" fn set_file_drop_handler(
  _data: *mut c_void,
  handler: Option<LaufeyFileDropFn>,
  user_data: *mut c_void,
) {
  *HANDLER.lock().unwrap() = handler.map(|f| Handler {
    f,
    user_data: user_data as usize,
  });
}

/// `test_trigger_file_drop`: the same dispatch as a flushed OS drag, on the
/// calling thread.
///
/// # Safety
/// `paths` must point to `count` NUL-terminated strings (or be NULL).
pub unsafe extern "C" fn test_trigger_file_drop(
  _data: *mut c_void,
  window_id: u32,
  phase: c_int,
  x: f64,
  y: f64,
  paths: *const *const c_char,
  count: usize,
) -> bool {
  let mut list = Vec::new();
  if !paths.is_null() {
    for i in 0..count.min(LAUFEY_MAX_DROP_PATHS) {
      let p = unsafe { *paths.add(i) };
      if !p.is_null() {
        list.push(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned());
      }
    }
  }
  dispatch(window_id, phase, x, y, &list)
}

/// `clipboard_capabilities`.
///
/// # Safety
/// Any `data`.
pub unsafe extern "C" fn clipboard_capabilities(_data: *mut c_void) -> u32 {
  CLIPBOARD_CAPABILITIES
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::Mutex as StdMutex;

  static CALLS: StdMutex<Vec<(u32, c_int, f64, f64, Option<Vec<String>>)>> =
    StdMutex::new(Vec::new());

  unsafe extern "C" fn record(
    _ud: *mut c_void,
    window_id: u32,
    phase: c_int,
    x: f64,
    y: f64,
    paths: *const *const c_char,
    count: usize,
  ) {
    let list = if paths.is_null() {
      None
    } else {
      Some(
        (0..count)
          .map(|i| {
            unsafe { CStr::from_ptr(*paths.add(i)) }
              .to_string_lossy()
              .into_owned()
          })
          .collect(),
      )
    };
    CALLS.lock().unwrap().push((window_id, phase, x, y, list));
  }

  #[test]
  fn batches_become_one_event_per_phase() {
    unsafe {
      set_file_drop_handler(
        std::ptr::null_mut(),
        Some(record),
        std::ptr::null_mut(),
      )
    };
    CALLS.lock().unwrap().clear();
    hovered(9, Path::new("/a"), 1.0, 2.0);
    hovered(9, Path::new("/b"), 1.0, 2.0);
    flush();
    // Nothing new: no event.
    flush();
    dropped(9, Path::new("/a"), 3.0, 4.0);
    dropped(9, Path::new("/b"), 3.0, 4.0);
    flush();
    hovered(9, Path::new("/c"), 0.0, 0.0);
    flush();
    cancelled(9);
    flush();
    // Enter + leave within one batch: neither is reported.
    hovered(9, Path::new("/d"), 0.0, 0.0);
    cancelled(9);
    flush();
    let calls = CALLS.lock().unwrap().clone();
    let ab = Some(vec!["/a".to_string(), "/b".to_string()]);
    assert_eq!(
      calls,
      vec![
        (9, LAUFEY_DRAG_ENTER, 1.0, 2.0, ab.clone()),
        (9, LAUFEY_DRAG_DROP, 3.0, 4.0, ab),
        (9, LAUFEY_DRAG_ENTER, 0.0, 0.0, Some(vec!["/c".to_string()])),
        (9, LAUFEY_DRAG_LEAVE, 0.0, 0.0, None),
      ]
    );
    unsafe {
      set_file_drop_handler(std::ptr::null_mut(), None, std::ptr::null_mut())
    };
    assert!(!dispatch(9, LAUFEY_DRAG_DROP, 0.0, 0.0, &[]));
  }
}
