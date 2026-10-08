// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The dialogs of the winit / servo backends: the prompt
//! (`LAUFEY_DIALOG_PROMPT`) everywhere, and on Linux the alert and confirm
//! too.
//!
//! The title, message and default value may come from page content, so none
//! of them is ever part of a command line a shell parses or of a script's
//! source, and each is shown as plain text:
//! - Windows: an in-process Win32 dialog (an in-memory template); each string
//!   is a control's text. (Alert and confirm: rfd, lib.rs.)
//! - macOS: `osascript` runs a fixed script; the strings are its `argv`
//!   (after `--`, so one starting with `-` is not an option). (Alert and
//!   confirm: rfd.)
//! - Linux, every dialog: the first provider this session has, in the same
//!   order for all three (`mod linux`): `kdialog` (Plasma's own dialogs),
//!   `zenity` (GNOME's), then an in-process GTK dialog on a GTK thread of
//!   its own (`gtk_thread`; the main path where neither tool is installed,
//!   as on Fedora). xdg-desktop-portal has no text-input portal, so there
//!   is no portal step. The tools run directly (never through a shell),
//!   each string an `--opt=value` argument or after `--`, never a separate
//!   argument that could read as an option. A tool that finds no display
//!   is not a cancel: the next provider runs. With none of them (no
//!   display, or neither tool nor a GTK display), the result is
//!   [`DialogOutcome::Unsupported`]: nothing was shown, which is not a
//!   cancel.
//!
//! Returns a [`DialogOutcome`].

/// What a dialog came to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DialogOutcome {
  /// OK; a prompt's text.
  Confirmed(Option<String>),
  /// The user dismissed it.
  Cancelled,
  /// No way to show it here; nothing was shown.
  Unsupported,
}

#[cfg(target_os = "macos")]
pub(crate) fn show_prompt_dialog(
  title: &str,
  message: &str,
  default_value: &str,
) -> DialogOutcome {
  match std::process::Command::new("osascript")
    .args(osascript_prompt_args(title, message, default_value))
    .output()
  {
    Ok(output) if output.status.success() => {
      let text = String::from_utf8_lossy(&output.stdout);
      DialogOutcome::Confirmed(Some(strip_one_newline(&text).to_string()))
    }
    _ => DialogOutcome::Cancelled,
  }
}

/// `osascript` arguments: a fixed script, then the strings as `argv`.
#[cfg(any(target_os = "macos", test))]
fn osascript_prompt_args(
  title: &str,
  message: &str,
  default_value: &str,
) -> Vec<String> {
  [
    "-e",
    "on run argv",
    "-e",
    "set r to display dialog (item 1 of argv) default answer (item 2 of argv) \
     with title (item 3 of argv) buttons {\"Cancel\", \"OK\"} default button \
     \"OK\"",
    "-e",
    "return text returned of r",
    "-e",
    "end run",
    "--",
    message,
    default_value,
    title,
  ]
  .iter()
  .map(|s| s.to_string())
  .collect()
}

/// The tool's output ends with one newline that isn't the user's.
#[cfg(any(target_os = "macos", target_os = "linux", test))]
fn strip_one_newline(s: &str) -> &str {
  s.strip_suffix('\n').unwrap_or(s)
}

/// Which dialog (`LAUFEY_DIALOG_ALERT` / `_CONFIRM` / `_PROMPT`).
#[cfg(any(target_os = "linux", test))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Kind {
  Alert,
  Confirm,
  Prompt,
}

#[cfg(target_os = "linux")]
pub(crate) use linux::show_dialog;

#[cfg(any(target_os = "linux", test))]
mod linux {
  use super::{strip_one_newline, DialogOutcome, Kind};

  /// An external dialog tool.
  #[derive(Debug, Clone, Copy, PartialEq, Eq)]
  pub(super) enum Tool {
    Kdialog,
    Zenity,
  }

  impl Tool {
    pub(super) fn program(self) -> &'static str {
      match self {
        Tool::Kdialog => "kdialog",
        Tool::Zenity => "zenity",
      }
    }

