// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! LAUFEY_E2E_ONLY=io: drag and drop, native file dialogs and the rich
//! clipboard (API 39). See docs/e2e-testing.md.
//!
//! - Clipboard: text / HTML (+ plain alternative) / PNG round trips, the
//!   formats list, a non-PNG write refused, and a change event after a write
//!   (through the OS watcher: the macOS change-count poll, Windows'
//!   clipboard listener, GTK's owner-change).
//! - File drops: every phase through `test_trigger_file_drop`, the dispatch
//!   the OS path uses, checked for window id, position and paths. On X11
//!   also a real XDND drag from a GTK drag source, moved with xdotool.
//! - File dialogs: real dialogs, closed by the test hook (cancel), by
//!   `cancel_file_dialog`, refused while another is open (busy), and accepted
//!   with a path the hook types in (save, open a file, open a directory),
//!   each result coming back through the dialog's own completion path.
//! - Abort stress: dialogs of every kind (open / save / folder, modal and
//!   app-level, with filters and multi-select) cancelled through
//!   `cancel_file_dialog` at delays from "at once" to well after the dialog
//!   window is up; each one settles cancelled exactly once and, on Windows,
//!   its window is gone afterwards.
//! - Windows: a real OLE drag of a file out of laufey_ole_drag_source
//!   (DoDragDrop with CF_HDROP, as Explorer does), driven with SendInput
//!   and released over the window, reaches on_file_drop the same way.
//! - Drag out: refused without a held mouse button and for a bad path (a
//!   real drag needs a person, or OS input injection, and is not run here).

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{
  DragResult, FileDialogKind, FileDialogOptions, FileDialogOutcome,
  FileDragPhase, FileDropEvent, FileFilter, Window,
};

use super::{check, na, wait_for, TINY_PNG};

