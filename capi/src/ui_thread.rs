// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Running code on the backend's UI thread (API 42): [`is_ui_thread`],
//! [`try_run_on_ui_thread`] (blocking) and [`spawn_on_ui_thread`] (a future),
//! over the C ABI's `dispatch_ui_task`, which guarantees every task an
//! answer: it runs on the UI thread, or it is reported as not run once the
//! backend's event loop has ended. A caller waiting for the UI thread is
//! therefore never stranded by the app quitting. `run_on_ui_thread`
//! (littledivy/laufey#79) is built on the same path.
//!
//! See `docs/c-abi.md` ("UI-thread tasks").

use std::any::Any;
use std::ffi::c_void;
use std::future::Future;

use crate::{api, try_api, LaufeyBackendApi};

/// Why a UI-thread task did not run.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UiThreadError {
  /// The backend's event loop has ended (the app is quitting): the task was
  /// not run and never will be.
  Shutdown,
  /// The backend has no `dispatch_ui_task` (an embedder's fake backend; every
  /// laufey backend of API 42 has one).
  Unsupported,
}

impl std::fmt::Display for UiThreadError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      UiThreadError::Shutdown => {
        f.write_str("the UI thread has shut down (the app is quitting)")
      }
      UiThreadError::Unsupported => {
        f.write_str("the backend cannot run tasks on its UI thread")
      }
    }
  }
}

impl std::error::Error for UiThreadError {}

/// True when called on the backend's UI thread: the thread its event loop
/// runs on (the process main thread on macOS and for the WebView backends,
/// CEF's `TID_UI`, the Winit event loop's thread). False before the backend
/// API is initialized.
pub fn is_ui_thread() -> bool {
  try_api().map(is_ui_thread_with).unwrap_or(false)
}

pub(crate) fn is_ui_thread_with(api: &LaufeyBackendApi) -> bool {
  match api.is_ui_thread {
    // SAFETY: a backend vtable entry, callable from any thread.
    Some(f) => unsafe { f(api.backend_data) },
    None => legacy_is_main_thread(),
  }
}

#[cfg(any(target_os = "macos", target_os = "ios"))]
fn legacy_is_main_thread() -> bool {
  unsafe extern "C" {
    fn pthread_main_np() -> i32;
  }
  // SAFETY: libSystem symbol on Apple.
  unsafe { pthread_main_np() != 0 }
}

#[cfg(not(any(target_os = "macos", target_os = "ios")))]
fn legacy_is_main_thread() -> bool {
  false
}

type Panic = Box<dyn Any + Send + 'static>;
/// The boxed job a dispatched task runs: `true` on the UI thread, `false`
/// when it will never run there.
type Job = Box<dyn FnOnce(bool) + Send + 'static>;

unsafe extern "C" fn job_trampoline(data: *mut c_void, ran: bool) {
  // SAFETY: `data` is the `Box<Job>` handed to dispatch_ui_task, which calls
  // this exactly once (laufey.h).
  let job = unsafe { Box::from_raw(data as *mut Job) };
  job(ran);
}

/// Hand `job` to the backend; false (and `job` dropped unrun) without
/// `dispatch_ui_task`.
fn dispatch_with(api: &LaufeyBackendApi, job: Job) -> Result<(), Job> {
  let Some(dispatch) = api.dispatch_ui_task else {
    return Err(job);
  };
  let data = Box::into_raw(Box::new(job)) as *mut c_void;
  // SAFETY: the trampoline reclaims `data` exactly once.
  unsafe { dispatch(api.backend_data, Some(job_trampoline), data) };
  Ok(())
}

fn run_caught<F, R>(f: F) -> Result<R, Panic>
where
  F: FnOnce() -> R,
{
  std::panic::catch_unwind(std::panic::AssertUnwindSafe(f))
}

/// Run `f` on the backend's UI thread and block until it returns; `f` runs
/// inline when called on the UI thread. A panic in `f` is resumed on the
/// caller's thread. Returns [`UiThreadError::Shutdown`] (without running
/// `f`) when the backend's event loop has ended or ends before `f` got to
/// run, so a runtime thread is never left waiting on a loop that is gone.
///
/// Blocking the calling thread while the UI thread waits for it deadlocks:
/// from an async runtime use [`spawn_on_ui_thread`].
pub fn try_run_on_ui_thread<F, R>(f: F) -> Result<R, UiThreadError>
where
  F: FnOnce() -> R + Send + 'static,
  R: Send + 'static,
{
  try_run_on_ui_thread_with(api(), f)
}