    pub(super) fn args(
      self,
      kind: Kind,
      title: &str,
      message: &str,
      default_value: &str,
    ) -> Vec<String> {
      match self {
        Tool::Kdialog => kdialog_args(kind, title, message, default_value),
        Tool::Zenity => zenity_args(kind, title, message, default_value),
      }
    }
  }

  /// The order the tools are tried in (the same for every dialog).
  pub(super) const TOOLS: [Tool; 2] = [Tool::Kdialog, Tool::Zenity];

  /// What running a tool came to.
  #[derive(Debug, Clone, PartialEq, Eq)]
  pub(super) enum Run {
    /// It isn't installed (or couldn't start): try the next provider.
    Missing,
    /// It ran: its exit code (None when a signal ended it), stdout, stderr.
    Exited(Option<i32>, String, String),
  }

  /// The tool could not show anything: it found no display. zenity 4.x
  /// exits 1 (the cancel code) with "Gtk-WARNING: Failed to open display"
  /// (zenity 3.x: "cannot open display"); Qt tools (kdialog) print "could
  /// not connect to display" and abort.
  pub(super) fn display_failure(stderr: &str) -> bool {
    let e = stderr.to_ascii_lowercase();
    e.contains("open display")
      || e.contains("connect to display")
      || e.contains("unable to init server")
  }

  /// What a tool's exit means, or `None` when it showed nothing (try the
  /// next provider). Exit 0 is OK; 1 is the user dismissing it (for an
  /// alert that is still "seen"), unless the tool says it found no display;
  /// anything else (a crash, a signal, an unknown option) is not an answer.
  pub(super) fn outcome(kind: Kind, run: Run) -> Option<DialogOutcome> {
    let Run::Exited(code, stdout, stderr) = run else {
      return None;
    };
    match code {
      Some(0) => Some(DialogOutcome::Confirmed(
        (kind == Kind::Prompt).then(|| strip_one_newline(&stdout).to_string()),
      )),
      Some(1) if display_failure(&stderr) => None,
      Some(1) if kind == Kind::Alert => Some(DialogOutcome::Confirmed(None)),
      Some(1) => Some(DialogOutcome::Cancelled),
      _ => None,
    }
  }

  /// Try each tool, then the in-process GTK dialog. `run` starts a tool
  /// (argv, never a shell); `in_process` shows the GTK dialog, or `None`
  /// when GTK has no display. With no display at all nothing is tried.
  pub(super) fn dialog_with(
    kind: Kind,
    title: &str,
    message: &str,
    default_value: &str,
    have_display: bool,
    run: impl Fn(Tool, &[String]) -> Run,
    in_process: impl FnOnce() -> Option<DialogOutcome>,
  ) -> DialogOutcome {
    if !have_display {
      return DialogOutcome::Unsupported;
    }
    for tool in TOOLS {
      let args = tool.args(kind, title, message, default_value);
      if let Some(outcome) = outcome(kind, run(tool, &args)) {
        return outcome;
      }
    }
    in_process().unwrap_or(DialogOutcome::Unsupported)
  }

  /// Page text for a kdialog label, shown as plain text. kdialog first
  /// unescapes `\\` and `\n` (Utils::parseString), then Qt shows the label
  /// as rich text when it looks like HTML (Qt::AutoText): so the text is
  /// HTML-escaped inside `<qt>`, newlines kept by `white-space:pre-wrap`,
  /// and every backslash doubled. `mnemonic`: the label has a buddy (the
  /// input box's), so Qt strips each `&` from the document as a mnemonic
  /// marker, and a literal one is written `&&`.
  pub(super) fn kdialog_text(text: &str, mnemonic: bool) -> String {
    let mut html = String::new();
    for c in text.chars() {
      match c {
        '&' if mnemonic => html.push_str("&amp;&amp;"),
        '&' => html.push_str("&amp;"),
        '<' => html.push_str("&lt;"),
        '>' => html.push_str("&gt;"),
        '"' => html.push_str("&quot;"),
        '\n' => html.push_str("<br>"),
        c => html.push(c),
      }
    }
    format!("<qt><p style=\"white-space:pre-wrap\">{html}</p></qt>")
      .replace('\\', "\\\\")
  }

  /// kdialog arguments. Every string is in the same argument as its
  /// option (`--opt=value`) or after `--`, so none can read as an option.
  pub(super) fn kdialog_args(
    kind: Kind,
    title: &str,
    message: &str,
    default_value: &str,
  ) -> Vec<String> {
    let mut args = vec![format!("--title={title}")];
    match kind {
      Kind::Alert => {
        args.push(format!("--msgbox={}", kdialog_text(message, false)))
      }
      Kind::Confirm => args.extend([
        format!("--yesno={}", kdialog_text(message, false)),
        "--yes-label=OK".to_string(),
        "--no-label=Cancel".to_string(),
      ]),
      Kind::Prompt => args.extend([
        format!("--inputbox={}", kdialog_text(message, true)),
        "--".to_string(),
        default_value.to_string(),
      ]),
    }
    args
  }

  /// zenity arguments (checked against zenity 4.2's source: msg.c,
  /// entry.c). An alert or confirm takes `--no-markup`, so `--text` is
  /// shown with gtk_label_set_text, as given. The entry dialog has no
  /// `--no-markup`: it shows `--text` with
  /// `gtk_label_set_text_with_mnemonic(g_strcompress(text))`, so a literal
  /// backslash is written `\\` and a literal underscore `__`. Titles and
  /// the entry text are shown as given.
  pub(super) fn zenity_args(
    kind: Kind,
    title: &str,
    message: &str,
    default_value: &str,
  ) -> Vec<String> {
    let title = format!("--title={title}");
    match kind {
      Kind::Alert => vec![
        "--info".to_string(),
        title,
        "--no-markup".to_string(),
        format!("--text={message}"),
      ],
      Kind::Confirm => vec![
        "--question".to_string(),
        title,
        "--no-markup".to_string(),
        format!("--text={message}"),
        "--ok-label=OK".to_string(),
        "--cancel-label=Cancel".to_string(),
      ],
      Kind::Prompt => {
        let text = message.replace('\\', "\\\\").replace('_', "__");
        vec![
          "--entry".to_string(),
          title,
          format!("--text={text}"),
          format!("--entry-text={default_value}"),
        ]
      }
    }
  }

  /// The dialog, from the first provider this session has.
  #[cfg(target_os = "linux")]
  pub(crate) fn show_dialog(
    kind: Kind,
    title: &str,
    message: &str,
    default_value: &str,
  ) -> DialogOutcome {
    let have_display = std::env::var_os("WAYLAND_DISPLAY")
      .is_some_and(|v| !v.is_empty())
      || std::env::var_os("DISPLAY").is_some_and(|v| !v.is_empty());
    let (title_s, message_s, default_s) = (
      title.to_string(),
      message.to_string(),
      default_value.to_string(),
    );
    dialog_with(
      kind,
      title,
      message,
      default_value,
      have_display,
      |tool, args| match std::process::Command::new(tool.program())
        .args(args)
        .stdin(std::process::Stdio::null())
        .output()
      {
        Ok(output) => Run::Exited(
          output.status.code(),
          String::from_utf8_lossy(&output.stdout).into_owned(),
          String::from_utf8_lossy(&output.stderr).into_owned(),
        ),
        Err(_) => Run::Missing,
      },
      move || {
        super::gtk_thread::run(move |gtk_ok| {
          gtk_ok
            .then(|| super::gtk_dialog(kind, &title_s, &message_s, &default_s))
        })
        .flatten()
      },
    )
  }
}

/// The process's GTK thread, started on first use: GTK may only ever be used
/// from the thread that initialized it, and the runtime calls in on
/// whichever thread it likes. It serves the in-process dialogs and, on
/// Linux, the tray (libappindicator and its menus, tray.rs). Where GTK
/// started, the thread iterates GLib between jobs, so the tray's D-Bus
/// registration and its menus' events are served while no job runs; a job
/// wakes it. A job that panics is caught there; a `run` caller gets `None`.
#[cfg(any(target_os = "linux", test))]
pub(crate) mod gtk_thread {
  use std::panic::{catch_unwind, AssertUnwindSafe};
  use std::sync::mpsc;
  use std::sync::{Condvar, Mutex};