pub async fn run() {
  let caps = laufey::window_capabilities();
  let clip = laufey::clipboard_capabilities();
  eprintln!("[e2e] window capabilities = {:#x}", caps.bits);
  eprintln!("[e2e] clipboard capabilities = {clip:?}");
  eprintln!(
    "[e2e] file drop={} enter_paths={} drag_out={} dialogs={} mixed={} modal={}",
    caps.file_drop(),
    caps.file_drop_enter_paths(),
    caps.file_drag_out(),
    caps.file_dialogs(),
    caps.file_dialog_files_and_directories(),
    caps.file_dialog_modal()
  );

  let w = Window::new(480, 360).title("native-e2e-io");
  w.show();
  if !wait_for(|| w.get_size().0 != 0, 100, 50).await {
    check("io window reports a size", false);
    return;
  }

  clipboard_checks(&clip).await;
  file_drop_checks(&w, &caps).await;
  xdnd_drop_check(&caps).await;
  ole_drop_check(&caps).await;
  dialog_checks(&w, &caps).await;
  dialog_abort_stress(&w, &caps).await;
  drag_out_checks(&w, &caps).await;
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

async fn clipboard_checks(clip: &laufey::ClipboardCapabilities) {
  check("clipboard reports text", clip.text);
  let nonce = std::process::id();

  // Text, from a worker thread: API 39 backends hop to their UI thread.
  let text = format!("laufey-io-{nonce} \u{e9}\u{4e2d}");
  let t2 = text.clone();
  let got = tokio::task::spawn_blocking(move || {
    laufey::write_clipboard_text(&t2);
    laufey::read_clipboard_text()
  })
  .await
  .ok()
  .flatten();
  if got.is_none() && !clip.html {
    // The Winit backend shells out to the platform's clipboard tools; with
    // none installed (wl-clipboard / xclip on Linux) reads come back empty.
    na("clipboard text (the clipboard tools are missing)");
  } else {
    check(
      "clipboard text round-trips from a worker thread",
      got.as_deref() == Some(text.as_str()),
    );
  }

  if clip.html {
    let html = format!("<b>laufey</b> io <i>{nonce}</i>");
    let plain = format!("laufey io {nonce}");
    check(
      "write_clipboard_html succeeds",
      laufey::write_clipboard_html(&html, Some(&plain)),
    );
    let read = laufey::read_clipboard_html().unwrap_or_default();
    eprintln!("[e2e]   html read back: {read:?}");
    check("clipboard HTML round-trips", read.contains(&html));
    check(
      "the HTML's plain-text alternative reads as text",
      laufey::read_clipboard_text().as_deref() == Some(plain.as_str()),
    );
    if clip.formats {
      let formats = laufey::read_clipboard_formats().unwrap_or_default();
      eprintln!("[e2e]   formats after HTML: {formats:?}");
      check(
        "formats list text/html and text/plain",
        formats.iter().any(|f| f == "text/html")
          && formats.iter().any(|f| f == "text/plain"),
      );
    }
  } else {
    na("clipboard HTML (backend has none)");
  }

  if clip.image {
    check(
      "a non-PNG image write is refused",
      !laufey::write_clipboard_image(b"definitely not a png"),
    );
    check(
      "write_clipboard_image succeeds",
      laufey::write_clipboard_image(TINY_PNG),
    );
    let png = laufey::read_clipboard_image().unwrap_or_default();
    eprintln!("[e2e]   image read back: {} bytes", png.len());
    check(
      "clipboard image reads back as PNG",
      png.starts_with(&[0x89, b'P', b'N', b'G']),
    );
    check("clipboard PNG round-trips byte for byte", png == TINY_PNG);
    if clip.formats {
      let formats = laufey::read_clipboard_formats().unwrap_or_default();
      eprintln!("[e2e]   formats after image: {formats:?}");
      check(
        "formats list image/png after an image write",
        formats.iter().any(|f| f == "image/png"),
      );
      check(
        "an image write replaced the text",
        !formats.iter().any(|f| f == "text/html"),
      );
    }
  } else {
    na("clipboard image (backend has none)");
  }

  if clip.change_events {
    let changes = Arc::new(AtomicUsize::new(0));
    let c = changes.clone();
    laufey::on_clipboard_change(move || {
      c.fetch_add(1, Ordering::SeqCst);
    });
    // Let the watcher start (macOS arms its poll asynchronously).
    tokio::time::sleep(Duration::from_millis(300)).await;
    let before = changes.load(Ordering::SeqCst);
    laufey::write_clipboard_text(&format!("laufey-io-change-{nonce}"));
    let fired =
      wait_for(|| changes.load(Ordering::SeqCst) > before, 60, 50).await;
    check("a clipboard change event follows a write", fired);
    laufey::clear_clipboard_change_handler();
    tokio::time::sleep(Duration::from_millis(700)).await;
    let after_clear = changes.load(Ordering::SeqCst);
    laufey::write_clipboard_text(&format!("laufey-io-quiet-{nonce}"));
    tokio::time::sleep(Duration::from_millis(1200)).await;
    check(
      "no change events after the handler is cleared",
      changes.load(Ordering::SeqCst) == after_clear,
    );
  } else {
    na("clipboard change events (backend / OS has none)");
  }
}

// ---------------------------------------------------------------------------
// File drops
// ---------------------------------------------------------------------------

async fn file_drop_checks(w: &Window, caps: &laufey::WindowCapabilities) {
  let events: Arc<Mutex<Vec<FileDropEvent>>> = Arc::new(Mutex::new(Vec::new()));
  let sink = events.clone();
  laufey::on_file_drop(move |e| sink.lock().unwrap().push(e));
  let id = w.id();
  let a = "/laufey/e2e/a.txt";
  let b = "/laufey/e2e/b c.png";
  let delivered = laufey::test_trigger_file_drop(
    id,
    FileDragPhase::Enter,
    10.0,
    20.0,
    &[a, b],
  );
  if !delivered {
    if caps.file_drop() {
      check("test_trigger_file_drop reaches the handler", false);
    } else {
      na("file drops (backend has none)");
    }
    laufey::clear_file_drop_handler();
    return;
  }
  check("file drop capability is reported", caps.file_drop());
  laufey::test_trigger_file_drop(id, FileDragPhase::Over, 11.5, 21.5, &[]);
  laufey::test_trigger_file_drop(id, FileDragPhase::Leave, 0.0, 0.0, &[a]);
  laufey::test_trigger_file_drop(id, FileDragPhase::Drop, 30.0, 40.0, &[a, b]);
  laufey::test_trigger_file_drop(id, FileDragPhase::Drop, 1.0, 1.0, &[]);
  let got = wait_for(|| events.lock().unwrap().len() >= 5, 40, 25).await;
  check("five drag phases are delivered", got);
  let ev = events.lock().unwrap().clone();
  for e in &ev {
    eprintln!("[e2e]   drop event {e:?}");
  }
  if ev.len() >= 5 {
    let ab = Some(vec![a.to_string(), b.to_string()]);
    check(
      "ENTER carries the window, position and paths",
      ev[0].window_id == id
        && ev[0].phase == FileDragPhase::Enter
        && ev[0].x == 10.0
        && ev[0].y == 20.0
        && ev[0].paths == ab
        && ev[0].count == 2,
    );
    check(
      "OVER without paths carries no paths",
      ev[1].phase == FileDragPhase::Over && ev[1].paths.is_none(),
    );
    check(
      "LEAVE never carries paths",
      ev[2].phase == FileDragPhase::Leave
        && ev[2].paths.is_none()
        && ev[2].count == 0,
    );
    check(
      "DROP carries the paths and the drop point",
      ev[3].phase == FileDragPhase::Drop
        && ev[3].paths == ab
        && ev[3].x == 30.0
        && ev[3].y == 40.0,
    );
    check(
      "a DROP with no paths still carries an (empty) list",
      ev[4].phase == FileDragPhase::Drop && ev[4].paths == Some(vec![]),
    );
  }
  laufey::clear_file_drop_handler();
  check(
    "no delivery once the handler is cleared",
    !laufey::test_trigger_file_drop(id, FileDragPhase::Drop, 0.0, 0.0, &[a]),
  );
}

/// X11: a real drag of a file out of a GTK drag source (laufey_xdnd_source,
/// which offers it as text/uri-list over XDND, as a file manager does),
/// moved with xdotool (XTEST pointer events) and released over the window,
/// reaches on_file_drop through the backend's own XDND handling: ENTER
/// first, then a DROP inside the window with the file's path.
///
/// The drop goes to a window of its own with a page loaded, as an app's
/// window has.
async fn xdnd_drop_check(caps: &laufey::WindowCapabilities) {
  if !cfg!(target_os = "linux") {
    return;
  }
  let source = std::env::var("LAUFEY_E2E_XDND_SOURCE").unwrap_or_default();
  if crate::os_view::xdotool().is_none() || source.is_empty() {
    na("a real XDND drop (needs xdotool and laufey_xdnd_source)");
    return;
  }
  if !caps.file_drop() {
    na("a real XDND drop (backend has no file drops)");
    return;
  }
  let dir = std::env::temp_dir()
    .join(format!("laufey-e2e-xdnd-{}", std::process::id()));
  let file = dir.join("dropped file \u{e9}.txt");
  if std::fs::create_dir_all(&dir).is_err()
    || std::fs::write(&file, b"xdnd").is_err()
  {
    check("a real XDND drop: temp file", false);
    return;
  }
  let path = file.to_string_lossy().into_owned();

  let events: Arc<Mutex<Vec<FileDropEvent>>> = Arc::new(Mutex::new(Vec::new()));
  let sink = events.clone();
  laufey::on_file_drop(move |e| sink.lock().unwrap().push(e));

  const TITLE: &str = "native-e2e-xdnd";
  let loaded = Arc::new(std::sync::atomic::AtomicBool::new(false));
  let w = {
    let loaded = loaded.clone();
    Window::new(480, 360)
      .title(TITLE)
      .on_page_load(move |_| loaded.store(true, Ordering::SeqCst))
      .load(&format!(
        "data:text/html,<title>{TITLE}</title><body%20style='margin:0;height:100vh'>drop</body>"
      ))
  };
  w.show();
  w.set_position(60, 60);
  let _ = wait_for(|| loaded.load(Ordering::SeqCst), 100, 50).await;
  tokio::time::sleep(Duration::from_millis(500)).await;
  let Some((tx, ty, tw, th)) = crate::os_view::content_rect(TITLE) else {
    check(
      "a real XDND drop: the window system reports the window",
      false,
    );
    laufey::clear_file_drop_handler();
    return;
  };

  let mut child = match std::process::Command::new(&source)
    .args(["900", "760", &path])
    .stdout(std::process::Stdio::piped())
    .spawn()
  {
    Ok(c) => c,
    Err(e) => {
      check(
        &format!("a real XDND drop: start the drag source ({e})"),
        false,
      );
      laufey::clear_file_drop_handler();
      return;
    }
  };
  let stdout = child.stdout.take();
  let (line_tx, mut line_rx) = tokio::sync::mpsc::unbounded_channel();
  std::thread::spawn(move || {
    use std::io::BufRead;
    if let Some(out) = stdout {
      for line in std::io::BufReader::new(out).lines().map_while(Result::ok) {
        let _ = line_tx.send(line);
      }
    }
  });
  let ready = tokio::time::timeout(Duration::from_secs(15), async {
    while let Some(line) = line_rx.recv().await {
      let f: Vec<i32> = line
        .strip_prefix("ready ")
        .map(|r| r.split(' ').filter_map(|v| v.parse().ok()).collect())
        .unwrap_or_default();
      if f.len() == 4 {
        return Some((f[0], f[1], f[2], f[3]));
      }
    }
    None
  })
  .await
  .ok()
  .flatten();
  let Some((sx, sy, sw, sh)) = ready else {
    check("a real XDND drop: the drag source window maps", false);
    let _ = child.kill();
    laufey::clear_file_drop_handler();
    return;
  };
  eprintln!(
    "[e2e]   xdnd: source at {sx},{sy} {sw}x{sh}, target at {tx},{ty} {tw}x{th}"
  );

  // Press on the source, move in steps (GTK starts the drag past its
  // threshold, then XDND follows the pointer), and release over the window.
  // The source sends the next XdndPosition only once the target has
  // answered the last one, so the pointer rests at each point before moving
  // on, and the drop is released where the target last saw the pointer.
  let (fx, fy) = (sx + sw / 2, sy + sh / 2);
  let (gx, gy) = (tx + tw / 2, ty + th / 2);
  let mv = |x: i32, y: i32| {
    crate::os_view::xdo(&["mousemove", &x.to_string(), &y.to_string()])
      .is_some()
  };
  let mut driven = mv(fx, fy);
  tokio::time::sleep(Duration::from_millis(200)).await;
  // Which window the press lands on, for the log.
  if let Some(at) = crate::os_view::xdo(&["getmouselocation", "--shell"]) {
    let window = at
      .lines()
      .find_map(|l| l.strip_prefix("WINDOW="))
      .unwrap_or("");
    eprintln!(
      "[e2e]   xdnd: pointer over window {window} (source {:?})",
      crate::os_view::x_window("laufey-xdnd-source")
    );
  }
  driven &= crate::os_view::xdo(&["mousedown", "1"]).is_some();
  for i in 1..=12 {
    tokio::time::sleep(Duration::from_millis(60)).await;
    driven &= mv(fx + (gx - fx) * i / 12, fy + (gy - fy) * i / 12);
  }
  let has = |phase: FileDragPhase| {
    events.lock().unwrap().iter().any(|e| e.phase == phase)
  };
  let entered = wait_for(|| has(FileDragPhase::Enter), 60, 50).await;
  // Then a few slow moves to the release point.
  let (rx, ry) = (gx + 20, gy + 15);
  for (x, y) in [(gx + 7, gy + 5), (gx + 14, gy + 10), (rx, ry)] {
    driven &= mv(x, y);
    tokio::time::sleep(Duration::from_millis(250)).await;
  }
  let over = wait_for(
    || {
      events.lock().unwrap().iter().any(|e| {
        e.phase == FileDragPhase::Over
          && (e.x - (rx - tx) as f64).abs() <= 2.0
          && (e.y - (ry - ty) as f64).abs() <= 2.0
      })
    },
    40,
    50,
  )
  .await;
  driven &= crate::os_view::xdo(&["mouseup", "1"]).is_some();
  check("xdotool drove the drag", driven);

  let dropped = wait_for(
    || {
      events
        .lock()
        .unwrap()
        .iter()
        .any(|e| e.phase == FileDragPhase::Drop)
    },
    100,
    50,
  )
  .await;
  let ev = events.lock().unwrap().clone();
  for e in &ev {
    eprintln!("[e2e]   xdnd event {e:?}");
  }
  // What the drag source saw ("drag-end" once GTK finished the drag).
  tokio::time::sleep(Duration::from_millis(700)).await;
  let mut drag_ended = false;
  while let Ok(line) = line_rx.try_recv() {
    eprintln!("[e2e]   xdnd source: {line}");
    drag_ended |= line == "drag-end";
  }
  // CEF used to report nothing here: CEF 149 calls OnDragEnter only for
  // Alloy-style browsers, so the paths now come from the XDND source itself
  // (cef/src/drag_paths_linux.cc).
  eprintln!("[e2e]   xdnd: the source saw the drag end: {drag_ended}");
  check(
    "a real XDND drag entering the window reports ENTER",
    entered,
  );
  check("moving over the window reports OVER at the pointer", over);
  check("a real XDND drop reaches on_file_drop", dropped);
  if dropped {
    let drop = ev.iter().find(|e| e.phase == FileDragPhase::Drop).unwrap();
    check(
      "the real drop carries the window and the dragged file's path",
      drop.window_id == w.id() && drop.paths == Some(vec![path.clone()]),
    );
    // At scale 1 a DIP is a pixel: the drop point is where the pointer was
    // released, relative to the content.
    let (ex, ey) = ((rx - tx) as f64, (ry - ty) as f64);
    check(
      &format!(
        "the real drop point is where it was released ({},{}; want {ex},{ey})",
        drop.x, drop.y
      ),
      (drop.x - ex).abs() <= 2.0 && (drop.y - ey).abs() <= 2.0,
    );
    check(
      "ENTER comes before the real DROP",
      ev.iter().position(|e| e.phase == FileDragPhase::Enter)
        < ev.iter().position(|e| e.phase == FileDragPhase::Drop)
        && ev.iter().any(|e| e.phase == FileDragPhase::Enter),
    );
  }
  let _ = child.kill();
  let _ = child.wait();
  laufey::clear_file_drop_handler();
  let _ = std::fs::remove_dir_all(&dir);
  w.close();
}

/// Windows: a real OLE drag of a file out of laufey_ole_drag_source (a
/// window whose whole area drags it as CF_HDROP with DoDragDrop, as Explorer
/// does), driven with SendInput (real pointer input) and released over the
/// window, reaches on_file_drop through the backend's own drop handling:
/// ENTER first, OVER at the pointer, then a DROP with the file's path.
async fn ole_drop_check(caps: &laufey::WindowCapabilities) {
  if !cfg!(windows) {
    return;
  }
  let source = std::env::var("LAUFEY_E2E_OLE_SOURCE").unwrap_or_default();
  if source.is_empty() {
    na("a real OLE drop (needs laufey_ole_drag_source)");
    return;
  }
  if !caps.file_drop() {
    na("a real OLE drop (backend has no file drops)");
  } else {
    #[cfg(windows)]
    ole_drop_run(&source).await;
  }
}

#[cfg(windows)]
async fn ole_drop_run(source: &str) {
  let dir =
    std::env::temp_dir().join(format!("laufey-e2e-ole-{}", std::process::id()));
  let file = dir.join("dropped file \u{e9}.txt");
  if std::fs::create_dir_all(&dir).is_err()
    || std::fs::write(&file, b"ole").is_err()
  {
    check("a real OLE drop: temp file", false);
    return;
  }
  let path = file.to_string_lossy().into_owned();

  let events: Arc<Mutex<Vec<FileDropEvent>>> = Arc::new(Mutex::new(Vec::new()));
  let sink = events.clone();
  laufey::on_file_drop(move |e| sink.lock().unwrap().push(e));

  const TITLE: &str = "native-e2e-ole";
  let loaded = Arc::new(std::sync::atomic::AtomicBool::new(false));
  let w = {
    let loaded = loaded.clone();
    Window::new(480, 360)
      .title(TITLE)
      .on_page_load(move |_| loaded.store(true, Ordering::SeqCst))
      .load(&format!("laufey-e2e://app/titled/{TITLE}"))
  };
  w.show();
  w.set_position(60, 60);
  let _ = wait_for(|| loaded.load(Ordering::SeqCst), 100, 50).await;
  tokio::time::sleep(Duration::from_millis(500)).await;
  // In front of everything else (a console window, the I/O window), or
  // the drop lands there: on top, and the foreground window if allowed.
  w.set_always_on_top(true);
  let hwnd = crate::menu_notification_checks::win::find_window(TITLE);
  let focused = crate::menu_notification_checks::win::focus(hwnd);
  tokio::time::sleep(Duration::from_millis(300)).await;
  let Some((tx, ty, tw, th)) = crate::os_view::content_rect(TITLE) else {
    check(
      "a real OLE drop: the window system reports the window",
      false,
    );
    laufey::clear_file_drop_handler();
    return;
  };
  // The source next to the window, on the primary display.
  let (sx0, sy0) = (tx + tw + 40, ty + 40);
  // CREATE_NO_WINDOW: a console window of its own (a Windows Terminal tab)
  // would open on top of the drop target.
  let mut command = std::process::Command::new(source);
  std::os::windows::process::CommandExt::creation_flags(
    &mut command,
    0x0800_0000,
  );
  let mut child = match command
    .args([&sx0.to_string(), &sy0.to_string(), &path])
    .stdout(std::process::Stdio::piped())
    .spawn()
  {
    Ok(c) => c,
    Err(e) => {
      check(
        &format!("a real OLE drop: start the drag source ({e})"),
        false,
      );
      laufey::clear_file_drop_handler();
      return;
    }
  };
  let stdout = child.stdout.take();
  let (line_tx, mut line_rx) = tokio::sync::mpsc::unbounded_channel();
  std::thread::spawn(move || {
    use std::io::BufRead;
    if let Some(out) = stdout {
      for line in std::io::BufReader::new(out).lines().map_while(Result::ok) {
        let _ = line_tx.send(line);
      }
    }
  });
  let ready = tokio::time::timeout(Duration::from_secs(15), async {
    while let Some(line) = line_rx.recv().await {
      let f: Vec<i32> = line
        .strip_prefix("ready ")
        .map(|r| r.split(' ').filter_map(|v| v.parse().ok()).collect())
        .unwrap_or_default();
      if f.len() == 4 {
        return Some((f[0], f[1], f[2], f[3]));
      }
    }
    None
  })
  .await
  .ok()
  .flatten();
  let Some((sx, sy, sw, sh)) = ready else {
    check("a real OLE drop: the drag source window shows", false);
    let _ = child.kill();
    laufey::clear_file_drop_handler();
    return;
  };
  eprintln!(
    "[e2e]   ole: source at {sx},{sy} {sw}x{sh}, target at {tx},{ty} {tw}x{th}"
  );

  // Press on the source (DoDragDrop starts there and tracks the pointer),
  // move in steps into the window, rest, and release over it.
  let (fx, fy) = (sx + sw / 2, sy + sh / 2);
  let (gx, gy) = (tx + tw / 2, ty + th / 2);
  let mut driven = ole_input::move_to(fx, fy);
  tokio::time::sleep(Duration::from_millis(200)).await;
  driven &= ole_input::button(true);
  tokio::time::sleep(Duration::from_millis(300)).await;
  for i in 1..=12 {
    driven &=
      ole_input::move_to(fx + (gx - fx) * i / 12, fy + (gy - fy) * i / 12);
    tokio::time::sleep(Duration::from_millis(60)).await;
  }
  let has = |phase: FileDragPhase| {
    events.lock().unwrap().iter().any(|e| e.phase == phase)
  };
  let entered = wait_for(|| has(FileDragPhase::Enter), 60, 50).await;
  let (rx, ry) = (gx + 20, gy + 15);
  for (x, y) in [(gx + 7, gy + 5), (gx + 14, gy + 10), (rx, ry)] {
    driven &= ole_input::move_to(x, y);
    tokio::time::sleep(Duration::from_millis(250)).await;
  }
  let over = wait_for(
    || {
      events.lock().unwrap().iter().any(|e| {
        e.phase == FileDragPhase::Over
          && (e.x - (rx - tx) as f64).abs() <= 2.0
          && (e.y - (ry - ty) as f64).abs() <= 2.0
      })
    },
    40,
    50,
  )
  .await;
  eprintln!(
    "[e2e]   ole: window in front {focused}; under the release point: {}",
    ole_input::window_at(rx, ry)
  );
  driven &= ole_input::button(false);
  check("SendInput drove the drag", driven);
  let dropped = wait_for(|| has(FileDragPhase::Drop), 100, 50).await;
  let ev = events.lock().unwrap().clone();
  for e in &ev {
    eprintln!("[e2e]   ole event {e:?}");
  }
  tokio::time::sleep(Duration::from_millis(700)).await;
  while let Ok(line) = line_rx.try_recv() {
    eprintln!("[e2e]   ole source: {line}");
  }
  check("a real OLE drag entering the window reports ENTER", entered);
  check("moving over the window reports OVER at the pointer", over);
  check("a real OLE drop reaches on_file_drop", dropped);
  if dropped {
    let drop = ev.iter().find(|e| e.phase == FileDragPhase::Drop).unwrap();
    check(
      &format!(
        "the real drop carries the window and the dragged file's path ({:?})",
        drop.paths
      ),
      drop.window_id == w.id() && drop.paths == Some(vec![path.clone()]),
    );
  }
  let _ = child.kill();
  let _ = child.wait();
  laufey::clear_file_drop_handler();
  let _ = std::fs::remove_dir_all(&dir);
  w.close();
}

/// Real pointer input for the OLE drop check: absolute moves and the left
/// button, through SendInput.
#[cfg(windows)]
mod ole_input {
  #[repr(C)]
  #[derive(Clone, Copy)]
  struct MouseInput {
    dx: i32,
    dy: i32,
    data: u32,
    flags: u32,
    time: u32,
    extra: usize,
  }

  #[repr(C)]
  #[derive(Clone, Copy)]
  struct Input {
    kind: u32,
    mi: MouseInput,
  }

  #[repr(C)]
  struct Point {
    x: i32,
    y: i32,
  }

  type Hwnd = *mut std::ffi::c_void;

  #[link(name = "user32")]
  extern "system" {
    fn SendInput(count: u32, inputs: *const Input, size: i32) -> u32;
    fn GetSystemMetrics(index: i32) -> i32;
    fn WindowFromPoint(point: Point) -> Hwnd;
    fn GetAncestor(hwnd: Hwnd, flags: u32) -> Hwnd;
    fn GetWindowTextW(hwnd: Hwnd, buf: *mut u16, len: i32) -> i32;
    fn GetClassNameW(hwnd: Hwnd, buf: *mut u16, len: i32) -> i32;
    fn GetWindowThreadProcessId(hwnd: Hwnd, pid: *mut u32) -> u32;
  }

  /// The top-level window at (x, y), for the log: title, class, and
  /// whether it is this process's.
  pub fn window_at(x: i32, y: i32) -> String {
    unsafe {
      let hwnd = WindowFromPoint(Point { x, y });
      if hwnd.is_null() {
        return "none".into();
      }
      let root = GetAncestor(hwnd, 2); // GA_ROOT
      let mut title = [0u16; 128];
      let mut class = [0u16; 128];
      let n = GetWindowTextW(root, title.as_mut_ptr(), 128).max(0) as usize;
      let c = GetClassNameW(root, class.as_mut_ptr(), 128).max(0) as usize;
      let mut pid = 0u32;
      GetWindowThreadProcessId(root, &mut pid);
      let owner = if pid == std::process::id() {
        "this process".to_string()
      } else {
        format!("pid {pid}")
      };
      format!(
        "{:?} ({}) of {owner}",
        String::from_utf16_lossy(&title[..n]),
        String::from_utf16_lossy(&class[..c]),
      )
    }
  }

  const MOUSEEVENTF_MOVE: u32 = 0x0001;
  const MOUSEEVENTF_LEFTDOWN: u32 = 0x0002;
  const MOUSEEVENTF_LEFTUP: u32 = 0x0004;
  const MOUSEEVENTF_ABSOLUTE: u32 = 0x8000;
  const MOUSEEVENTF_VIRTUALDESK: u32 = 0x4000;

  fn send(dx: i32, dy: i32, flags: u32) -> bool {
    let input = Input {
      kind: 0, // INPUT_MOUSE
      mi: MouseInput {
        dx,
        dy,
        data: 0,
        flags,
        time: 0,
        extra: 0,
      },
    };
    unsafe { SendInput(1, &input, std::mem::size_of::<Input>() as i32) == 1 }
  }

  /// Moves the pointer to (x, y) in physical virtual-screen pixels.
  pub fn move_to(x: i32, y: i32) -> bool {
    // SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SM_CXVIRTUALSCREEN,
    // SM_CYVIRTUALSCREEN: absolute input is 0..65535 over the virtual desk.
    let (vx, vy, vw, vh) = unsafe {
      (
        GetSystemMetrics(76),
        GetSystemMetrics(77),
        GetSystemMetrics(78),
        GetSystemMetrics(79),
      )
    };
    if vw <= 1 || vh <= 1 {
      return false;
    }
    let nx = ((x - vx) as i64 * 65535 / (vw - 1) as i64) as i32;
    let ny = ((y - vy) as i64 * 65535 / (vh - 1) as i64) as i32;
    send(
      nx,
      ny,
      MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
    )
  }

  pub fn button(down: bool) -> bool {
    send(
      0,
      0,
      if down {
        MOUSEEVENTF_LEFTDOWN
      } else {
        MOUSEEVENTF_LEFTUP
      },
    )
  }
}

// ---------------------------------------------------------------------------
// File dialogs
// ---------------------------------------------------------------------------

/// Polls the test hook until a dialog is up to act on (the backend shows it
/// asynchronously on its UI thread).
async fn respond(accept: bool, path: Option<&str>) -> bool {
  for _ in 0..100 {
    if laufey::test_file_dialog_respond(accept, path) {
      return true;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  false
}

async fn outcome_of<F: std::future::Future<Output = FileDialogOutcome>>(
  f: F,
) -> FileDialogOutcome {
  match tokio::time::timeout(Duration::from_secs(20), f).await {
    Ok(o) => o,
    Err(_) => FileDialogOutcome::Failed,
  }
}

/// Canonical form for comparing a path the dialog returned with the one we
/// asked for (macOS reports /tmp as /private/tmp; a save target may not
/// exist yet, so canonicalize its directory).
fn canon(p: &Path) -> PathBuf {
  if let Ok(c) = std::fs::canonicalize(p) {
    return c;
  }
  match (p.parent(), p.file_name()) {
    (Some(dir), Some(name)) => std::fs::canonicalize(dir)
      .map(|d| d.join(name))
      .unwrap_or_else(|_| p.to_path_buf()),
    _ => p.to_path_buf(),
  }
}

fn same_path(a: &str, b: &Path) -> bool {
  canon(Path::new(a)) == canon(b)
}

async fn dialog_checks(w: &Window, caps: &laufey::WindowCapabilities) {
  if !caps.file_dialogs() {
    let d = laufey::show_file_dialog(0, &FileDialogOptions::default());
    check(
      "show_file_dialog fails without the capability",
      d.id == 0 && outcome_of(d.outcome).await == FileDialogOutcome::Failed,
    );
    na("file dialogs (backend has none)");
    return;
  }
  let parent = if caps.file_dialog_modal() { w.id() } else { 0 };
  let dir =
    std::env::temp_dir().join(format!("laufey-io-{}", std::process::id()));
  let _ = std::fs::create_dir_all(&dir);
  let dir_str = dir.to_string_lossy().into_owned();

  // 1. Cancel through the test hook.
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      title: Some("laufey e2e: open".into()),
      default_path: Some(dir_str.clone()),
      filters: vec![FileFilter {
        name: "Text".into(),
        extensions: vec!["txt".into()],
      }],
      ..Default::default()
    },
  );
  check("an open dialog gets an id", d.id != 0);
  let acted = respond(false, None).await;
  check("the test hook finds the open dialog", acted);
  check(
    "a dialog closed by the hook resolves cancelled",
    outcome_of(d.outcome).await == FileDialogOutcome::Cancelled,
  );

  // 2. cancel_file_dialog, and BUSY while one is open.
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      kind: FileDialogKind::Save,
      title: Some("laufey e2e: save".into()),
      ..Default::default()
    },
  );
  let busy = laufey::show_file_dialog(0, &FileDialogOptions::default());
  check(
    "a second dialog is refused while one is open",
    busy.id == 0 && outcome_of(busy.outcome).await == FileDialogOutcome::Busy,
  );
  check(
    "cancel_file_dialog finds it",
    laufey::cancel_file_dialog(d.id),
  );
  check(
    "a cancelled dialog resolves cancelled",
    outcome_of(d.outcome).await == FileDialogOutcome::Cancelled,
  );
  check(
    "cancel_file_dialog of a closed dialog is false",
    !laufey::cancel_file_dialog(d.id),
  );

  // 3. Accept a save dialog with a typed-in path.
  let target = dir.join("saved by laufey.txt");
  let _ = std::fs::remove_file(&target);
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      kind: FileDialogKind::Save,
      default_path: Some(dir_str.clone()),
      ..Default::default()
    },
  );
  let acted = respond(true, Some(&target.to_string_lossy())).await;
  let o = outcome_of(d.outcome).await;
  eprintln!("[e2e]   save accept -> {o:?}");
  accept_check("save dialog returns the typed path", acted, &o, &[&target]);

  // 4. Accept an open dialog on an existing file.
  let file = dir.join("open me.txt");
  let _ = std::fs::write(&file, b"laufey");
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      files: true,
      default_path: Some(dir_str.clone()),
      ..Default::default()
    },
  );
  let acted = respond(true, Some(&file.to_string_lossy())).await;
  let o = outcome_of(d.outcome).await;
  eprintln!("[e2e]   open-file accept -> {o:?}");
  accept_check("open dialog returns the chosen file", acted, &o, &[&file]);

  // 5. Accept a folder dialog on a directory.
  let sub = dir.join("pick me");
  let _ = std::fs::create_dir_all(&sub);
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      directories: true,
      default_path: Some(dir_str.clone()),
      ..Default::default()
    },
  );
  let acted = respond(true, Some(&sub.to_string_lossy())).await;
  let o = outcome_of(d.outcome).await;
  eprintln!("[e2e]   folder accept -> {o:?}");
  accept_check(
    "folder dialog returns the chosen directory",
    acted,
    &o,
    &[&sub],
  );

  let _ = std::fs::remove_dir_all(&dir);
}

