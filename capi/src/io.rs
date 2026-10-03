// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Drag and drop, native file dialogs and the rich clipboard (API 39).
//!
//! See `docs/drag-and-drop.md`, `docs/file-dialogs.md` and
//! `docs/clipboard.md`. Every function here may be called from any thread:
//! the backends hop to their UI thread where the platform needs it. The
//! asynchronous ones (a drag out, a dialog) return a future that resolves
//! when the OS is done; the request itself is made when the function is
//! called, not when the future is first polled.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::future::Future;
use std::sync::{Arc, Mutex, OnceLock};

use crate::{api, LaufeyBackendApi};

// --- Constants (mirror laufey.h) -------------------------------------------

pub const LAUFEY_DRAG_ENTER: i32 = 0;
pub const LAUFEY_DRAG_OVER: i32 = 1;
pub const LAUFEY_DRAG_LEAVE: i32 = 2;
pub const LAUFEY_DRAG_DROP: i32 = 3;
pub const LAUFEY_MAX_DROP_PATHS: usize = 4096;

pub const LAUFEY_DRAG_RESULT_DROPPED: i32 = 0;
pub const LAUFEY_DRAG_RESULT_CANCELLED: i32 = 1;
pub const LAUFEY_DRAG_RESULT_FAILED: i32 = 2;

pub const LAUFEY_FILE_DIALOG_OPEN: i32 = 0;
pub const LAUFEY_FILE_DIALOG_SAVE: i32 = 1;
pub const LAUFEY_FILE_DIALOG_CHOOSE_FILES: u32 = 1 << 0;
pub const LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES: u32 = 1 << 1;
pub const LAUFEY_FILE_DIALOG_MULTIPLE: u32 = 1 << 2;
pub const LAUFEY_FILE_DIALOG_SHOW_HIDDEN: u32 = 1 << 3;
pub const LAUFEY_FILE_DIALOG_NO_OVERWRITE_CONFIRM: u32 = 1 << 4;
pub const LAUFEY_FILE_DIALOG_ACCEPTED: i32 = 0;
pub const LAUFEY_FILE_DIALOG_CANCELLED: i32 = 1;
pub const LAUFEY_FILE_DIALOG_BUSY: i32 = 2;
pub const LAUFEY_FILE_DIALOG_FAILED: i32 = 3;
pub const LAUFEY_TEST_DIALOG_CANCEL: i32 = 0;
pub const LAUFEY_TEST_DIALOG_ACCEPT: i32 = 1;

pub const LAUFEY_CLIPBOARD_CAP_TEXT: u32 = 1 << 0;
pub const LAUFEY_CLIPBOARD_CAP_HTML: u32 = 1 << 1;
pub const LAUFEY_CLIPBOARD_CAP_IMAGE: u32 = 1 << 2;
pub const LAUFEY_CLIPBOARD_CAP_FORMATS: u32 = 1 << 3;
pub const LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS: u32 = 1 << 4;
pub const LAUFEY_CLIPBOARD_MAX_READ_BYTES: usize = 64 * 1024 * 1024;

// ===========================================================================
// File drops
// ===========================================================================

/// The phase of a file drag over a window.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileDragPhase {
  /// Files are dragged into the window.
  Enter,
  /// They moved over it.
  Over,
  /// The drag left the window or was cancelled.
  Leave,
  /// They were dropped on it.
  Drop,
}

impl FileDragPhase {
  pub fn from_raw(raw: i32) -> Option<Self> {
    match raw {
      LAUFEY_DRAG_ENTER => Some(Self::Enter),
      LAUFEY_DRAG_OVER => Some(Self::Over),
      LAUFEY_DRAG_LEAVE => Some(Self::Leave),
      LAUFEY_DRAG_DROP => Some(Self::Drop),
      _ => None,
    }
  }
  pub fn to_raw(self) -> i32 {
    match self {
      Self::Enter => LAUFEY_DRAG_ENTER,
      Self::Over => LAUFEY_DRAG_OVER,
      Self::Leave => LAUFEY_DRAG_LEAVE,
      Self::Drop => LAUFEY_DRAG_DROP,
    }
  }
}