pub(crate) fn try_run_on_ui_thread_with<F, R>(
  api: &LaufeyBackendApi,
  f: F,
) -> Result<R, UiThreadError>
where
  F: FnOnce() -> R + Send + 'static,
  R: Send + 'static,
{
  if api.dispatch_ui_task.is_none() {
    return Err(UiThreadError::Unsupported);
  }
  if is_ui_thread_with(api) {
    return Ok(f());
  }
  let (tx, rx) = std::sync::mpsc::sync_channel::<Option<Result<R, Panic>>>(1);
  let job: Job = Box::new(move |ran| {
    let _ = tx.send(ran.then(|| run_caught(f)));
  });
  if dispatch_with(api, job).is_err() {
    return Err(UiThreadError::Unsupported);
  }
  match rx.recv() {
    Ok(Some(Ok(value))) => Ok(value),
    Ok(Some(Err(panic))) => std::panic::resume_unwind(panic),
    // Not run (the loop ended), or a backend that dropped the task.
    Ok(None) | Err(_) => Err(UiThreadError::Shutdown),
  }
}

/// Run `f` on the backend's UI thread without blocking: the returned future
/// resolves with its result. `f` is queued when this function is called (not
/// when the future is first polled), behind the work already posted to the
/// UI thread, even when called on the UI thread. A panic in `f` is resumed
/// where the future is polled. Resolves with [`UiThreadError::Shutdown`]
/// (without running `f`) when the backend's event loop has ended or ends
/// before `f` got to run. Dropping the future does not stop `f`.
pub fn spawn_on_ui_thread<F, R>(
  f: F,
) -> impl Future<Output = Result<R, UiThreadError>> + Send + 'static
where
  F: FnOnce() -> R + Send + 'static,
  R: Send + 'static,
{
  spawn_on_ui_thread_with(api(), f)
}