/// `cancel_file_dialog` (what an embedder's abort calls) closes a dialog of
/// any kind whenever it arrives: before the dialog is shown, while it is
/// being built, or long after its window is up. Each one settles cancelled
/// exactly once (the outcome channel would report a second settle as a
/// failure on the next dialog: the slot would be busy), and the slot is
/// free right after. On Windows the dialog's window must also be gone.
async fn dialog_abort_stress(w: &Window, caps: &laufey::WindowCapabilities) {
  if !caps.file_dialogs() {
    na("file dialog abort stress (backend has no file dialogs)");
    return;
  }
  let modal_parent = if caps.file_dialog_modal() { w.id() } else { 0 };
  // LAUFEY_E2E_ABORT_ROUNDS: more rounds when hunting a race by hand.
  let rounds: usize = std::env::var("LAUFEY_E2E_ABORT_ROUNDS")
    .ok()
    .and_then(|v| v.parse().ok())
    .unwrap_or(12);
  let delays_ms = [0u64, 50, 250, 700, 1500, 2000];
  let mut settled = 0usize;
  for i in 0..rounds {
    let delay = delays_ms[i % delays_ms.len()];
    let kind = i % 3; // open file(s), save, folder
    let modal = (i / 3) % 2 == 1;
    let title = format!("laufey e2e abort {i}");
    let mut options = FileDialogOptions {
      title: Some(title.clone()),
      ..Default::default()
    };
    match kind {
      0 => {
        options.multiple = true;
        options.filters = vec![FileFilter {
          name: "Text".into(),
          extensions: vec!["txt".into()],
        }];
      }
      1 => {
        options.kind = FileDialogKind::Save;
        options.default_path = Some("e2e.txt".into());
      }
      _ => options.directories = true,
    }
    let label = format!(
      "{} dialog ({}), aborted after {delay} ms",
      ["open", "save", "folder"][kind],
      if modal { "modal" } else { "app-level" }
    );
    let d =
      laufey::show_file_dialog(if modal { modal_parent } else { 0 }, &options);
    if d.id == 0 {
      check(&format!("{label}: the dialog opens"), false);
      break;
    }
    tokio::time::sleep(Duration::from_millis(delay)).await;
    #[cfg(windows)]
    let window_seen = delay >= 1500 && win_dialog::exists(&title);
    let found = laufey::cancel_file_dialog(d.id);
    let outcome =
      tokio::time::timeout(Duration::from_secs(10), d.outcome).await;
    let ok = matches!(outcome, Ok(FileDialogOutcome::Cancelled));
    check(
      &format!("{label}: settles cancelled ({found}, {outcome:?})"),
      found && ok,
    );
    if !ok {
      // A dialog that never settled holds the slot: the rest can't run.
      break;
    }
    settled += 1;
    #[cfg(windows)]
    {
      if delay >= 1500 {
        eprintln!("[e2e]   {label}: its window was up: {window_seen}");
      }
      check(
        &format!("{label}: its window is gone"),
        wait_for(|| !win_dialog::exists(&title), 50, 100).await,
      );
    }
    // Exactly once: the slot is free at once, so a new dialog opens (and is
    // cancelled before it shows).
    let next = laufey::show_file_dialog(0, &FileDialogOptions::default());
    let freed = next.id != 0;
    if freed {
      laufey::cancel_file_dialog(next.id);
    }
    let next_outcome =
      tokio::time::timeout(Duration::from_secs(10), next.outcome).await;
    check(
      &format!("{label}: settles once and frees the slot"),
      freed && matches!(next_outcome, Ok(FileDialogOutcome::Cancelled)),
    );
  }
  check(
    &format!("every aborted dialog settled ({settled}/{rounds})"),
    settled == rounds,
  );
}