/// One file-drag phase, as [`on_file_drop`] reports it.
#[derive(Debug, Clone, PartialEq)]
pub struct FileDropEvent {
  pub window_id: u32,
  pub phase: FileDragPhase,
  /// The pointer, in window content coordinates (the mouse handlers' space).
  pub x: f64,
  pub y: f64,
  /// Absolute native paths. Always `Some` for [`FileDragPhase::Drop`];
  /// `None` for `Enter` / `Over` on backends that reveal the paths only on
  /// the drop (see `WindowCapabilities::file_drop_enter_paths`), and for
  /// `Leave`.
  pub paths: Option<Vec<String>>,
  /// How many files are dragged (the paths' number when there are paths).
  pub count: usize,
}

type FileDropHandler = Arc<dyn Fn(FileDropEvent) + Send + Sync>;

fn file_drop_handler() -> &'static Mutex<Option<FileDropHandler>> {
  static SLOT: OnceLock<Mutex<Option<FileDropHandler>>> = OnceLock::new();
  SLOT.get_or_init(|| Mutex::new(None))
}

/// Read a `const char* const*` of `count` strings (lossily: the OS is the
/// source). `None` for a NULL array.
unsafe fn read_paths(
  paths: *const *const c_char,
  count: usize,
) -> Option<Vec<String>> {
  if paths.is_null() {
    return None;
  }
  let mut out = Vec::with_capacity(count.min(LAUFEY_MAX_DROP_PATHS));
  for i in 0..count.min(LAUFEY_MAX_DROP_PATHS) {
    let p = unsafe { *paths.add(i) };
    if !p.is_null() {
      out.push(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned());
    }
  }
  Some(out)
}

unsafe extern "C" fn file_drop_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  phase: c_int,
  x: f64,
  y: f64,
  paths: *const *const c_char,
  count: usize,
) {
  let Some(phase) = FileDragPhase::from_raw(phase) else {
    return;
  };
  let paths = unsafe { read_paths(paths, count) };
  let count = paths.as_ref().map(|p| p.len()).unwrap_or(count);
  let event = FileDropEvent {
    window_id,
    phase,
    x,
    y,
    paths: if phase == FileDragPhase::Drop {
      Some(paths.unwrap_or_default())
    } else {
      paths
    },
    count,
  };
  // Cloned out: the handler runs without the lock held (it may replace
  // itself).
  let handler = file_drop_handler().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(event);
  }
}

/// Register the (process-wide) file-drop handler: files dragged over a window
/// and dropped on it, with their native paths. The page keeps getting its own
/// DOM drag events (with `File` objects, never paths). The handler runs on
/// the backend UI thread; keep it short. Replaces any previous handler. A
/// no-op on backends older than API 39.
pub fn on_file_drop<F>(handler: F)
where
  F: Fn(FileDropEvent) + Send + Sync + 'static,
{
  *file_drop_handler().lock().unwrap() = Some(Arc::new(handler));
  let api = api();
  if let Some(f) = api.set_file_drop_handler {
    unsafe {
      f(
        api.backend_data,
        Some(file_drop_trampoline),
        std::ptr::null_mut(),
      )
    };
  }
}

/// Remove the file-drop handler.
pub fn clear_file_drop_handler() {
  let api = api();
  if let Some(f) = api.set_file_drop_handler {
    unsafe { f(api.backend_data, None, std::ptr::null_mut()) };
  }
  *file_drop_handler().lock().unwrap() = None;
}

/// Test-only: deliver a file-drag phase through the backend's own dispatch.
/// Returns whether a handler got it (false on backends without the hook).
pub fn test_trigger_file_drop(
  window_id: u32,
  phase: FileDragPhase,
  x: f64,
  y: f64,
  paths: &[&str],
) -> bool {
  let api = api();
  let Some(f) = api.test_trigger_file_drop else {
    return false;
  };
  let owned: Vec<CString> =
    paths.iter().filter_map(|p| CString::new(*p).ok()).collect();
  let ptrs: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
  unsafe {
    f(
      api.backend_data,
      window_id,
      phase.to_raw(),
      x,
      y,
      if ptrs.is_empty() {
        std::ptr::null()
      } else {
        ptrs.as_ptr()
      },
      ptrs.len(),
    )
  }
}

// ===========================================================================
// Drag out
// ===========================================================================

/// How a drag out of the app ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DragResult {
  /// A target accepted the files.
  Dropped,
  /// The user cancelled, or dropped where nothing took them.
  Cancelled,
  /// The drag never started (no mouse button held, another drag running, an
  /// unknown window, a bad path, or a backend without drag-out).
  Failed,
}

impl DragResult {
  pub fn from_raw(raw: i32) -> Self {
    match raw {
      LAUFEY_DRAG_RESULT_DROPPED => Self::Dropped,
      LAUFEY_DRAG_RESULT_CANCELLED => Self::Cancelled,
      _ => Self::Failed,
    }
  }
}