  type Job = Box<dyn FnOnce(bool) + Send>;

  fn sender() -> &'static Mutex<Option<mpsc::Sender<Job>>> {
    static SENDER: Mutex<Option<mpsc::Sender<Job>>> = Mutex::new(None);
    &SENDER
  }

  /// Whether GTK starts on the GTK thread: not when it can't open a
  /// display, nor when something else in the process already initialized it
  /// on another thread (gtk-rs would panic).
  #[cfg(target_os = "linux")]
  fn init_gtk() -> bool {
    catch_unwind(|| !gtk::is_initialized() && gtk::init().is_ok())
      .unwrap_or(false)
  }

  #[cfg(not(target_os = "linux"))]
  fn init_gtk() -> bool {
    false
  }

  /// Wake the GTK thread out of its GLib wait to take a new job. Safe from
  /// any thread, and before the thread waits (the wakeup is kept until its
  /// next iteration).
  #[cfg(target_os = "linux")]
  fn wake() {
    gtk::glib::MainContext::default().wakeup();
  }

  #[cfg(not(target_os = "linux"))]
  fn wake() {}

  /// One GLib iteration, waiting for an event (a source, or [`wake`]).
  #[cfg(target_os = "linux")]
  fn iterate() {
    gtk::main_iteration_do(true);
  }

  #[cfg(not(target_os = "linux"))]
  fn iterate() {}

  /// Whether GTK started on the GTK thread, once the thread has tried.
  fn started() -> &'static (Mutex<Option<bool>>, Condvar) {
    static STARTED: (Mutex<Option<bool>>, Condvar) =
      (Mutex::new(None), Condvar::new());
    &STARTED
  }

  fn serve(rx: mpsc::Receiver<Job>) {
    let gtk_ok = init_gtk();
    {
      let (state, cv) = started();
      *state.lock().unwrap_or_else(|e| e.into_inner()) = Some(gtk_ok);
      cv.notify_all();
    }
    let run_job = |job: Job| {
      // A panicking job drops its result sender: a `run` caller sees None,
      // and the thread serves the next one.
      let _ = catch_unwind(AssertUnwindSafe(|| job(gtk_ok)));
    };
    if !gtk_ok {
      for job in rx {
        run_job(job);
      }
      return;
    }
    loop {
      loop {
        match rx.try_recv() {
          Ok(job) => run_job(job),
          Err(mpsc::TryRecvError::Empty) => break,
          Err(mpsc::TryRecvError::Disconnected) => return,
        }
      }
      iterate();
    }
  }

  /// Queue `job` on the GTK thread (its argument: GTK is usable there)
  /// without waiting for it. Jobs run in the order they were queued. False
  /// when the thread is gone.
  pub(crate) fn spawn(job: impl FnOnce(bool) + Send + 'static) -> bool {
    let mut guard = sender().lock().unwrap_or_else(|e| e.into_inner());
    let sender = guard.get_or_insert_with(|| {
      let (tx, rx) = mpsc::channel::<Job>();
      std::thread::Builder::new()
        .name("laufey-gtk".into())
        .spawn(move || serve(rx))
        .expect("spawn the GTK thread");
      tx
    });
    let sent = sender.send(Box::new(job)).is_ok();
    wake();
    sent
  }

  /// Whether GTK is usable on the GTK thread, starting the thread if it
  /// hasn't been. Waits only for GTK's initialization, never behind a queued
  /// job (a modal dialog).
  pub(crate) fn usable() -> bool {
    let (state, cv) = started();
    if let Some(ok) = *state.lock().unwrap_or_else(|e| e.into_inner()) {
      return ok;
    }
    if !spawn(|_| {}) {
      return false;
    }
    let guard = state.lock().unwrap_or_else(|e| e.into_inner());
    let guard = cv
      .wait_while(guard, |s| s.is_none())
      .unwrap_or_else(|e| e.into_inner());
    guard.unwrap_or(false)
  }

  /// Run `job` on the GTK thread (its argument: GTK is usable there) and
  /// wait for its result. `None` when the job panicked or the thread is
  /// gone.
  pub(crate) fn run<R: Send + 'static>(
    job: impl FnOnce(bool) -> R + Send + 'static,
  ) -> Option<R> {
    let (tx, rx) = mpsc::channel();
    if !spawn(move |gtk_ok| {
      let _ = tx.send(job(gtk_ok));
    }) {
      return None;
    }
    rx.recv().ok()
  }
}

/// Test hook: answer the next in-process GTK dialog by itself once it is up
/// (`(OK?, text for a prompt's entry)`), so the GTK path runs end to end
/// under Xvfb.
#[cfg(all(target_os = "linux", test))]
pub(crate) static GTK_AUTO_ANSWER: std::sync::Mutex<
  Option<(bool, Option<String>)>,
> = std::sync::Mutex::new(None);

/// The in-process GTK dialog: a message dialog (with an entry for a
/// prompt), its text as plain text. Runs on the GTK thread.
#[cfg(target_os = "linux")]
fn gtk_dialog(
  kind: Kind,
  title: &str,
  message: &str,
  default_value: &str,
) -> DialogOutcome {
  use gtk::prelude::*;
  let (message_type, buttons) = match kind {
    Kind::Alert => (gtk::MessageType::Info, gtk::ButtonsType::Ok),
    Kind::Confirm => (gtk::MessageType::Question, gtk::ButtonsType::OkCancel),
    Kind::Prompt => (gtk::MessageType::Question, gtk::ButtonsType::OkCancel),
  };
  // The "text" property: plain text (use-markup stays false).
  let dialog = gtk::MessageDialog::new(
    None::<&gtk::Window>,
    gtk::DialogFlags::MODAL,
    message_type,
    buttons,
    message,
  );
  dialog.set_title(title);
  dialog.set_default_response(gtk::ResponseType::Ok);
  dialog.set_keep_above(true);
  let entry = (kind == Kind::Prompt).then(|| {
    let entry = gtk::Entry::new();
    entry.set_text(default_value);
    entry.set_activates_default(true);
    dialog.content_area().add(&entry);
    entry.show();
    entry
  });
  #[cfg(test)]
  if let Some((ok, text)) = GTK_AUTO_ANSWER.lock().unwrap().take() {
    let (d, e) = (dialog.clone(), entry.clone());
    gtk::glib::timeout_add_local_once(
      std::time::Duration::from_millis(100),
      move || {
        if let (Some(e), Some(text)) = (e, text) {
          e.set_text(&text);
        }
        d.response(if ok {
          gtk::ResponseType::Ok
        } else {
          gtk::ResponseType::Cancel
        });
      },
    );
  }
  let response = dialog.run();
  let text = entry.map(|e| e.text().to_string());
  // SAFETY: the dialog is ours and no reference to it outlives this call.
  unsafe { dialog.destroy() };
  while gtk::events_pending() {
    gtk::main_iteration();
  }
  match (kind, response == gtk::ResponseType::Ok) {
    // An alert closed any way was seen.
    (Kind::Alert, _) => DialogOutcome::Confirmed(None),
    (_, true) => DialogOutcome::Confirmed(text),
    (_, false) => DialogOutcome::Cancelled,
  }
}

