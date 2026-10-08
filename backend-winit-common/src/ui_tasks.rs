// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! `dispatch_ui_task` / `is_ui_thread` (API 42): UI-thread tasks with a
//! delivery guarantee, the Rust twin of backend-common's
//! `laufey_ui_tasks.h`.
//!
//! A task posted as a `UiTask` user event runs when the event loop gets to
//! it, but once the loop has exited nothing ever does, so a runtime thread
//! waiting for it would wait forever (and with it the backend's shutdown,
//! which waits for the runtime thread). Every task dispatched here is kept
//! until it runs; [`close`] (the loop has ended) answers each one still
//! pending with `ran = false`, and later dispatches are answered at once. The
//! posted event carries an id, not the task, so an event delivered after
//! [`close`] does nothing.

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::LazyLock;
use std::sync::Mutex;
use std::thread::ThreadId;

/// `laufey_ui_task_fn`.
pub type UiTaskFn = unsafe extern "C" fn(*mut c_void, bool);

struct State {
  closed: bool,
  next_id: usize,
  pending: HashMap<usize, (UiTaskFn, usize)>,
  ui_thread: Option<ThreadId>,
}

static STATE: LazyLock<Mutex<State>> = LazyLock::new(|| {
  Mutex::new(State {
    closed: false,
    next_id: 0,
    pending: HashMap::new(),
    ui_thread: None,
  })
});

fn state() -> std::sync::MutexGuard<'static, State> {
  STATE.lock().unwrap_or_else(|e| e.into_inner())
}

/// Record the calling thread (the one that runs the event loop) as the UI
/// thread. Call before the runtime starts.
pub fn bind_current_thread() {
  state().ui_thread = Some(std::thread::current().id());
}

/// True on the thread [`bind_current_thread`] ran on.
pub fn is_ui_thread() -> bool {
  state().ui_thread == Some(std::thread::current().id())
}

/// See `dispatch_ui_task` in laufey.h. `post` queues `run(data)` on the
/// event loop and reports whether it could.
pub fn dispatch(
  task: UiTaskFn,
  data: usize,
  post: impl FnOnce(unsafe extern "C" fn(*mut c_void), *mut c_void) -> bool,
) {
  let id = {
    let mut s = state();
    if s.closed {
      None
    } else {
      s.next_id += 1;
      let id = s.next_id;
      s.pending.insert(id, (task, data));
      Some(id)
    }
  };
  let Some(id) = id else {
    // SAFETY: the caller's task, called exactly once.
    unsafe { task(data as *mut c_void, false) };
    return;
  };
  if !post(run, id as *mut c_void) {
    cancel(id);
  }
}

/// The posted event: run the task if it is still pending.
unsafe extern "C" fn run(raw: *mut c_void) {
  let id = raw as usize;
  let entry = state().pending.remove(&id);
  if let Some((task, data)) = entry {
    // SAFETY: the caller's task, called exactly once.
    unsafe { task(data as *mut c_void, true) };
  }
}

fn cancel(id: usize) {
  let entry = state().pending.remove(&id);
  if let Some((task, data)) = entry {
    // SAFETY: as in `run`.
    unsafe { task(data as *mut c_void, false) };
  }
}

/// The event loop has ended: answer every pending task with `ran = false`
/// (on this thread) and every later dispatch at once. Idempotent.
pub fn close() {
  let pending = {
    let mut s = state();
    s.closed = true;
    std::mem::take(&mut s.pending)
  };
  for (_, (task, data)) in pending {
    // SAFETY: as in `run`.
    unsafe { task(data as *mut c_void, false) };
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::atomic::{AtomicUsize, Ordering};

  static RAN: AtomicUsize = AtomicUsize::new(0);
  static NOT_RAN: AtomicUsize = AtomicUsize::new(0);

  unsafe extern "C" fn probe(_data: *mut c_void, ran: bool) {
    if ran {
      RAN.fetch_add(1, Ordering::SeqCst);
    } else {
      NOT_RAN.fetch_add(1, Ordering::SeqCst);
    }
  }

  // One test: the state is process-wide.
  #[test]
  fn tasks_run_once_or_are_cancelled_once() {
    std::thread::spawn(bind_current_thread).join().unwrap();
    assert!(!is_ui_thread());
    bind_current_thread();
    assert!(is_ui_thread());

    // Posted and run.
    let mut queued: Vec<(unsafe extern "C" fn(*mut c_void), usize)> =
      Vec::new();
    dispatch(probe, 0, |f, d| {
      queued.push((f, d as usize));
      true
    });
    let (f, d) = queued.pop().unwrap();
    unsafe { f(d as *mut c_void) };
    unsafe { f(d as *mut c_void) }; // a duplicate delivery is ignored
    assert_eq!(RAN.load(Ordering::SeqCst), 1);

    // Refused post: cancelled at once.
    dispatch(probe, 0, |_, _| false);
    assert_eq!(NOT_RAN.load(Ordering::SeqCst), 1);

    // Pending when the loop ends: cancelled by close, the late event is
    // ignored, later dispatches are answered at once.
    dispatch(probe, 0, |f, d| {
      queued.push((f, d as usize));
      true
    });
    close();
    assert_eq!(NOT_RAN.load(Ordering::SeqCst), 2);
    let (f, d) = queued.pop().unwrap();
    unsafe { f(d as *mut c_void) };
    assert_eq!(RAN.load(Ordering::SeqCst), 1);
    dispatch(probe, 0, |_, _| panic!("must not post after close"));
    assert_eq!(NOT_RAN.load(Ordering::SeqCst), 3);
  }
}