pub(crate) fn spawn_on_ui_thread_with<F, R>(
  api: &LaufeyBackendApi,
  f: F,
) -> impl Future<Output = Result<R, UiThreadError>> + Send + 'static
where
  F: FnOnce() -> R + Send + 'static,
  R: Send + 'static,
{
  let (tx, rx) = tokio::sync::oneshot::channel::<Option<Result<R, Panic>>>();
  let job: Job = Box::new(move |ran| {
    let _ = tx.send(ran.then(|| run_caught(f)));
  });
  let dispatched = dispatch_with(api, job).is_ok();
  async move {
    if !dispatched {
      return Err(UiThreadError::Unsupported);
    }
    match rx.await {
      Ok(Some(Ok(value))) => Ok(value),
      Ok(Some(Err(panic))) => std::panic::resume_unwind(panic),
      Ok(None) | Err(_) => Err(UiThreadError::Shutdown),
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::atomic::{AtomicBool, Ordering};
  use std::sync::Mutex;

  // A fake UI thread for dispatch_ui_task: a thread draining a queue, which
  // can be told to end (answering what is left with ran = false, as the
  // backends' UiLoopEnded does).
  struct FakeUi {
    queue: Mutex<Option<std::sync::mpsc::Sender<(usize, bool)>>>,
    thread: Mutex<Option<std::thread::ThreadId>>,
  }

  static FAKE: FakeUi = FakeUi {
    queue: Mutex::new(None),
    thread: Mutex::new(None),
  };
  static FAKE_ENDED: AtomicBool = AtomicBool::new(false);

  type TaskFn = unsafe extern "C" fn(*mut c_void, bool);

  unsafe extern "C" fn fake_dispatch(
    _backend_data: *mut c_void,
    task: Option<TaskFn>,
    data: *mut c_void,
  ) {
    let task = task.unwrap();
    if FAKE_ENDED.load(Ordering::SeqCst) {
      unsafe { task(data, false) };
      return;
    }
    // The fn pointer travels as usize, with its data.
    let packed = Box::into_raw(Box::new((task as usize, data as usize)));
    let tx = FAKE.queue.lock().unwrap().clone().unwrap();
    tx.send((packed as usize, true)).unwrap();
  }

  unsafe extern "C" fn fake_is_ui_thread(_backend_data: *mut c_void) -> bool {
    *FAKE.thread.lock().unwrap() == Some(std::thread::current().id())
  }

  fn fake_api() -> &'static LaufeyBackendApi {
    static API: std::sync::OnceLock<&'static LaufeyBackendApi> =
      std::sync::OnceLock::new();
    API.get_or_init(|| {
      let (tx, rx) = std::sync::mpsc::channel::<(usize, bool)>();
      *FAKE.queue.lock().unwrap() = Some(tx);
      let handle = std::thread::spawn(move || {
        for (packed, _) in rx {
          let (task, data) =
            *unsafe { Box::from_raw(packed as *mut (usize, usize)) };
          let task: TaskFn = unsafe { std::mem::transmute(task) };
          let ran = !FAKE_ENDED.load(Ordering::SeqCst);
          unsafe { task(data as *mut c_void, ran) };
        }
      });
      *FAKE.thread.lock().unwrap() = Some(handle.thread().id());
      let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
      fake.dispatch_ui_task = Some(fake_dispatch);
      fake.is_ui_thread = Some(fake_is_ui_thread);
      Box::leak(Box::new(fake))
    })
  }

  fn block_on<F: Future>(fut: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
      .build()
      .unwrap()
      .block_on(fut)
  }

  // One test: the fake's "ended" switch is process-wide.
  #[test]
  fn tasks_run_on_the_ui_thread_or_report_shutdown() {
    let api = fake_api();
    let ui = FAKE.thread.lock().unwrap().unwrap();

    // Blocking: runs on the UI thread, returns the value.
    let (on_ui, tid) = try_run_on_ui_thread_with(api, move || {
      let me = std::thread::current().id();
      (me == ui, me)
    })
    .unwrap();
    assert!(on_ui);
    assert_ne!(tid, std::thread::current().id());
    assert!(!is_ui_thread_with(api));

    // From the UI thread itself: inline (no deadlock).
    let nested = try_run_on_ui_thread_with(api, move || {
      try_run_on_ui_thread_with(fake_api(), || 7).unwrap()
    });
    assert_eq!(nested, Ok(7));

    // Async: queued at call time, resolves with the value.
    let fut =
      spawn_on_ui_thread_with(api, move || std::thread::current().id() == ui);
    assert_eq!(block_on(fut), Ok(true));

    // A panic on the UI thread is resumed on the caller.
    let caught = std::panic::catch_unwind(|| {
      try_run_on_ui_thread_with(api, || -> u32 { panic!("boom") })
    });
    assert!(caught.is_err());
    let caught = std::panic::catch_unwind(|| {
      block_on(spawn_on_ui_thread_with(api, || -> u32 { panic!("boom") }))
    });
    assert!(caught.is_err());

    // The loop ended: nothing runs, both paths report Shutdown.
    FAKE_ENDED.store(true, Ordering::SeqCst);
    let ran = std::sync::Arc::new(AtomicBool::new(false));
    let r2 = ran.clone();
    assert_eq!(
      try_run_on_ui_thread_with(api, move || r2.store(true, Ordering::SeqCst)),
      Err(UiThreadError::Shutdown)
    );
    let r3 = ran.clone();
    assert_eq!(
      block_on(spawn_on_ui_thread_with(api, move || {
        r3.store(true, Ordering::SeqCst)
      })),
      Err(UiThreadError::Shutdown)
    );
    assert!(!ran.load(Ordering::SeqCst));
  }

  #[test]
  fn without_dispatch_ui_task() {
    let fake: &'static LaufeyBackendApi =
      Box::leak(Box::new(unsafe { std::mem::zeroed() }));
    assert_eq!(
      try_run_on_ui_thread_with(fake, || 1),
      Err(UiThreadError::Unsupported)
    );
    assert_eq!(
      block_on(spawn_on_ui_thread_with(fake, || 1)),
      Err(UiThreadError::Unsupported)
    );
  }
}
