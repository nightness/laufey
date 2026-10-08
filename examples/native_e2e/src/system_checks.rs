// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! LAUFEY_E2E_ONLY=system | devtools-off (and the shortcut-holder helper):
//! global shortcuts, launch at login and DevTools control (API 40). See
//! docs/e2e-testing.md.
//!
//! - Global shortcuts: register / list / unregister round trip with the
//!   canonical form, ALREADY_REGISTERED for another spelling, INVALID for a
//!   bad or modifier-less accelerator, a press through the test hook (and,
//!   on Windows, a real key press through SendInput), and a CONFLICT the OS
//!   reports while a second process (this binary in shortcut-holder mode)
//!   holds the same combination, then OK once it let go.
//! - Launch at login: on -> state read back (plus the OS artefact: the HKCU
//!   Run value, the XDG autostart file) -> off -> read back. Only in CI or
//!   with LAUFEY_E2E_LOGIN_ITEM=1, so a developer's machine is left alone;
//!   the original state is restored.
//! - DevTools: enabled (launch setting and the engine's own setting read
//!   back), open / close / toggle round trips. devtools-off runs under
//!   LAUFEY_INSPECTABLE=0 and checks that everything stays closed.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{LoginItemState, ShortcutError, Window};

use super::{check, na, wait_for};

/// Unlikely to be bound by anything on a CI runner or a dev machine.
const SHORTCUT: &str = "Ctrl+Alt+Shift+F10";
const HELD_SHORTCUT: &str = "Ctrl+Alt+Shift+F11";

pub async fn run() {
  let caps = laufey::system_capabilities();
  eprintln!(
    "[e2e] system capabilities = {:#x} (shortcuts={} user_binds={} login={} devtools={})",
    caps.bits,
    caps.global_shortcuts(),
    caps.shortcuts_user_binds(),
    caps.launch_at_login(),
    caps.devtools()
  );
  shortcut_checks(&caps).await;
  login_item_checks(&caps).await;
  devtools_checks(&caps, true).await;
}

pub async fn devtools_off() {
  let caps = laufey::system_capabilities();
  devtools_checks(&caps, false).await;
}

// ---------------------------------------------------------------------------
// Global shortcuts
// ---------------------------------------------------------------------------

async fn shortcut_checks(caps: &laufey::SystemCapabilities) {
  if !caps.global_shortcuts() {
    // Winit, or Linux with neither X11 nor the portal.
    na("global shortcuts (not supported by this backend / session)");
    check(
      "register_shortcut is NOT_SUPPORTED without the capability",
      laufey::register_shortcut(SHORTCUT).await
        == Err(ShortcutError::NotSupported),
    );
    return;
  }
  if caps.shortcuts_user_binds() {
    // The portal asks the user; nothing can answer its dialog in CI.
    na("global shortcuts (the portal needs a user to approve them)");
    return;
  }

  let pressed: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
  {
    let pressed = pressed.clone();
    laufey::on_shortcut(move |accel| {
      pressed.lock().unwrap().push(accel.to_string());
    });
  }

  let canonical = laufey::register_shortcut(SHORTCUT).await;
  eprintln!("[e2e] register_shortcut({SHORTCUT}) = {canonical:?}");
  check(
    "register_shortcut answers OK with the canonical form",
    canonical.as_deref() == Ok(SHORTCUT),
  );
  check(
    "canonical_accelerator maps another spelling",
    laufey::canonical_accelerator("shift+alt+control+f10").as_deref()
      == Some(SHORTCUT),
  );
  check(
    "canonical_accelerator refuses garbage",
    laufey::canonical_accelerator("Ctrl+Nope").is_none(),
  );
  check(
    "shortcuts() lists it",
    laufey::shortcuts() == vec![SHORTCUT.to_string()],
  );
  check(
    "another spelling is ALREADY_REGISTERED",
    laufey::register_shortcut("shift+alt+control+f10").await
      == Err(ShortcutError::AlreadyRegistered),
  );
  check(
    "a printable key without a modifier is INVALID",
    laufey::register_shortcut("K").await == Err(ShortcutError::Invalid),
  );
  check(
    "a navigation key without a modifier is INVALID",
    laufey::register_shortcut("Escape").await == Err(ShortcutError::Invalid)
      && laufey::register_shortcut("Shift+Up").await
        == Err(ShortcutError::Invalid),
  );
  check(
    "an unknown key is INVALID",
    laufey::register_shortcut("Ctrl+Nope").await == Err(ShortcutError::Invalid),
  );

  // A press through the backend's own dispatch.
  check(
    "test_trigger_shortcut reaches the handler",
    laufey::test_trigger_shortcut("Shift+Alt+Ctrl+F10"),
  );
  check(
    "the handler gets the canonical accelerator",
    wait_for(
      || pressed.lock().unwrap().iter().any(|a| a == SHORTCUT),
      50,
      20,
    )
    .await,
  );

  real_press_check(&pressed).await;
  conflict_checks().await;

  check(
    "unregister_shortcut releases it",
    laufey::unregister_shortcut(SHORTCUT),
  );
  check("shortcuts() is empty again", laufey::shortcuts().is_empty());
  check(
    "a second unregister reports false",
    !laufey::unregister_shortcut(SHORTCUT),
  );
  check(
    "test_trigger_shortcut refuses an unregistered shortcut",
    !laufey::test_trigger_shortcut(SHORTCUT),
  );
  // And it can be taken again (the OS binding was really released).
  check(
    "re-registering after unregister is OK",
    laufey::register_shortcut(SHORTCUT).await.is_ok(),
  );
  laufey::unregister_all_shortcuts();
  check(
    "unregister_all_shortcuts empties the list",
    laufey::shortcuts().is_empty(),
  );
  laufey::clear_shortcut_handler();
}