#[cfg(not(any(
  target_os = "macos",
  target_os = "windows",
  target_os = "linux"
)))]
pub(crate) fn show_prompt_dialog(
  _title: &str,
  _message: &str,
  default_value: &str,
) -> DialogOutcome {
  DialogOutcome::Confirmed(Some(default_value.to_string()))
}

#[cfg(target_os = "windows")]
pub(crate) use win::show_prompt_dialog;

#[cfg(target_os = "windows")]
mod win {
  use std::ffi::c_void;

  type Hwnd = *mut c_void;

  const MESSAGE_ID: i32 = 100;
  const EDIT_ID: i32 = 101;
  const IDOK: i32 = 1;
  const IDCANCEL: i32 = 2;

  const WS_POPUP: u32 = 0x8000_0000;
  const WS_CHILD: u32 = 0x4000_0000;
  const WS_VISIBLE: u32 = 0x1000_0000;
  const WS_CAPTION: u32 = 0x00C0_0000;
  const WS_SYSMENU: u32 = 0x0008_0000;
  const WS_TABSTOP: u32 = 0x0001_0000;
  const WS_EX_CLIENTEDGE: u32 = 0x0000_0200;
  const DS_MODALFRAME: u32 = 0x80;
  const DS_SETFOREGROUND: u32 = 0x200;
  const DS_CENTER: u32 = 0x800;
  const SS_NOPREFIX: u32 = 0x80;
  const SS_EDITCONTROL: u32 = 0x2000;
  const ES_AUTOHSCROLL: u32 = 0x80;
  const BS_DEFPUSHBUTTON: u32 = 0x1;
  const WM_INITDIALOG: u32 = 0x0110;
  const WM_COMMAND: u32 = 0x0111;
  const WM_SETFONT: u32 = 0x0030;
  const EM_SETSEL: u32 = 0x00B1;
  const DEFAULT_GUI_FONT: i32 = 17;
  const DT_WORDBREAK: u32 = 0x10;
  const DT_CALCRECT: u32 = 0x400;
  const DT_NOPREFIX: u32 = 0x800;
  const DT_EDITCONTROL: u32 = 0x2000;
  const GWL_STYLE: i32 = -16;
  const GWL_EXSTYLE: i32 = -20;
  /// DWLP_MSGRESULT (0) + sizeof(LRESULT) + sizeof(DLGPROC).
  const DWLP_USER: i32 = 2 * std::mem::size_of::<usize>() as i32;