unsafe extern "C" fn drag_result_trampoline(
  user_data: *mut c_void,
  result: c_int,
) {
  // The backend calls this exactly once per request (laufey.h).
  let tx = unsafe {
    Box::from_raw(user_data as *mut tokio::sync::oneshot::Sender<DragResult>)
  };
  let _ = tx.send(DragResult::from_raw(result));
}

/// Drag `paths` (absolute, existing) out of window `window_id` as a copy,
/// with `icon_png` under the pointer (`None`: the platform's file icon).
/// Call it while the left mouse button is held (from the page's `dragstart`,
/// after cancelling the page's own drag). Resolves when the drag ends.
pub fn start_file_drag(
  window_id: u32,
  paths: &[&str],
  icon_png: Option<&[u8]>,
) -> impl Future<Output = DragResult> + Send + 'static {
  start_file_drag_with(api(), window_id, paths, icon_png)
}

fn start_file_drag_with(
  api: &LaufeyBackendApi,
  window_id: u32,
  paths: &[&str],
  icon_png: Option<&[u8]>,
) -> impl Future<Output = DragResult> + Send + 'static {
  let owned: Option<Vec<CString>> =
    paths.iter().map(|p| CString::new(*p).ok()).collect();
  let rx = match (api.start_file_drag, owned) {
    (Some(f), Some(owned)) if !owned.is_empty() => {
      let ptrs: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
      let (tx, rx) = tokio::sync::oneshot::channel::<DragResult>();
      let user_data = Box::into_raw(Box::new(tx)) as *mut c_void;
      let (icon, icon_len) = match icon_png {
        Some(b) if !b.is_empty() => (b.as_ptr(), b.len()),
        _ => (std::ptr::null(), 0),
      };
      unsafe {
        f(
          api.backend_data,
          window_id,
          ptrs.as_ptr(),
          ptrs.len(),
          icon,
          icon_len,
          Some(drag_result_trampoline),
          user_data,
        )
      };
      Some(rx)
    }
    _ => None,
  };
  async move {
    match rx {
      Some(rx) => rx.await.unwrap_or(DragResult::Failed),
      None => DragResult::Failed,
    }
  }
}

// ===========================================================================
// File dialogs
// ===========================================================================

/// One file-type filter: a label and extensions without the dot (`"*"`: any).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FileFilter {
  pub name: String,
  pub extensions: Vec<String>,
}

/// What kind of dialog [`show_file_dialog`] shows.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum FileDialogKind {
  #[default]
  Open,
  Save,
}

/// Options for [`show_file_dialog`]. `default_path` is a directory to start
/// in or a file path (a save dialog also proposes its name).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FileDialogOptions {
  pub kind: FileDialogKind,
  /// Open: pick files (the default when neither is set).
  pub files: bool,
  /// Open: pick directories. With `files` too, either, where
  /// `WindowCapabilities::file_dialog_files_and_directories` says so.
  pub directories: bool,
  /// Open: allow several.
  pub multiple: bool,
  pub show_hidden: bool,
  /// Save: don't confirm replacing a file (where the OS lets us).
  pub no_overwrite_confirm: bool,
  pub title: Option<String>,
  pub default_path: Option<String>,
  pub button_label: Option<String>,
  pub filters: Vec<FileFilter>,
}

impl FileDialogOptions {
  pub fn flags(&self) -> u32 {
    let mut f = 0;
    if self.files {
      f |= LAUFEY_FILE_DIALOG_CHOOSE_FILES;
    }
    if self.directories {
      f |= LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES;
    }
    if self.multiple {
      f |= LAUFEY_FILE_DIALOG_MULTIPLE;
    }
    if self.show_hidden {
      f |= LAUFEY_FILE_DIALOG_SHOW_HIDDEN;
    }
    if self.no_overwrite_confirm {
      f |= LAUFEY_FILE_DIALOG_NO_OVERWRITE_CONFIRM;
    }
    f
  }
}

/// How a file dialog ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FileDialogOutcome {
  /// The selection: one or more absolute paths.
  Accepted(Vec<String>),
  /// The user (or [`cancel_file_dialog`]) dismissed it.
  Cancelled,
  /// Another file dialog was open.
  Busy,
  /// It could not be shown (bad options, no dialogs on this backend).
  Failed,
}

/// A dialog [`show_file_dialog`] opened: its id (0 if it was answered at
/// once) and the future of its outcome.
pub struct FileDialog<F> {
  pub id: u32,
  pub outcome: F,
}