/// Windows: whether a visible window of this process has `title` (the file
/// dialog's own top-level window).
#[cfg(windows)]
mod win_dialog {
  use std::ffi::c_void;

  #[link(name = "user32")]
  extern "system" {
    fn EnumWindows(
      cb: unsafe extern "system" fn(*mut c_void, isize) -> i32,
      lparam: isize,
    ) -> i32;
    fn GetWindowThreadProcessId(hwnd: *mut c_void, pid: *mut u32) -> u32;
    fn IsWindowVisible(hwnd: *mut c_void) -> i32;
    fn GetWindowTextW(hwnd: *mut c_void, buf: *mut u16, len: i32) -> i32;
  }

  pub fn exists(title: &str) -> bool {
    struct Search {
      pid: u32,
      title: Vec<u16>,
      found: bool,
    }
    unsafe extern "system" fn visit(hwnd: *mut c_void, lparam: isize) -> i32 {
      let search = &mut *(lparam as *mut Search);
      let mut pid = 0u32;
      GetWindowThreadProcessId(hwnd, &mut pid);
      if pid != search.pid || IsWindowVisible(hwnd) == 0 {
        return 1;
      }
      let mut buf = [0u16; 256];
      let n = GetWindowTextW(hwnd, buf.as_mut_ptr(), buf.len() as i32);
      if n > 0 && buf[..n as usize] == search.title[..] {
        search.found = true;
        return 0;
      }
      1
    }
    let mut search = Search {
      pid: std::process::id(),
      title: title.encode_utf16().collect(),
      found: false,
    };
    unsafe { EnumWindows(visit, &mut search as *mut Search as isize) };
    search.found
  }
}