  #[repr(C)]
  struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
  }

  /// DLGTEMPLATE (2-byte packed) followed by its empty menu, class and title
  /// arrays; the whole template must be DWORD-aligned.
  #[repr(C, align(4))]
  struct Template {
    style: u32,
    ex_style: u32,
    cdit: u16,
    x: i16,
    y: i16,
    cx: i16,
    cy: i16,
    menu: u16,
    class: u16,
    title: u16,
  }

  type DlgProc = unsafe extern "system" fn(Hwnd, u32, usize, isize) -> isize;

  #[link(name = "user32")]
  extern "system" {
    fn DialogBoxIndirectParamW(
      instance: *mut c_void,
      template: *const Template,
      owner: Hwnd,
      proc_: DlgProc,
      param: isize,
    ) -> isize;
    fn EndDialog(dialog: Hwnd, result: isize) -> i32;
    fn CreateWindowExW(
      ex_style: u32,
      class: *const u16,
      name: *const u16,
      style: u32,
      x: i32,
      y: i32,
      w: i32,
      h: i32,
      parent: Hwnd,
      menu: isize,
      instance: *mut c_void,
      param: *mut c_void,
    ) -> Hwnd;
    fn SendMessageW(hwnd: Hwnd, msg: u32, wp: usize, lp: isize) -> isize;
    fn SetWindowTextW(hwnd: Hwnd, text: *const u16) -> i32;
    fn GetWindowTextW(hwnd: Hwnd, buf: *mut u16, len: i32) -> i32;
    fn GetWindowTextLengthW(hwnd: Hwnd) -> i32;
    fn GetDlgItem(dialog: Hwnd, id: i32) -> Hwnd;
    fn MoveWindow(
      hwnd: Hwnd,
      x: i32,
      y: i32,
      w: i32,
      h: i32,
      paint: i32,
    ) -> i32;
    fn SetFocus(hwnd: Hwnd) -> Hwnd;
    fn GetDpiForWindow(hwnd: Hwnd) -> u32;
    fn AdjustWindowRectExForDpi(
      rect: *mut Rect,
      style: u32,
      menu: i32,
      ex_style: u32,
      dpi: u32,
    ) -> i32;
    fn SetWindowPos(
      hwnd: Hwnd,
      after: Hwnd,
      x: i32,
      y: i32,
      w: i32,
      h: i32,
      flags: u32,
    ) -> i32;
    fn GetDC(hwnd: Hwnd) -> *mut c_void;
    fn ReleaseDC(hwnd: Hwnd, dc: *mut c_void) -> i32;
    fn DrawTextW(
      dc: *mut c_void,
      text: *const u16,
      len: i32,
      rect: *mut Rect,
      format: u32,
    ) -> i32;
    #[cfg_attr(target_pointer_width = "32", link_name = "SetWindowLongW")]
    fn SetWindowLongPtrW(hwnd: Hwnd, index: i32, value: isize) -> isize;
    #[cfg_attr(target_pointer_width = "32", link_name = "GetWindowLongW")]
    fn GetWindowLongPtrW(hwnd: Hwnd, index: i32) -> isize;
  }
  #[link(name = "gdi32")]
  extern "system" {
    fn GetStockObject(kind: i32) -> *mut c_void;
    fn SelectObject(dc: *mut c_void, object: *mut c_void) -> *mut c_void;
  }
  #[link(name = "kernel32")]
  extern "system" {
    fn GetModuleHandleW(name: *const u16) -> *mut c_void;
  }

  struct State {
    title: Vec<u16>,
    message: Vec<u16>,
    value: Vec<u16>,
    result: Option<String>,
  }

  fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
  }

  unsafe fn add_control(
    dialog: Hwnd,
    class: &str,
    text: *const u16,
    style: u32,
    ex_style: u32,
    id: i32,
    font: *mut c_void,
  ) -> Hwnd {
    let class = wide(class);
    let control = CreateWindowExW(
      ex_style,
      class.as_ptr(),
      text,
      WS_CHILD | WS_VISIBLE | style,
      0,
      0,
      0,
      0,
      dialog,
      id as isize,
      GetModuleHandleW(std::ptr::null()),
      std::ptr::null_mut(),
    );
    if !control.is_null() {
      SendMessageW(control, WM_SETFONT, font as usize, 0);
    }
    control
  }

  unsafe fn lay_out(dialog: Hwnd, state: &State) {
    let dpi = match GetDpiForWindow(dialog) {
      0 => 96,
      d => d as i32,
    };
    let px = |dip: i32| dip * dpi / 96;
    let font = GetStockObject(DEFAULT_GUI_FONT);
    let (margin, gap, button_w, button_h, edit_h, text_w) =
      (px(12), px(8), px(80), px(26), px(24), px(360));

    // The message's height wrapped to text_w; "&" is a character, not a
    // mnemonic marker (DT_NOPREFIX / SS_NOPREFIX).
    let mut text_rect = Rect {
      left: 0,
      top: 0,
      right: text_w,
      bottom: 0,
    };
    let dc = GetDC(dialog);
    let old = SelectObject(dc, font);
    DrawTextW(
      dc,
      state.message.as_ptr(),
      -1,
      &mut text_rect,
      DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX,
    );
    SelectObject(dc, old);
    ReleaseDC(dialog, dc);
    let text_h = text_rect.bottom.clamp(px(16), px(480));

    let label = add_control(
      dialog,
      "STATIC",
      state.message.as_ptr(),
      SS_NOPREFIX | SS_EDITCONTROL,
      0,
      MESSAGE_ID,
      font,
    );
    let edit = add_control(
      dialog,
      "EDIT",
      state.value.as_ptr(),
      WS_TABSTOP | ES_AUTOHSCROLL,
      WS_EX_CLIENTEDGE,
      EDIT_ID,
      font,
    );
    let ok_text = wide("OK");
    let cancel_text = wide("Cancel");
    let ok = add_control(
      dialog,
      "BUTTON",
      ok_text.as_ptr(),
      WS_TABSTOP | BS_DEFPUSHBUTTON,
      0,
      IDOK,
      font,
    );
    let cancel = add_control(
      dialog,
      "BUTTON",
      cancel_text.as_ptr(),
      WS_TABSTOP,
      0,
      IDCANCEL,
      font,
    );

    let mut y = margin;
    MoveWindow(label, margin, y, text_w, text_h, 0);
    y += text_h + gap;
    MoveWindow(edit, margin, y, text_w, edit_h, 0);
    y += edit_h + px(16);
    MoveWindow(cancel, margin + text_w - button_w, y, button_w, button_h, 0);
    MoveWindow(
      ok,
      margin + text_w - 2 * button_w - gap,
      y,
      button_w,
      button_h,
      0,
    );
    y += button_h + margin;

    let mut frame = Rect {
      left: 0,
      top: 0,
      right: margin * 2 + text_w,
      bottom: y,
    };
    AdjustWindowRectExForDpi(
      &mut frame,
      GetWindowLongPtrW(dialog, GWL_STYLE) as u32,
      0,
      GetWindowLongPtrW(dialog, GWL_EXSTYLE) as u32,
      dpi as u32,
    );
    // SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE: DS_CENTER placed it.
    SetWindowPos(
      dialog,
      std::ptr::null_mut(),
      0,
      0,
      frame.right - frame.left,
      frame.bottom - frame.top,
      0x2 | 0x4 | 0x10,
    );
  }

  unsafe extern "system" fn dialog_proc(
    dialog: Hwnd,
    msg: u32,
    wp: usize,
    lp: isize,
  ) -> isize {
    match msg {
      WM_INITDIALOG => {
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        let state = &*(lp as *const State);
        SetWindowTextW(dialog, state.title.as_ptr());
        lay_out(dialog, state);
        let edit = GetDlgItem(dialog, EDIT_ID);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SetFocus(edit);
        0 // The focus is set.
      }
      WM_COMMAND => {
        let id = (wp & 0xFFFF) as i32;
        let state = GetWindowLongPtrW(dialog, DWLP_USER) as *mut State;
        if id == IDOK && !state.is_null() {
          let edit = GetDlgItem(dialog, EDIT_ID);
          let len = GetWindowTextLengthW(edit).max(0);
          let mut buf = vec![0u16; len as usize + 1];
          let n = GetWindowTextW(edit, buf.as_mut_ptr(), len + 1).max(0);
          (*state).result = Some(String::from_utf16_lossy(&buf[..n as usize]));
          EndDialog(dialog, IDOK as isize);
          1
        } else if id == IDCANCEL {
          EndDialog(dialog, IDCANCEL as isize);
          1
        } else {
          0
        }
      }
      _ => 0,
    }
  }

  pub(crate) fn show_prompt_dialog(
    title: &str,
    message: &str,
    default_value: &str,
  ) -> super::DialogOutcome {
    let template = Template {
      style: WS_POPUP
        | WS_CAPTION
        | WS_SYSMENU
        | DS_MODALFRAME
        | DS_SETFOREGROUND
        | DS_CENTER,
      ex_style: 0,
      cdit: 0,
      x: 0,
      y: 0,
      cx: 240,
      cy: 80,
      menu: 0,
      class: 0,
      title: 0,
    };
    let mut state = State {
      title: wide(title),
      message: wide(message),
      value: wide(default_value),
      result: None,
    };
    // SAFETY: the template and the state outlive the modal call, which
    // returns only once the dialog is destroyed.
    let result = unsafe {
      DialogBoxIndirectParamW(
        GetModuleHandleW(std::ptr::null()),
        &template,
        std::ptr::null_mut(),
        dialog_proc,
        &mut state as *mut State as isize,
      )
    };
    if result == IDOK as isize {
      super::DialogOutcome::Confirmed(Some(
        state.result.take().unwrap_or_default(),
      ))
    } else {
      super::DialogOutcome::Cancelled
    }
  }
}