unsafe extern "C" fn file_dialog_trampoline(
  user_data: *mut c_void,
  _dialog_id: u32,
  status: c_int,
  paths: *const *const c_char,
  count: usize,
) {
  // Exactly once per request (laufey.h).
  let tx = unsafe {
    Box::from_raw(
      user_data as *mut tokio::sync::oneshot::Sender<FileDialogOutcome>,
    )
  };
  let outcome = match status {
    LAUFEY_FILE_DIALOG_ACCEPTED => match unsafe { read_paths(paths, count) } {
      Some(list) if !list.is_empty() => FileDialogOutcome::Accepted(list),
      _ => FileDialogOutcome::Cancelled,
    },
    LAUFEY_FILE_DIALOG_CANCELLED => FileDialogOutcome::Cancelled,
    LAUFEY_FILE_DIALOG_BUSY => FileDialogOutcome::Busy,
    _ => FileDialogOutcome::Failed,
  };
  let _ = tx.send(outcome);
}

/// Show the OS's open / save / folder dialog, modal to window `window_id`
/// (0: app-level). Never blocks: the outcome future resolves when the user
/// closes it. One dialog is open at a time; another meanwhile is `Busy`.
pub fn show_file_dialog(
  window_id: u32,
  options: &FileDialogOptions,
) -> FileDialog<impl Future<Output = FileDialogOutcome> + Send + 'static> {
  show_file_dialog_with(api(), window_id, options)
}

fn cstring_opt(s: &Option<String>) -> Result<Option<CString>, ()> {
  match s {
    None => Ok(None),
    Some(v) => CString::new(v.as_str()).map(Some).map_err(|_| ()),
  }
}

fn show_file_dialog_with(
  api: &LaufeyBackendApi,
  window_id: u32,
  options: &FileDialogOptions,
) -> FileDialog<impl Future<Output = FileDialogOutcome> + Send + 'static> {
  // Owned C copies of everything the options point at, alive for the call.
  struct COptions {
    title: Option<CString>,
    default_path: Option<CString>,
    button_label: Option<CString>,
    names: Vec<CString>,
    exts: Vec<Vec<CString>>,
  }
  let built: Result<COptions, ()> = (|| {
    let mut names = Vec::new();
    let mut exts = Vec::new();
    for f in &options.filters {
      names.push(CString::new(f.name.as_str()).map_err(|_| ())?);
      exts.push(
        f.extensions
          .iter()
          .map(|e| CString::new(e.as_str()).map_err(|_| ()))
          .collect::<Result<Vec<_>, _>>()?,
      );
    }
    Ok(COptions {
      title: cstring_opt(&options.title)?,
      default_path: cstring_opt(&options.default_path)?,
      button_label: cstring_opt(&options.button_label)?,
      names,
      exts,
    })
  })();
  let mut id = 0;
  let rx = match (api.show_file_dialog, built) {
    (Some(f), Ok(c)) => {
      let ext_ptrs: Vec<Vec<*const c_char>> = c
        .exts
        .iter()
        .map(|list| list.iter().map(|e| e.as_ptr()).collect())
        .collect();
      let filters: Vec<crate::ffi::laufey_file_filter_t> = c
        .names
        .iter()
        .zip(ext_ptrs.iter())
        .map(|(name, list)| crate::ffi::laufey_file_filter_t {
          name: name.as_ptr(),
          extensions: list.as_ptr(),
          extension_count: list.len(),
        })
        .collect();
      let raw = crate::ffi::laufey_file_dialog_options_t {
        kind: match options.kind {
          FileDialogKind::Open => LAUFEY_FILE_DIALOG_OPEN,
          FileDialogKind::Save => LAUFEY_FILE_DIALOG_SAVE,
        },
        flags: options.flags(),
        title: c.title.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        default_path: c
          .default_path
          .as_ref()
          .map_or(std::ptr::null(), |s| s.as_ptr()),
        button_label: c
          .button_label
          .as_ref()
          .map_or(std::ptr::null(), |s| s.as_ptr()),
        filters: if filters.is_empty() {
          std::ptr::null()
        } else {
          filters.as_ptr()
        },
        filter_count: filters.len(),
      };
      let (tx, rx) = tokio::sync::oneshot::channel::<FileDialogOutcome>();
      let user_data = Box::into_raw(Box::new(tx)) as *mut c_void;
      id = unsafe {
        f(
          api.backend_data,
          window_id,
          &raw,
          Some(file_dialog_trampoline),
          user_data,
        )
      };
      Some(rx)
    }
    _ => None,
  };
  FileDialog {
    id,
    outcome: async move {
      match rx {
        Some(rx) => rx.await.unwrap_or(FileDialogOutcome::Failed),
        None => FileDialogOutcome::Failed,
      }
    },
  }
}