/// A real key press reaches the global shortcut: on Windows injected with
/// SendInput and arriving as WM_HOTKEY on the UI thread; on X11 injected
/// with xdotool (XTEST), which the X server matches against the shortcut's
/// passive key grab. (macOS needs Accessibility permission to post key
/// events, so it relies on the test hook above.)
async fn real_press_check(pressed: &Arc<Mutex<Vec<String>>>) {
  let before = pressed.lock().unwrap().len();
  #[cfg(windows)]
  {
    let sent = win_input::press_ctrl_alt_shift_f10();
    check("SendInput injected the key press", sent);
  }
  #[cfg(not(windows))]
  {
    if crate::os_view::xdotool().is_none() {
      na("a real key press (needs OS input injection: SendInput on Windows, xdotool on X11)");
      return;
    }
    check(
      "xdotool injected Ctrl+Alt+Shift+F10",
      crate::os_view::xdo(&["key", "--clearmodifiers", "ctrl+alt+shift+F10"])
        .is_some(),
    );
  }
  check(
    "a real key press fires the global shortcut",
    wait_for(|| pressed.lock().unwrap().len() > before, 100, 20).await,
  );
  check(
    "the real press carries the canonical accelerator",
    pressed.lock().unwrap()[before..]
      .iter()
      .all(|a| a == SHORTCUT),
  );
}