fn accept_check(
  name: &str,
  acted: bool,
  outcome: &FileDialogOutcome,
  expected: &[&Path],
) {
  if !acted {
    check(&format!("{name} (the hook found the dialog)"), false);
    return;
  }
  match outcome {
    FileDialogOutcome::Accepted(paths) => check(
      name,
      paths.len() == expected.len()
        && paths.iter().zip(expected).all(|(p, e)| same_path(p, e)),
    ),
    // The hook acted, but the platform dialog wouldn't take the typed path
    // (e.g. a portal dialog); the dialog still closed exactly once.
    FileDialogOutcome::Cancelled => na(&format!(
      "{name} (this dialog can't be accepted programmatically)"
    )),
    _ => check(name, false),
  }
}

// ---------------------------------------------------------------------------
// Drag out
// ---------------------------------------------------------------------------

async fn drag_out_checks(w: &Window, caps: &laufey::WindowCapabilities) {
  let me = std::env::current_exe().unwrap_or_default();
  let me = me.to_string_lossy().into_owned();
  let r = tokio::time::timeout(
    Duration::from_secs(10),
    laufey::start_file_drag(w.id(), &["relative/path.txt"], None),
  )
  .await;
  eprintln!("[e2e]   relative-path drag -> {r:?}");
  check(
    "a drag of a relative path fails",
    matches!(r, Ok(DragResult::Failed)),
  );
  let r = tokio::time::timeout(
    Duration::from_secs(10),
    laufey::start_file_drag(w.id(), &[&me], None),
  )
  .await;
  eprintln!("[e2e]   no-button drag -> {r:?}");
  if caps.file_drag_out() {
    // No mouse button is held in an automated run, so the OS can't start a
    // drag: refused, through the backend's UI-thread path.
    check(
      "a drag without a held mouse button fails",
      matches!(r, Ok(DragResult::Failed)),
    );
  } else {
    check(
      "drag-out fails without the capability",
      matches!(r, Ok(DragResult::Failed)),
    );
    na("file drag-out (backend has none)");
  }
  na("a real drag out (needs a person or OS input injection)");
}