/// Close an open file dialog as if the user cancelled it. False when no
/// dialog with that id is open.
pub fn cancel_file_dialog(dialog_id: u32) -> bool {
  let api = api();
  match api.cancel_file_dialog {
    Some(f) => unsafe { f(api.backend_data, dialog_id) },
    None => false,
  }
}

/// Test-only: act on the open file dialog as a user would (see laufey.h's
/// `test_file_dialog_respond`).
pub fn test_file_dialog_respond(accept: bool, path: Option<&str>) -> bool {
  let api = api();
  let Some(f) = api.test_file_dialog_respond else {
    return false;
  };
  let c = match path.map(CString::new) {
    Some(Ok(c)) => Some(c),
    Some(Err(_)) => return false,
    None => None,
  };
  unsafe {
    f(
      api.backend_data,
      if accept {
        LAUFEY_TEST_DIALOG_ACCEPT
      } else {
        LAUFEY_TEST_DIALOG_CANCEL
      },
      c.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
    )
  }
}

// ===========================================================================
// Clipboard
// ===========================================================================

/// What the clipboard can do on this backend / OS.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ClipboardCapabilities {
  pub text: bool,
  pub html: bool,
  pub image: bool,
  pub formats: bool,
  pub change_events: bool,
}

/// Any thread. Backends older than API 39 report text only, when they have
/// the text pair.
pub fn clipboard_capabilities() -> ClipboardCapabilities {
  clipboard_capabilities_with(api())
}

fn clipboard_capabilities_with(
  api: &LaufeyBackendApi,
) -> ClipboardCapabilities {
  let bits = match api.clipboard_capabilities {
    Some(f) => unsafe { f(api.backend_data) },
    None if api.read_clipboard_text.is_some() => LAUFEY_CLIPBOARD_CAP_TEXT,
    None => 0,
  };
  ClipboardCapabilities {
    text: bits & LAUFEY_CLIPBOARD_CAP_TEXT != 0,
    html: bits & LAUFEY_CLIPBOARD_CAP_HTML != 0,
    image: bits & LAUFEY_CLIPBOARD_CAP_IMAGE != 0,
    formats: bits & LAUFEY_CLIPBOARD_CAP_FORMATS != 0,
    change_events: bits & LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS != 0,
  }
}

/// Take a backend string (freed with `string_free`).
pub(crate) unsafe fn take_backend_string(
  api: &LaufeyBackendApi,
  ptr: *mut c_char,
) -> Option<String> {
  if ptr.is_null() {
    return None;
  }
  let s = unsafe { CStr::from_ptr(ptr) }
    .to_string_lossy()
    .into_owned();
  if let Some(free) = api.string_free {
    unsafe { free(api.backend_data, ptr) };
  }
  Some(s)
}

/// The clipboard's HTML, or `None` when it holds none (or more than
/// [`LAUFEY_CLIPBOARD_MAX_READ_BYTES`]).
pub fn read_clipboard_html() -> Option<String> {
  let api = api();
  let f = api.read_clipboard_html?;
  unsafe { take_backend_string(api, f(api.backend_data)) }
}

/// Replace the clipboard with `html`, plus `text` as the plain alternative.
/// False if the write failed or the backend has no HTML clipboard.
pub fn write_clipboard_html(html: &str, text: Option<&str>) -> bool {
  let api = api();
  let Some(f) = api.write_clipboard_html else {
    return false;
  };
  let Ok(h) = CString::new(html) else {
    return false;
  };
  let t = match text.map(CString::new) {
    Some(Ok(t)) => Some(t),
    Some(Err(_)) => return false,
    None => None,
  };
  unsafe {
    f(
      api.backend_data,
      h.as_ptr(),
      t.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
    )
  }
}

/// The clipboard's image as PNG bytes, or `None` when it holds none.
pub fn read_clipboard_image() -> Option<Vec<u8>> {
  read_clipboard_image_with(api())
}