/// A second process holds HELD_SHORTCUT: this one gets CONFLICT from the
/// OS, then OK once the holder has let go.
async fn conflict_checks() {
  let dir = std::env::temp_dir()
    .join(format!("laufey-e2e-shortcut-holder-{}", std::process::id()));
  let _ = std::fs::remove_dir_all(&dir);
  if std::fs::create_dir_all(&dir).is_err() {
    check("shortcut holder: temp dir", false);
    return;
  }
  let exe = match std::env::current_exe() {
    Ok(e) => e,
    Err(_) => {
      check("shortcut holder: current_exe", false);
      return;
    }
  };
  let mut cmd = std::process::Command::new(&exe);
  cmd
    .env("LAUFEY_E2E_ONLY", "shortcut-holder")
    .env("LAUFEY_E2E_HOLDER_DIR", &dir)
    // Its own web profile (CEF locks a profile directory per process).
    .env("LAUFEY_DATA_DIR", dir.join("data"))
    .env_remove("LAUFEY_SINGLE_INSTANCE");
  for arg in std::env::args().skip(1) {
    cmd.arg(arg);
  }
  let mut child = match cmd.spawn() {
    Ok(c) => c,
    Err(e) => {
      eprintln!("[e2e] spawning the shortcut holder failed: {e}");
      check("shortcut holder starts", false);
      return;
    }
  };
  let ready = dir.join("ready");
  let got_ready = wait_for(|| ready.exists(), 600, 50).await;
  let holder_status = std::fs::read_to_string(&ready).unwrap_or_default();
  eprintln!("[e2e] shortcut holder: {}", holder_status.trim());
  check(
    "the second process holds the shortcut",
    got_ready && holder_status.trim() == "ok",
  );
  if got_ready && holder_status.trim() == "ok" {
    let r = laufey::register_shortcut(HELD_SHORTCUT).await;
    eprintln!("[e2e] register_shortcut({HELD_SHORTCUT}) while held = {r:?}");
    check(
      "the OS refuses a shortcut another process holds (CONFLICT)",
      r == Err(ShortcutError::Conflict),
    );
    check(
      "a refused shortcut isn't listed",
      !laufey::shortcuts().iter().any(|s| s == HELD_SHORTCUT),
    );
  }
  let _ = std::fs::write(dir.join("release"), b"1");
  let mut exited = false;
  for _ in 0..600 {
    if matches!(child.try_wait(), Ok(Some(_))) {
      exited = true;
      break;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  if !exited {
    let _ = child.kill();
  }
  check("the shortcut holder exits", exited);
  if got_ready && holder_status.trim() == "ok" {
    let r = laufey::register_shortcut(HELD_SHORTCUT).await;
    check(
      "the shortcut is free once the other process let go",
      r.as_deref() == Ok(HELD_SHORTCUT),
    );
    laufey::unregister_shortcut(HELD_SHORTCUT);
  }
  let _ = child.wait();
  let _ = std::fs::remove_dir_all(&dir);
}

/// The shortcut-holder process: registers HELD_SHORTCUT, reports, and holds
/// it until told to let go (or a minute passes).
pub async fn hold_shortcut() {
  let Some(dir) = std::env::var_os("LAUFEY_E2E_HOLDER_DIR").map(PathBuf::from)
  else {
    eprintln!("[e2e-holder] LAUFEY_E2E_HOLDER_DIR is not set");
    return;
  };
  let r = laufey::register_shortcut(HELD_SHORTCUT).await;
  let status = match &r {
    Ok(_) => "ok".to_string(),
    Err(e) => e.code().to_string(),
  };
  let tmp = dir.join("ready.tmp");
  let _ = std::fs::write(&tmp, status.as_bytes());
  let _ = std::fs::rename(&tmp, dir.join("ready"));
  let release = dir.join("release");
  wait_for(|| release.exists(), 1200, 50).await;
  laufey::unregister_all_shortcuts();
  // Let the UI thread release the OS binding before the process ends.
  tokio::time::sleep(Duration::from_millis(200)).await;
}

#[cfg(windows)]
mod win_input {
  #[repr(C)]
  #[derive(Clone, Copy)]
  struct KeybdInput {
    vk: u16,
    scan: u16,
    flags: u32,
    time: u32,
    extra: usize,
  }

  // INPUT: the type, then a union whose largest member (MOUSEINPUT) is 8
  // bytes bigger than KEYBDINPUT.
  #[repr(C)]
  #[derive(Clone, Copy)]
  struct Input {
    kind: u32,
    ki: KeybdInput,
    _pad: [u8; 8],
  }

  extern "system" {
    fn SendInput(count: u32, inputs: *const Input, size: i32) -> u32;
  }

  const INPUT_KEYBOARD: u32 = 1;
  const KEYEVENTF_KEYUP: u32 = 2;

  fn key(vk: u16, up: bool) -> Input {
    Input {
      kind: INPUT_KEYBOARD,
      ki: KeybdInput {
        vk,
        scan: 0,
        flags: if up { KEYEVENTF_KEYUP } else { 0 },
        time: 0,
        extra: 0,
      },
      _pad: [0; 8],
    }
  }

  pub fn press_ctrl_alt_shift_f10() -> bool {
    const CTRL: u16 = 0x11;
    const ALT: u16 = 0x12;
    const SHIFT: u16 = 0x10;
    const F10: u16 = 0x79;
    let seq = [
      key(CTRL, false),
      key(ALT, false),
      key(SHIFT, false),
      key(F10, false),
      key(F10, true),
      key(SHIFT, true),
      key(ALT, true),
      key(CTRL, true),
    ];
    let sent = unsafe {
      SendInput(
        seq.len() as u32,
        seq.as_ptr(),
        std::mem::size_of::<Input>() as i32,
      )
    };
    sent as usize == seq.len()
  }
}

// ---------------------------------------------------------------------------
// Launch at login
// ---------------------------------------------------------------------------

async fn login_item_checks(caps: &laufey::SystemCapabilities) {
  if !caps.launch_at_login() {
    na("launch at login (not supported by this backend / OS)");
    check(
      "launch_at_login is NOT_SUPPORTED without the capability",
      laufey::launch_at_login() == LoginItemState::NotSupported,
    );
    return;
  }
  let allowed = std::env::var_os("CI").is_some()
    || std::env::var("LAUFEY_E2E_LOGIN_ITEM").as_deref() == Ok("1");
  if !allowed {
    na("launch at login round trip (set CI or LAUFEY_E2E_LOGIN_ITEM=1)");
    return;
  }
  let initial = laufey::launch_at_login();
  eprintln!("[e2e] launch_at_login (initial) = {initial:?}");

  let on = laufey::set_launch_at_login(true);
  eprintln!("[e2e] set_launch_at_login(true) = {on:?}");
  let on_ok = matches!(
    on,
    Ok(LoginItemState::Enabled) | Ok(LoginItemState::RequiresApproval)
  );
  check("set_launch_at_login(true) turns it on", on_ok);
  if let Ok(state) = on {
    check(
      "launch_at_login reads the new state back",
      laufey::launch_at_login() == state,
    );
  }
  check(
    "the OS has the login entry",
    login_artifact_present() != Some(false),
  );

  let off = laufey::set_launch_at_login(false);
  eprintln!("[e2e] set_launch_at_login(false) = {off:?}");
  check(
    "set_launch_at_login(false) turns it off",
    off == Ok(LoginItemState::Disabled),
  );
  check(
    "launch_at_login reads disabled back",
    laufey::launch_at_login() == LoginItemState::Disabled,
  );
  check(
    "the OS login entry is gone",
    login_artifact_present() != Some(true),
  );

  if matches!(
    initial,
    LoginItemState::Enabled | LoginItemState::RequiresApproval
  ) {
    let _ = laufey::set_launch_at_login(true);
  }
}

fn login_item_name() -> String {
  if let Ok(id) = std::env::var("LAUFEY_APP_ID") {
    if !id.is_empty() {
      return id;
    }
  }
  std::env::current_exe()
    .ok()
    .and_then(|p| p.file_stem().map(|s| s.to_string_lossy().into_owned()))
    .unwrap_or_default()
}

/// Whether the OS-level entry exists: the XDG autostart file on Linux, the
/// HKCU Run value on Windows. None where it can't be checked from outside
/// (macOS keeps SMAppService state in its own database).
fn login_artifact_present() -> Option<bool> {
  let name = login_item_name();
  if cfg!(target_os = "linux") {
    let base = std::env::var_os("XDG_CONFIG_HOME")
      .map(PathBuf::from)
      .filter(|p| p.is_absolute())
      .or_else(|| {
        std::env::var_os("HOME").map(|h| Path::new(&h).join(".config"))
      })?;
    let file = base.join("autostart").join(format!("{name}.desktop"));
    let present = file.exists();
    if present {
      let text = std::fs::read_to_string(&file).unwrap_or_default();
      let exe = std::env::current_exe().ok()?;
      eprintln!("[e2e] {}:\n{text}", file.display());
      return Some(text.contains(&*exe.to_string_lossy()));
    }
    return Some(false);
  }
  if cfg!(windows) {
    let out = std::process::Command::new("reg")
      .args([
        "query",
        r"HKCU\Software\Microsoft\Windows\CurrentVersion\Run",
        "/v",
        &name,
      ])
      .output()
      .ok()?;
    let text = String::from_utf8_lossy(&out.stdout).to_string();
    if !out.status.success() {
      return Some(false);
    }
    let exe = std::env::current_exe().ok()?;
    eprintln!("[e2e] HKCU Run value: {}", text.trim());
    return Some(
      text
        .to_lowercase()
        .contains(&exe.to_string_lossy().to_lowercase()),
    );
  }
  None
}

// ---------------------------------------------------------------------------
// DevTools
// ---------------------------------------------------------------------------

async fn devtools_checks(caps: &laufey::SystemCapabilities, expect_on: bool) {
  if !caps.devtools() {
    na("DevTools (no web engine)");
    check("devtools_enabled is false without an engine", {
      !laufey::devtools_enabled()
    });
    return;
  }
  check(
    if expect_on {
      "DevTools are enabled by default"
    } else {
      "LAUFEY_INSPECTABLE=0 disables DevTools"
    },
    laufey::devtools_enabled() == expect_on,
  );

  let w = Window::new(480, 360).title("native-e2e-devtools");
  w.navigate("about:blank");
  w.show();
  if !wait_for(|| w.get_size().0 != 0, 100, 50).await {
    check("devtools window reports a size", false);
    return;
  }
  // The engine's own setting, read back (the web view may still be coming
  // up on WebView2).
  check(
    "the engine's DevTools setting matches the launch setting",
    wait_for(|| w.is_devtools_enabled() == expect_on, 100, 50).await,
  );
  check("DevTools start closed", !w.is_devtools_open());

  if expect_on {
    w.open_devtools();
    check(
      "open_devtools opens them",
      wait_for(|| w.is_devtools_open(), 300, 50).await,
    );
    w.close_devtools();
    check(
      "close_devtools closes them",
      wait_for(|| !w.is_devtools_open(), 300, 50).await,
    );
    w.toggle_devtools();
    check(
      "toggle_devtools opens them",
      wait_for(|| w.is_devtools_open(), 300, 50).await,
    );
    w.toggle_devtools();
    check(
      "toggle_devtools closes them again",
      wait_for(|| !w.is_devtools_open(), 300, 50).await,
    );
    // Closed while still opening: the engine may finish opening them after
    // the call returns (WebKitGTK shows the inspector once its web process
    // answers), and that must not bring them back.
    w.open_devtools();
    w.close_devtools();
    check(
      "DevTools closed right after open read as closed",
      !w.is_devtools_open(),
    );
    tokio::time::sleep(Duration::from_secs(3)).await;
    check(
      "DevTools closed right after open stay closed",
      !w.is_devtools_open(),
    );
    w.toggle_devtools();
    check(
      "toggle_devtools opens them after that",
      wait_for(|| w.is_devtools_open(), 300, 50).await,
    );
    w.close_devtools();
    check(
      "close_devtools closes them after that",
      wait_for(|| !w.is_devtools_open(), 300, 50).await,
    );
  } else {
    w.open_devtools();
    w.toggle_devtools();
    tokio::time::sleep(Duration::from_secs(3)).await;
    check(
      "open_devtools / toggle_devtools do nothing",
      !w.is_devtools_open(),
    );
    // CEF renders PDFs through the DevTools protocol (Page.printToPDF);
    // turning the DevTools UI off must not break it. (The other engines'
    // PDF paths don't involve DevTools; the main battery covers them.)
    if std::env::var("LAUFEY_E2E_BACKEND").as_deref() != Ok("cef") {
      na("print_to_pdf with DevTools off (only CEF prints through DevTools)");
      w.close();
      return;
    }
    let mut outcome = None;
    for attempt in 0..3u32 {
      if attempt > 0 {
        tokio::time::sleep(Duration::from_millis(700)).await;
      }
      let slot = Arc::new(Mutex::new(None::<Result<Vec<u8>, String>>));
      let s2 = slot.clone();
      w.print_to_pdf(None, move |r| *s2.lock().unwrap() = Some(r));
      if !wait_for(|| slot.lock().unwrap().is_some(), 150, 100).await {
        continue;
      }
      let r = slot.lock().unwrap().take().unwrap();
      let transient = matches!(&r, Err(e) if !e.contains("not supported"));
      outcome = Some(r);
      if !transient {
        break;
      }
    }
    match outcome {
      Some(Ok(bytes)) => check(
        "print_to_pdf still works with DevTools off",
        bytes.starts_with(b"%PDF-"),
      ),
      Some(Err(e)) if e.contains("not supported") => {
        na("print_to_pdf with DevTools off (no PDF support)")
      }
      other => {
        eprintln!("[e2e] print_to_pdf with DevTools off: {other:?}");
        check("print_to_pdf still works with DevTools off", false);
      }
    }
  }
  w.close();
}