#[cfg(test)]
mod tests {
  use super::linux::{Run, Tool};
  use super::*;

  const HOSTILE: &str =
    "'; calc; ' $(touch /tmp/x) `id` \"q\" \\n \\\\ _a_ &b <b>x</b> &amp;\nline2 \u{2019}; \u{1F600}";

  #[test]
  fn osascript_strings_are_argv_not_script() {
    let args = osascript_prompt_args("T -e x", HOSTILE, "-d");
    // The script lines are fixed; no string reaches them.
    let script: Vec<&String> = args.iter().take(8).collect();
    for line in &script {
      assert!(!line.contains("calc") && !line.contains("touch"));
    }
    assert_eq!(args[8], "--");
    assert_eq!(&args[9..], &[HOSTILE, "-d", "T -e x"]);
  }

  #[test]
  fn zenity_strings_are_option_values_and_plain_text() {
    let args = linux::zenity_args(Kind::Prompt, "--help", HOSTILE, "--x");
    assert_eq!(args.len(), 4);
    assert_eq!(args[0], "--entry");
    assert_eq!(args[1], "--title=--help");
    assert_eq!(args[3], "--entry-text=--x");
    // Escaped so g_strcompress + the mnemonic parse give the text back.
    let text = args[2].strip_prefix("--text=").unwrap();
    assert_eq!(unmnemonic(&strcompress(text)), HOSTILE);
    // Alert and confirm: --no-markup, so the text is shown as given (no
    // g_strcompress, no Pango markup).
    for (kind, first) in
      [(Kind::Alert, "--info"), (Kind::Confirm, "--question")]
    {
      let args = linux::zenity_args(kind, "--help", HOSTILE, "");
      assert_eq!(args[0], first);
      assert_eq!(args[1], "--title=--help");
      assert!(args.contains(&"--no-markup".to_string()));
      assert!(args.contains(&format!("--text={HOSTILE}")));
    }
    let confirm = linux::zenity_args(Kind::Confirm, "T", "M", "");
    assert!(confirm.contains(&"--ok-label=OK".to_string()));
    assert!(confirm.contains(&"--cancel-label=Cancel".to_string()));
  }

  #[test]
  fn kdialog_strings_are_option_values_or_after_dashdash() {
    let args = linux::kdialog_args(Kind::Prompt, "--help", HOSTILE, "--x");
    assert_eq!(args.len(), 4);
    assert_eq!(args[0], "--title=--help");
    assert!(args[1].starts_with("--inputbox=<qt>"));
    assert_eq!(&args[2..], &["--".to_string(), "--x".to_string()]);
    let alert = linux::kdialog_args(Kind::Alert, "T", HOSTILE, "");
    assert_eq!(alert[0], "--title=T");
    assert!(alert[1].starts_with("--msgbox=<qt>"));
    assert_eq!(alert.len(), 2);
    let confirm = linux::kdialog_args(Kind::Confirm, "T", HOSTILE, "");
    assert!(confirm[1].starts_with("--yesno=<qt>"));
    assert_eq!(&confirm[2..], &["--yes-label=OK", "--no-label=Cancel"]);
  }

  #[test]
  fn kdialog_text_is_plain() {
    // What kdialog and Qt do with it: Utils::parseString, then the rich
    // text document, then (input box only) the mnemonic strip.
    for mnemonic in [false, true] {
      let text = linux::kdialog_text(HOSTILE, mnemonic);
      assert!(text.starts_with("<qt>"));
      let mut shown = html_text(&parse_string(&text));
      if mnemonic {
        shown = strip_mnemonics(&shown);
      }
      assert_eq!(shown, HOSTILE, "mnemonic: {mnemonic}");
    }
  }

  /// Runs `dialog_with` over scripted tool results, recording the tools
  /// tried.
  fn scripted(
    kind: Kind,
    have_display: bool,
    kdialog: Run,
    zenity: Run,
    in_process: Option<DialogOutcome>,
  ) -> (DialogOutcome, Vec<&'static str>, bool) {
    let tried = std::cell::RefCell::new(Vec::new());
    let gtk_ran = std::cell::Cell::new(false);
    let outcome = linux::dialog_with(
      kind,
      "T",
      "M",
      "D",
      have_display,
      |tool, args| {
        tried.borrow_mut().push(tool.program());
        // Never a shell: the program is the tool itself, the strings argv.
        assert!(!args.iter().any(|a| a == "-c"));
        match tool {
          Tool::Kdialog => kdialog.clone(),
          Tool::Zenity => zenity.clone(),
        }
      },
      || {
        gtk_ran.set(true);
        in_process
      },
    );
    (outcome, tried.into_inner(), gtk_ran.get())
  }

  fn ok(out: &str) -> Run {
    Run::Exited(Some(0), out.into(), String::new())
  }
  fn code(c: i32, err: &str) -> Run {
    Run::Exited(Some(c), String::new(), err.into())
  }