fn read_clipboard_image_with(api: &LaufeyBackendApi) -> Option<Vec<u8>> {
  let f = api.read_clipboard_image?;
  let mut len: usize = 0;
  let ptr = unsafe { f(api.backend_data, &mut len) };
  if ptr.is_null() {
    return None;
  }
  let bytes = if len > 0 && len <= LAUFEY_CLIPBOARD_MAX_READ_BYTES {
    Some(unsafe { std::slice::from_raw_parts(ptr, len) }.to_vec())
  } else {
    None
  };
  if let Some(free) = api.buffer_free {
    unsafe { free(api.backend_data, ptr as *mut c_void) };
  }
  bytes
}

/// Replace the clipboard with a PNG image. False if `png` isn't a decodable
/// PNG, the write failed, or the backend has no image clipboard.
pub fn write_clipboard_image(png: &[u8]) -> bool {
  let api = api();
  match api.write_clipboard_image {
    Some(f) if !png.is_empty() => unsafe {
      f(api.backend_data, png.as_ptr(), png.len())
    },
    _ => false,
  }
}

/// The kinds of content on the clipboard as MIME types (`text/plain`,
/// `text/html`, `image/png`, `text/uri-list`, `text/rtf`); empty for an empty
/// clipboard, `None` when the backend can't tell.
pub fn read_clipboard_formats() -> Option<Vec<String>> {
  read_clipboard_formats_with(api())
}

fn read_clipboard_formats_with(api: &LaufeyBackendApi) -> Option<Vec<String>> {
  let f = api.read_clipboard_formats?;
  let joined = unsafe { take_backend_string(api, f(api.backend_data)) }?;
  Some(
    joined
      .split('\n')
      .filter(|s| !s.is_empty())
      .map(str::to_owned)
      .collect(),
  )
}

type ClipboardChangeHandler = Arc<dyn Fn() + Send + Sync>;

fn clipboard_change_handler() -> &'static Mutex<Option<ClipboardChangeHandler>>
{
  static SLOT: OnceLock<Mutex<Option<ClipboardChangeHandler>>> =
    OnceLock::new();
  SLOT.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn clipboard_change_trampoline(_user_data: *mut c_void) {
  // Cloned out: the handler runs without the lock held (it may replace
  // itself).
  let handler = clipboard_change_handler().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler();
  }
}

/// Call `handler` (on the backend UI thread) whenever the system clipboard
/// changes. macOS polls the pasteboard only while a handler is set, so clear
/// it when nobody listens. See `ClipboardCapabilities::change_events`.
pub fn on_clipboard_change<F>(handler: F)
where
  F: Fn() + Send + Sync + 'static,
{
  *clipboard_change_handler().lock().unwrap() = Some(Arc::new(handler));
  let api = api();
  if let Some(f) = api.set_clipboard_change_handler {
    unsafe {
      f(
        api.backend_data,
        Some(clipboard_change_trampoline),
        std::ptr::null_mut(),
      )
    };
  }
}

/// Stop clipboard change events (and the macOS poll).
pub fn clear_clipboard_change_handler() {
  let api = api();
  if let Some(f) = api.set_clipboard_change_handler {
    unsafe { f(api.backend_data, None, std::ptr::null_mut()) };
  }
  *clipboard_change_handler().lock().unwrap() = None;
}

#[cfg(test)]
mod tests {
  use super::*;

