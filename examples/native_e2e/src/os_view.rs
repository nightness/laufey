// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The window system's own view of a window, and real input.
//!
//! - `content_rect` reads where a window's content really is, in physical
//!   pixels, from the window system rather than from laufey: X11 through
//!   xwininfo, Windows through Win32. The HiDPI checks compare it with
//!   laufey's DIP sizes and positions.
//! - On X11, `xdo` drives xdotool (XTEST) for real key presses and pointer
//!   drags. scripts/native-e2e-run.sh sets LAUFEY_E2E_XDOTOOL on Linux when
//!   xdotool is installed; without it the real-input checks are N/A, and with
//!   it they must pass.

/// xdotool, when the run provides it (X11 only).
pub fn xdotool() -> Option<String> {
  if !cfg!(target_os = "linux") {
    return None;
  }
  std::env::var("LAUFEY_E2E_XDOTOOL")
    .ok()
    .filter(|p| !p.is_empty())
}

/// Runs xdotool with `args`; its stdout on success.
pub fn xdo(args: &[&str]) -> Option<String> {
  let tool = xdotool()?;
  let out = std::process::Command::new(tool).args(args).output().ok()?;
  if !out.status.success() {
    eprintln!(
      "[e2e]   xdotool {args:?} failed: {}",
      String::from_utf8_lossy(&out.stderr).trim()
    );
    return None;
  }
  Some(String::from_utf8_lossy(&out.stdout).into_owned())
}

/// The X window whose title is exactly `title`.
#[cfg_attr(windows, allow(dead_code))]
pub fn x_window(title: &str) -> Option<String> {
  let pattern = format!("^{}$", title.replace('.', "\\."));
  let found =
    xdo(&["search", "--onlyvisible", "--name", &pattern]).and_then(|out| {
      out
        .lines()
        .map(str::trim)
        .find(|l| !l.is_empty())
        .map(String::from)
    });
  if found.is_none() {
    // Say what is there, for the log.
    let ids =
      xdo(&["search", "--onlyvisible", "--name", "."]).unwrap_or_default();
    for id in ids.lines().map(str::trim).filter(|l| !l.is_empty()) {
      let name = xdo(&["getwindowname", id]).unwrap_or_default();
      eprintln!("[e2e]   visible X window {id}: {:?}", name.trim());
    }
    eprintln!("[e2e]   no visible X window titled {title:?}");
  }
  found
}

/// Gives the X window titled `title` the keyboard focus: through the window
/// manager when one runs (`_NET_ACTIVE_WINDOW`, then waits until it is the
/// active window), else directly (XSetInputFocus).
#[cfg_attr(windows, allow(dead_code))]
pub async fn x_focus(title: &str) -> bool {
  let Some(id) = x_window(title) else {
    return false;
  };
  if std::env::var_os("LAUFEY_E2E_WM_RUNNING").is_none() {
    return xdo(&["windowfocus", &id]).is_some();
  }
  if xdo(&["windowactivate", &id]).is_none() {
    return false;
  }
  for _ in 0..40 {
    if xdo(&["getactivewindow"]).is_some_and(|a| a.trim() == id) {
      return true;
    }
    tokio::time::sleep(std::time::Duration::from_millis(50)).await;
  }
  false
}

/// The content area of the window titled `title`, in physical pixels of the
/// screen: (x, y, width, height).
pub fn content_rect(title: &str) -> Option<(i32, i32, i32, i32)> {
  #[cfg(target_os = "linux")]
  {
    // xwininfo's absolute origin (XTranslateCoordinates to the root):
    // xdotool getwindowgeometry is off by the frame for a window a
    // reparenting window manager (openbox) has framed.
    let id = x_window(title)?;
    let out = std::process::Command::new("xwininfo")
      .args(["-id", &id])
      .output()
      .ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    let field = |label: &str| -> Option<i32> {
      text
        .lines()
        .find_map(|l| l.trim().strip_prefix(label))
        .and_then(|v| v.trim().parse().ok())
    };
    Some((
      field("Absolute upper-left X:")?,
      field("Absolute upper-left Y:")?,
      field("Width:")?,
      field("Height:")?,
    ))
  }
  #[cfg(windows)]
  {
    crate::menu_notification_checks::win::client_rect(title)
  }
  #[cfg(not(any(target_os = "linux", windows)))]
  {
    let _ = title;
    None
  }
}