  #[test]
  fn linux_providers_in_order() {
    use Run::Missing;
    // kdialog first (Plasma); its answer is final, OK or cancel.
    let (o, tried, gtk) =
      scripted(Kind::Prompt, true, ok("typed\n"), Missing, None);
    assert_eq!(o, DialogOutcome::Confirmed(Some("typed".into())));
    assert_eq!(tried, ["kdialog"]);
    assert!(!gtk);
    let (o, tried, _) =
      scripted(Kind::Prompt, true, code(1, ""), Missing, None);
    assert_eq!(o, DialogOutcome::Cancelled);
    assert_eq!(tried, ["kdialog"]);
    // No kdialog (stock GNOME): zenity.
    let (o, tried, _) = scripted(Kind::Prompt, true, Missing, ok("z\n"), None);
    assert_eq!(o, DialogOutcome::Confirmed(Some("z".into())));
    assert_eq!(tried, ["kdialog", "zenity"]);
    // A tool that failed (crashed, couldn't open the display) is not a
    // cancel: the next provider runs.
    let (o, tried, gtk) = scripted(
      Kind::Prompt,
      true,
      Run::Exited(None, String::new(), String::new()),
      code(255, ""),
      Some(DialogOutcome::Confirmed(Some("g".into()))),
    );
    assert_eq!(o, DialogOutcome::Confirmed(Some("g".into())));
    assert_eq!(tried, ["kdialog", "zenity"]);
    assert!(gtk);
    // Neither tool (Fedora, stock Kubuntu's missing zenity): the in-process
    // dialog.
    let (o, _, gtk) = scripted(
      Kind::Prompt,
      true,
      Missing,
      Missing,
      Some(DialogOutcome::Cancelled),
    );
    assert_eq!(o, DialogOutcome::Cancelled);
    assert!(gtk);
    // Nothing can show it: unsupported, never a fake cancel.
    let (o, _, _) = scripted(Kind::Prompt, true, Missing, Missing, None);
    assert_eq!(o, DialogOutcome::Unsupported);
    let (o, tried, gtk) = scripted(
      Kind::Prompt,
      false,
      ok("x"),
      Missing,
      Some(DialogOutcome::Cancelled),
    );
    assert_eq!(o, DialogOutcome::Unsupported);
    assert!(tried.is_empty());
    assert!(!gtk);
  }

  #[test]
  fn alert_and_confirm_use_the_same_providers() {
    use Run::Missing;
    for kind in [Kind::Alert, Kind::Confirm] {
      // kdialog, then zenity, then GTK: not a zenity-only path.
      let (o, tried, _) = scripted(kind, true, ok(""), Missing, None);
      assert_eq!(o, DialogOutcome::Confirmed(None));
      assert_eq!(tried, ["kdialog"]);
      let (_, tried, gtk) = scripted(
        kind,
        true,
        Missing,
        Missing,
        Some(DialogOutcome::Confirmed(None)),
      );
      assert_eq!(tried, ["kdialog", "zenity"]);
      assert!(gtk);
      let (o, _, _) = scripted(kind, true, Missing, Missing, None);
      assert_eq!(o, DialogOutcome::Unsupported);
      let (o, _, _) = scripted(kind, false, ok(""), Missing, None);
      assert_eq!(o, DialogOutcome::Unsupported);
    }
    // Dismissing an alert is still having seen it; a confirm's dismissal
    // is a cancel.
    let (o, _, _) = scripted(Kind::Alert, true, code(1, ""), Missing, None);
    assert_eq!(o, DialogOutcome::Confirmed(None));
    let (o, _, _) = scripted(Kind::Confirm, true, Missing, code(1, ""), None);
    assert_eq!(o, DialogOutcome::Cancelled);
  }

  #[test]
  fn a_tool_with_no_display_is_not_a_cancel() {
    // zenity 4.2 with no display (checked on Ubuntu 26.04): exit 1, the
    // cancel code, with this on stderr.
    let zenity4 = "\n(zenity:14211): Gtk-WARNING **: 14:26:18.363: Failed to \
                   open display\n";
    assert!(linux::display_failure(zenity4));
    assert!(linux::display_failure("zenity: cannot open display: :0"));
    assert!(linux::display_failure(
      "qt.qpa.xcb: could not connect to display "
    ));
    assert!(!linux::display_failure("Gtk-Message: GtkDialog mapped without a transient parent. This is discouraged."));
    for kind in [Kind::Alert, Kind::Confirm, Kind::Prompt] {
      let (o, tried, gtk) = scripted(
        kind,
        true,
        Run::Missing,
        code(1, zenity4),
        Some(DialogOutcome::Confirmed(None)),
      );
      assert_eq!(o, DialogOutcome::Confirmed(None), "{kind:?}");
      assert_eq!(tried, ["kdialog", "zenity"]);
      assert!(gtk);
    }
    // A plain cancel still is one (warnings on stderr are common).
    let (o, _, gtk) = scripted(
      Kind::Prompt,
      true,
      Run::Missing,
      code(
        1,
        "Gtk-Message: GtkDialog mapped without a transient parent",
      ),
      Some(DialogOutcome::Confirmed(None)),
    );
    assert_eq!(o, DialogOutcome::Cancelled);
    assert!(!gtk);
  }

  #[test]
  fn gtk_jobs_run_on_one_thread() {
    // Whatever thread asks, GTK is only ever used from the GTK thread.
    let here = std::thread::current().id();
    let a = gtk_thread::run(|_| std::thread::current().id()).unwrap();
    let b = std::thread::spawn(|| {
      gtk_thread::run(|_| std::thread::current().id()).unwrap()
    })
    .join()
    .unwrap();
    assert_eq!(a, b);
    assert_ne!(a, here);
    // A job that panics is caught there (it never reaches an extern "C"
    // caller), and the thread serves the next one.
    let prev = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let panicked = gtk_thread::run(|_| -> u32 { panic!("in a dialog") });
    std::panic::set_hook(prev);
    assert_eq!(panicked, None);
    assert_eq!(gtk_thread::run(|_| 7), Some(7));
  }

  #[test]
  fn gtk_jobs_run_in_queue_order() {
    let seen = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
    for i in 0..20 {
      let seen = seen.clone();
      assert!(gtk_thread::spawn(move |_| seen.lock().unwrap().push(i)));
    }
    // `run` queues behind them, so they have all run when it returns.
    assert_eq!(gtk_thread::run(|_| ()), Some(()));
    assert_eq!(*seen.lock().unwrap(), (0..20).collect::<Vec<_>>());
  }