  // Handlers run after the slot's lock is released, so one that replaces
  // itself (or calls back into the laufey API) cannot deadlock.
  #[test]
  fn io_handlers_run_without_the_slot_lock() {
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
      *clipboard_change_handler().lock().unwrap() = Some(Arc::new(|| {
        *clipboard_change_handler().lock().unwrap() = None;
      }));
      unsafe { clipboard_change_trampoline(std::ptr::null_mut()) };
      *file_drop_handler().lock().unwrap() =
        Some(Arc::new(|_: FileDropEvent| {
          *file_drop_handler().lock().unwrap() = None;
        }));
      unsafe {
        file_drop_trampoline(
          std::ptr::null_mut(),
          1,
          LAUFEY_DRAG_DROP,
          0.0,
          0.0,
          std::ptr::null(),
          0,
        );
      }
      let _ = tx.send(
        clipboard_change_handler().lock().unwrap().is_none()
          && file_drop_handler().lock().unwrap().is_none(),
      );
    });
    let cleared = rx
      .recv_timeout(std::time::Duration::from_secs(10))
      .expect("a handler deadlocked on its own slot");
    assert!(cleared);
  }

  fn block_on<F: Future>(fut: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
      .build()
      .unwrap()
      .block_on(fut)
  }

  #[test]
  fn phases_round_trip() {
    for p in [
      FileDragPhase::Enter,
      FileDragPhase::Over,
      FileDragPhase::Leave,
      FileDragPhase::Drop,
    ] {
      assert_eq!(FileDragPhase::from_raw(p.to_raw()), Some(p));
    }
    assert_eq!(FileDragPhase::from_raw(9), None);
    assert_eq!(DragResult::from_raw(0), DragResult::Dropped);
    assert_eq!(DragResult::from_raw(1), DragResult::Cancelled);
    assert_eq!(DragResult::from_raw(7), DragResult::Failed);
  }

  // --- drag out ---

  unsafe extern "C" fn fake_drag(
    _: *mut c_void,
    window_id: u32,
    paths: *const *const c_char,
    count: usize,
    icon: *const u8,
    icon_len: usize,
    cb: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
    ud: *mut c_void,
  ) {
    let list = unsafe { read_paths(paths, count) }.unwrap();
    let ok = window_id == 4
      && list == vec!["/a".to_string(), "/b".to_string()]
      && ((icon.is_null() && icon_len == 0) || icon_len == 3);
    unsafe {
      cb.unwrap()(
        ud,
        if ok {
          LAUFEY_DRAG_RESULT_DROPPED
        } else {
          LAUFEY_DRAG_RESULT_FAILED
        },
      )
    };
  }

  #[test]
  fn start_file_drag_resolves_once() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    // No backend support: Failed without calling anything.
    assert_eq!(
      block_on(start_file_drag_with(&fake, 4, &["/a"], None)),
      DragResult::Failed
    );
    fake.start_file_drag = Some(fake_drag);
    assert_eq!(
      block_on(start_file_drag_with(&fake, 4, &["/a", "/b"], None)),
      DragResult::Dropped
    );
    assert_eq!(
      block_on(start_file_drag_with(
        &fake,
        4,
        &["/a", "/b"],
        Some(&[1, 2, 3])
      )),
      DragResult::Dropped
    );
    // A NUL in a path or no paths never reach the backend.
    assert_eq!(
      block_on(start_file_drag_with(&fake, 4, &["/a\0b"], None)),
      DragResult::Failed
    );
    assert_eq!(
      block_on(start_file_drag_with(&fake, 4, &[], None)),
      DragResult::Failed
    );
  }

  // --- dialogs ---

  unsafe extern "C" fn fake_dialog(
    _: *mut c_void,
    window_id: u32,
    options: *const crate::ffi::laufey_file_dialog_options_t,
    cb: Option<
      unsafe extern "C" fn(
        *mut c_void,
        u32,
        c_int,
        *const *const c_char,
        usize,
      ),
    >,
    ud: *mut c_void,
  ) -> u32 {
    let o = unsafe { &*options };
    let title = unsafe { CStr::from_ptr(o.title) }.to_str().unwrap();
    assert!(o.default_path.is_null());
    let f0 = unsafe { &*o.filters };
    let e1 = unsafe { CStr::from_ptr(*f0.extensions.add(1)) }
      .to_str()
      .unwrap();
    let cb = cb.unwrap();
    match title {
      "busy" => {
        unsafe { cb(ud, 0, LAUFEY_FILE_DIALOG_BUSY, std::ptr::null(), 0) };
        0
      }
      "pick" => {
        assert_eq!(window_id, 2);
        assert_eq!(o.kind, LAUFEY_FILE_DIALOG_OPEN);
        assert_eq!(
          o.flags,
          LAUFEY_FILE_DIALOG_MULTIPLE | LAUFEY_FILE_DIALOG_CHOOSE_FILES
        );
        assert_eq!(o.filter_count, 1);
        assert_eq!(f0.extension_count, 2);
        assert_eq!(e1, "jpg");
        let a = CString::new("/x.png").unwrap();
        let b = CString::new("/y.jpg").unwrap();
        let arr = [a.as_ptr(), b.as_ptr()];
        unsafe { cb(ud, 9, LAUFEY_FILE_DIALOG_ACCEPTED, arr.as_ptr(), 2) };
        9
      }
      _ => {
        unsafe { cb(ud, 5, LAUFEY_FILE_DIALOG_CANCELLED, std::ptr::null(), 0) };
        5
      }
    }
  }

  fn opts(title: &str) -> FileDialogOptions {
    FileDialogOptions {
      files: true,
      multiple: true,
      title: Some(title.into()),
      filters: vec![FileFilter {
        name: "Images".into(),
        extensions: vec!["png".into(), "jpg".into()],
      }],
      ..Default::default()
    }
  }

  #[test]
  fn file_dialog_outcomes() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let d = show_file_dialog_with(&fake, 2, &opts("pick"));
    assert_eq!(d.id, 0);
    assert_eq!(block_on(d.outcome), FileDialogOutcome::Failed);
    fake.show_file_dialog = Some(fake_dialog);
    let d = show_file_dialog_with(&fake, 2, &opts("pick"));
    assert_eq!(d.id, 9);
    assert_eq!(
      block_on(d.outcome),
      FileDialogOutcome::Accepted(vec!["/x.png".into(), "/y.jpg".into()])
    );
    let d = show_file_dialog_with(&fake, 2, &opts("busy"));
    assert_eq!((d.id, block_on(d.outcome)), (0, FileDialogOutcome::Busy));
    let d = show_file_dialog_with(&fake, 2, &opts("other"));
    assert_eq!(block_on(d.outcome), FileDialogOutcome::Cancelled);
    // A NUL in an option is refused before the backend.
    let mut bad = opts("pick");
    bad.title = Some("a\0b".into());
    let d = show_file_dialog_with(&fake, 2, &bad);
    assert_eq!(block_on(d.outcome), FileDialogOutcome::Failed);
  }

  #[test]
  fn dialog_flags() {
    let o = FileDialogOptions {
      directories: true,
      show_hidden: true,
      no_overwrite_confirm: true,
      ..Default::default()
    };
    assert_eq!(
      o.flags(),
      LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES
        | LAUFEY_FILE_DIALOG_SHOW_HIDDEN
        | LAUFEY_FILE_DIALOG_NO_OVERWRITE_CONFIRM
    );
  }

  // --- clipboard ---

  unsafe extern "C" fn fake_caps(_: *mut c_void) -> u32 {
    LAUFEY_CLIPBOARD_CAP_TEXT | LAUFEY_CLIPBOARD_CAP_IMAGE
  }
  unsafe extern "C" fn fake_read_text(_: *mut c_void) -> *mut c_char {
    std::ptr::null_mut()
  }

  #[test]
  fn clipboard_caps() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(
      clipboard_capabilities_with(&fake),
      ClipboardCapabilities::default()
    );
    // A pre-39 backend with the text pair: text only.
    fake.read_clipboard_text = Some(fake_read_text);
    assert!(clipboard_capabilities_with(&fake).text);
    assert!(!clipboard_capabilities_with(&fake).html);
    fake.clipboard_capabilities = Some(fake_caps);
    let caps = clipboard_capabilities_with(&fake);
    assert!(caps.text && caps.image && !caps.html && !caps.change_events);
  }

  static FREED: std::sync::atomic::AtomicUsize =
    std::sync::atomic::AtomicUsize::new(0);

  unsafe extern "C" fn fake_image(_: *mut c_void, len: *mut usize) -> *mut u8 {
    let v: Box<[u8]> = vec![0x89, b'P', b'N', b'G'].into_boxed_slice();
    unsafe { *len = v.len() };
    Box::into_raw(v) as *mut u8
  }
  unsafe extern "C" fn fake_buffer_free(_: *mut c_void, p: *mut c_void) {
    let _ = unsafe {
      Box::from_raw(std::ptr::slice_from_raw_parts_mut(p as *mut u8, 4))
    };
    FREED.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
  }
  unsafe extern "C" fn fake_formats(_: *mut c_void) -> *mut c_char {
    CString::new("text/plain\ntext/html\n").unwrap().into_raw()
  }
  unsafe extern "C" fn fake_string_free(_: *mut c_void, s: *mut c_char) {
    let _ = unsafe { CString::from_raw(s) };
  }

  #[test]
  fn clipboard_reads_copy_and_free() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(read_clipboard_image_with(&fake), None);
    assert_eq!(read_clipboard_formats_with(&fake), None);
    fake.read_clipboard_image = Some(fake_image);
    fake.buffer_free = Some(fake_buffer_free);
    fake.read_clipboard_formats = Some(fake_formats);
    fake.string_free = Some(fake_string_free);
    assert_eq!(
      read_clipboard_image_with(&fake),
      Some(vec![0x89, b'P', b'N', b'G'])
    );
    assert_eq!(FREED.load(std::sync::atomic::Ordering::SeqCst), 1);
    assert_eq!(
      read_clipboard_formats_with(&fake),
      Some(vec!["text/plain".to_string(), "text/html".to_string()])
    );
  }
}