  /// Where GTK starts, the GTK thread serves GLib between jobs (the tray's
  /// D-Bus registration and menus need it), and GTK widgets can be built in
  /// a job (the tray menu panicked "GTK has not been initialized" when built
  /// on the winit thread).
  #[cfg(target_os = "linux")]
  #[test]
  fn gtk_thread_serves_glib_between_jobs() {
    let display = std::env::var_os("DISPLAY").is_some_and(|v| !v.is_empty())
      || std::env::var_os("WAYLAND_DISPLAY").is_some_and(|v| !v.is_empty());
    let gtk_ok = gtk_thread::run(|ok| ok).unwrap();
    if !display || !gtk_ok {
      eprintln!("gtk_thread_serves_glib_between_jobs: no display, skipped");
      return;
    }
    let (tx, rx) = std::sync::mpsc::channel();
    assert!(gtk_thread::spawn(move |_| {
      let _menu = gtk::Menu::new();
      gtk::glib::timeout_add_local_once(
        std::time::Duration::from_millis(50),
        move || {
          let _ = tx.send(());
        },
      );
    }));
    // No job follows: only the thread's own GLib iteration fires the source.
    assert!(rx.recv_timeout(std::time::Duration::from_secs(10)).is_ok());
  }

  /// The real GTK dialogs, end to end (Linux with a display: CI runs this
  /// under xvfb-run). Each is answered by GTK_AUTO_ANSWER once it is up,
  /// from threads other than the GTK one, and GTK is never initialized on
  /// the asking thread.
  #[cfg(target_os = "linux")]
  #[test]
  fn gtk_dialogs_end_to_end() {
    let display = std::env::var_os("DISPLAY").is_some_and(|v| !v.is_empty())
      || std::env::var_os("WAYLAND_DISPLAY").is_some_and(|v| !v.is_empty());
    let gtk_ok = gtk_thread::run(|ok| ok).unwrap();
    if !display || !gtk_ok {
      eprintln!("gtk_dialogs_end_to_end: no display GTK can open, skipped");
      return;
    }
    let ask = |kind, ok: bool, text: Option<&str>| {
      *GTK_AUTO_ANSWER.lock().unwrap() = Some((ok, text.map(str::to_string)));
      let (k, t) = (kind, HOSTILE.to_string());
      std::thread::spawn(move || {
        gtk_thread::run(move |ok| ok.then(|| gtk_dialog(k, &t, &t, "def")))
          .flatten()
      })
      .join()
      .unwrap()
    };
    assert_eq!(
      ask(Kind::Prompt, true, Some("typed")),
      Some(DialogOutcome::Confirmed(Some("typed".into())))
    );
    // OK with the default text untouched.
    assert_eq!(
      ask(Kind::Prompt, true, None),
      Some(DialogOutcome::Confirmed(Some("def".into())))
    );
    assert_eq!(
      ask(Kind::Prompt, false, None),
      Some(DialogOutcome::Cancelled)
    );
    assert_eq!(
      ask(Kind::Confirm, true, None),
      Some(DialogOutcome::Confirmed(None))
    );
    assert_eq!(
      ask(Kind::Confirm, false, None),
      Some(DialogOutcome::Cancelled)
    );
    assert_eq!(
      ask(Kind::Alert, false, None),
      Some(DialogOutcome::Confirmed(None))
    );
    // The asking thread never initialized GTK.
    assert!(!gtk::is_initialized_main_thread());
  }

  #[test]
  fn strips_only_the_tools_newline() {
    assert_eq!(strip_one_newline(" a \n\n"), " a \n");
    assert_eq!(strip_one_newline("a"), "a");
  }

  /// g_strcompress for the escapes `zenity_args` can produce (`\\`) and the
  /// ones a message could contain (`\n`).
  fn strcompress(s: &str) -> String {
    let mut out = String::new();
    let mut it = s.chars();
    while let Some(c) = it.next() {
      if c != '\\' {
        out.push(c);
        continue;
      }
      match it.next() {
        Some('n') => out.push('\n'),
        Some('t') => out.push('\t'),
        Some(other) => out.push(other),
        None => {}
      }
    }
    out
  }

  /// GTK's mnemonic parse: `__` is `_`, a lone `_` marks the mnemonic.
  fn unmnemonic(s: &str) -> String {
    let mut out = String::new();
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
      if c == '_' {
        if it.peek() == Some(&'_') {
          it.next();
          out.push('_');
        }
        continue;
      }
      out.push(c);
    }
    out
  }

  /// kdialog's Utils::parseString: `\\` is `\`, `\n` a newline.
  fn parse_string(s: &str) -> String {
    let mut out = String::new();
    let mut it = s.chars();
    while let Some(c) = it.next() {
      if c != '\\' {
        out.push(c);
        continue;
      }
      match it.next() {
        Some('\\') => out.push('\\'),
        Some('n') => out.push('\n'),
        Some(other) => {
          out.push('\\');
          out.push(other);
        }
        None => {}
      }
    }
    out
  }

  /// The text of the `<qt><p style=...>…</p></qt>` document `kdialog_text`
  /// builds: entities decoded, `<br>` a newline.
  fn html_text(s: &str) -> String {
    let body = s
      .strip_prefix("<qt><p style=\"white-space:pre-wrap\">")
      .and_then(|b| b.strip_suffix("</p></qt>"))
      .expect("the kdialog wrapper");
    // No raw tag may be left in the body but <br>.
    let body = body.replace("<br>", "\n");
    assert!(!body.contains('<') && !body.contains('>'));
    body
      .replace("&lt;", "<")
      .replace("&gt;", ">")
      .replace("&quot;", "\"")
      .replace("&amp;", "&")
  }

  /// QLabel with a buddy: each `&` is removed, `&&` keeps one.
  fn strip_mnemonics(s: &str) -> String {
    let mut out = String::new();
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
      if c == '&' {
        if let Some(&next) = it.peek() {
          it.next();
          out.push(next);
        }
        continue;
      }
      out.push(c);
    }
    out
  }
}
