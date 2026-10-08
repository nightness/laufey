// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Window state, size constraints, screens, normal bounds and app lifetime
//! for the winit backends (API 38). The C ABI entry points live in
//! `define_common_backend_fns!`; this module keeps the bookkeeping and does
//! the winit calls on the event-loop thread.
//!
//! What winit can't do is left out of `capabilities()`: display-changed
//! events (winit has no monitor-change event), title bar styles and
//! backdrops. A screen's work area is its bounds (winit doesn't report the
//! work area).

use std::collections::HashMap;
use std::ffi::{c_int, c_void};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Condvar, Mutex, OnceLock};
use std::time::{Duration, Instant};

use winit::dpi::LogicalSize;
use winit::monitor::MonitorHandle;
use winit::window::{Fullscreen, Window};

use crate::{LaufeyScreen, LaufeyWindowStateFn};

pub const STATE_MAXIMIZED: u32 = 1 << 0;
pub const STATE_MINIMIZED: u32 = 1 << 1;
pub const STATE_FULLSCREEN: u32 = 1 << 2;

pub const ACTION_MAXIMIZE: c_int = 1;
pub const ACTION_UNMAXIMIZE: c_int = 2;
pub const ACTION_MINIMIZE: c_int = 3;
pub const ACTION_RESTORE: c_int = 4;
pub const ACTION_ENTER_FULLSCREEN: c_int = 5;
pub const ACTION_LEAVE_FULLSCREEN: c_int = 6;

pub const CAP_STATE: u32 = 1 << 0;
pub const CAP_STATE_EVENTS: u32 = 1 << 1;
pub const CAP_SIZE_CONSTRAINTS: u32 = 1 << 2;
pub const CAP_SCREENS: u32 = 1 << 3;
pub const CAP_NORMAL_BOUNDS: u32 = 1 << 12;
pub const CAP_KEEP_ALIVE: u32 = 1 << 13;
pub const CAP_SET_POSITION: u32 = 1 << 14;

/// How long geometry must stay unchanged in the normal state before it is
/// taken as the normal bounds (zoom animations report intermediate frames
/// while the window still looks normal). Same as backend-common.
const SETTLE: Duration = Duration::from_millis(300);

type Bounds = (i32, i32, i32, i32);

#[derive(Default)]
struct Record {
  has_state: bool,
  state: u32,
  constraints: [i32; 4],
  committed: Option<Bounds>,
  candidate: Option<(Bounds, Instant)>,
  pending_action: Option<c_int>,
  screen: i64,
}

fn records() -> &'static Mutex<HashMap<u32, Record>> {
  static STORE: OnceLock<Mutex<HashMap<u32, Record>>> = OnceLock::new();
  STORE.get_or_init(|| Mutex::new(HashMap::new()))
}

static STATE_HANDLER: Mutex<Option<(LaufeyWindowStateFn, usize)>> =
  Mutex::new(None);
static SCREENS: Mutex<Vec<LaufeyScreen>> = Mutex::new(Vec::new());
/// Whether the event loop has read the monitors yet (`resumed`, a window
/// created). The runtime starts before the event loop runs, so an early
/// screens() would otherwise find the cache empty.
static SCREENS_READ: (Mutex<bool>, Condvar) =
  (Mutex::new(false), Condvar::new());

/// How long screens() waits for the event loop's first read of the monitors.
const FIRST_SCREENS_WAIT: Duration = Duration::from_secs(5);
static QUIT_ON_LAST_WINDOW: AtomicBool = AtomicBool::new(true);
static QUITTING: AtomicBool = AtomicBool::new(false);

pub fn capabilities() -> u32 {
  let mut caps = CAP_STATE
    | CAP_STATE_EVENTS
    | CAP_SIZE_CONSTRAINTS
    | CAP_SCREENS
    | CAP_NORMAL_BOUNDS
    | CAP_KEEP_ALIVE;
  // winit picks Wayland when a Wayland display exists (and falls back to
  // X11 when none answers); Wayland clients can't place their windows.
  #[cfg(target_os = "linux")]
  let wayland = crate::platform::current_display_backend() == "wayland";
  #[cfg(not(target_os = "linux"))]
  let wayland = false;
  if !wayland {
    caps |= CAP_SET_POSITION;
  }
  caps
}

// --- State ---

pub fn set_state_handler(handler: Option<(LaufeyWindowStateFn, usize)>) {
  *STATE_HANDLER.lock().unwrap() = handler;
}

/// Stores `action` for the event-loop thread to apply.
pub fn queue_action(window_id: u32, action: c_int) {
  records()
    .lock()
    .unwrap()
    .entry(window_id)
    .or_default()
    .pending_action = Some(action);
}

pub fn take_action(window_id: u32) -> Option<c_int> {
  records()
    .lock()
    .unwrap()
    .get_mut(&window_id)
    .and_then(|r| r.pending_action.take())
}

pub fn state_of(window: &Window) -> u32 {
  let mut state = 0;
  if window.fullscreen().is_some() {
    state |= STATE_FULLSCREEN;
  } else if window.is_decorated()
    && window.is_resizable()
    && window.is_maximized()
  {
    // Only asked of a titled, resizable window: on macOS winit answers
    // `is_maximized` for any other window by switching its style mask to
    // titled + resizable and back, which resizes it -- and this runs on
    // every resize, so a frameless window would resize forever and starve
    // the event loop. Such a window has no maximize control anyway.
    state |= STATE_MAXIMIZED;
  }
  if window.is_minimized() == Some(true) {
    state |= STATE_MINIMIZED;
  }
  state
}

/// Event-loop thread.
pub fn apply_action(window: &Window, action: c_int) {
  let fullscreen = window.fullscreen().is_some();
  match action {
    ACTION_MAXIMIZE if !fullscreen => window.set_maximized(true),
    ACTION_UNMAXIMIZE if window.is_maximized() => window.set_maximized(false),
    ACTION_MINIMIZE => window.set_minimized(true),
    ACTION_RESTORE if window.is_minimized() != Some(false) => {
      window.set_minimized(false)
    }
    ACTION_ENTER_FULLSCREEN if !fullscreen => {
      window.set_fullscreen(Some(Fullscreen::Borderless(None)))
    }
    ACTION_LEAVE_FULLSCREEN if fullscreen => window.set_fullscreen(None),
    _ => {}
  }
}

fn logical_bounds(window: &Window) -> Bounds {
  let scale = window.scale_factor();
  let (x, y) = window
    .outer_position()
    .map(|p| {
      let l = p.to_logical::<f64>(scale);
      (l.x.round() as i32, l.y.round() as i32)
    })
    .unwrap_or((0, 0));
  let size = window.inner_size().to_logical::<f64>(scale);
  (x, y, size.width.round() as i32, size.height.round() as i32)
}

/// Event-loop thread: read the window's state and geometry back, track the
/// normal bounds and call the state handler on a change. Call it after every
/// event that can reveal a state change (resize, move, focus, occlusion) and
/// after applying an action.
pub fn report(window_id: u32, window: &Window) {
  let state = state_of(window);
  let bounds = logical_bounds(window);
  report_with(window_id, state, bounds, Instant::now());
}

fn report_with(window_id: u32, state: u32, bounds: Bounds, now: Instant) {
  let previous;
  {
    let mut records = records().lock().unwrap();
    let r = records.entry(window_id).or_default();
    previous = if r.has_state { r.state } else { 0 };
    // Settle a candidate that rested long enough, whatever happens next.
    if let Some((b, t)) = r.candidate {
      if now.duration_since(t) >= SETTLE {
        r.committed = Some(b);
        r.candidate = None;
      }
    }
    if state == 0 {
      if r.committed.is_none() {
        r.committed = Some(bounds);
      }
      // The same geometry again keeps resting since its first report.
      if r.candidate.map(|(b, _)| b) != Some(bounds) {
        r.candidate = Some((bounds, now));
      }
    } else {
      // Leaving the normal state: a young candidate was an animation frame.
      r.candidate = None;
    }
    r.has_state = true;
    r.state = state;
  }
  if previous == state {
    return;
  }
  let handler = *STATE_HANDLER.lock().unwrap();
  if let Some((f, user_data)) = handler {
    unsafe { f(user_data as *mut c_void, window_id, state, previous) };
  }
}

pub fn get_state(window_id: u32) -> u32 {
  records()
    .lock()
    .unwrap()
    .get(&window_id)
    .map(|r| r.state)
    .unwrap_or(0)
}

/// The normal bounds: `current` for a normal window, else the committed ones.
pub fn normal_bounds(window_id: u32, current: Bounds) -> Bounds {
  let mut records = records().lock().unwrap();
  let Some(r) = records.get_mut(&window_id) else {
    return current;
  };
  if r.state == 0 {
    return current;
  }
  r.committed.unwrap_or(current)
}

pub fn forget(window_id: u32) {
  records().lock().unwrap().remove(&window_id);
}

// --- Size constraints ---

/// Normalizes (negative to 0, a max below the min raised to it) and stores.
pub fn set_constraints(window_id: u32, c: [i32; 4]) -> [i32; 4] {
  let [mut min_w, mut min_h, mut max_w, mut max_h] = c;
  min_w = min_w.max(0);
  min_h = min_h.max(0);
  max_w = max_w.max(0);
  max_h = max_h.max(0);
  if max_w > 0 && max_w < min_w {
    max_w = min_w;
  }
  if max_h > 0 && max_h < min_h {
    max_h = min_h;
  }
  let c = [min_w, min_h, max_w, max_h];
  records()
    .lock()
    .unwrap()
    .entry(window_id)
    .or_default()
    .constraints = c;
  c
}

pub fn get_constraints(window_id: u32) -> [i32; 4] {
  records()
    .lock()
    .unwrap()
    .get(&window_id)
    .map(|r| r.constraints)
    .unwrap_or_default()
}

pub fn clamp(c: [i32; 4], width: i32, height: i32) -> (i32, i32) {
  let [min_w, min_h, max_w, max_h] = c;
  let mut w = width;
  let mut h = height;
  if min_w > 0 {
    w = w.max(min_w);
  }
  if max_w > 0 {
    w = w.min(max_w);
  }
  if min_h > 0 {
    h = h.max(min_h);
  }
  if max_h > 0 {
    h = h.min(max_h);
  }
  (w, h)
}

/// Event-loop thread: hands the constraints to winit and brings the window
/// into the range.
pub fn apply_constraints(window: &Window, c: [i32; 4]) {
  let [min_w, min_h, max_w, max_h] = c;
  window.set_min_inner_size(
    (min_w > 0 || min_h > 0).then(|| LogicalSize::new(min_w, min_h)),
  );
  window.set_max_inner_size((max_w > 0 || max_h > 0).then(|| {
    LogicalSize::new(
      if max_w > 0 {
        max_w
      } else {
        i32::from(u16::MAX)
      },
      if max_h > 0 {
        max_h
      } else {
        i32::from(u16::MAX)
      },
    )
  }));
  if state_of(window) != 0 {
    return;
  }
  let (_, _, w, h) = logical_bounds(window);
  let clamped = clamp(c, w, h);
  if clamped != (w, h) {
    let _ = window.request_inner_size(LogicalSize::new(clamped.0, clamped.1));
  }
}

// --- Screens ---

fn monitor_id(name: Option<String>, index: usize) -> i64 {
  // FNV-1a over the monitor name (winit has no stable id), plus the index
  // among same-named monitors; JS-safe like backend-common's ids.
  let key = format!("{}#{index}", name.unwrap_or_default());
  let mut h: u64 = 1469598103934665603;
  for b in key.bytes() {
    h ^= u64::from(b);
    h = h.wrapping_mul(1099511628211);
  }
  let id = (h & 0x1f_ffff_ffff_ffff) as i64;
  if id == 0 {
    1
  } else {
    id
  }
}

fn screen_from(m: &MonitorHandle, id: i64, is_primary: bool) -> LaufeyScreen {
  let scale = m.scale_factor();
  let pos = m.position().to_logical::<f64>(scale);
  let size = m.size().to_logical::<f64>(scale);
  let (x, y) = (pos.x.round() as i32, pos.y.round() as i32);
  let (width, height) = (size.width.round() as i32, size.height.round() as i32);
  LaufeyScreen {
    id,
    x,
    y,
    width,
    height,
    work_x: x,
    work_y: y,
    work_width: width,
    work_height: height,
    scale_factor: scale,
    is_primary,
  }
}

/// Event-loop thread: re-reads the monitors (from the event loop or any
/// window). Cached for the getters, which may run on any thread.
pub fn refresh_screens(
  monitors: impl Iterator<Item = MonitorHandle>,
  primary: Option<MonitorHandle>,
) {
  let mut seen: HashMap<Option<String>, usize> = HashMap::new();
  let mut screens = Vec::new();
  for m in monitors {
    let name = m.name();
    let index = seen.entry(name.clone()).or_insert(0);
    let id = monitor_id(name, *index);
    *index += 1;
    screens.push(screen_from(&m, id, primary.as_ref() == Some(&m)));
  }
  // Some X servers list no monitor through RandR; the primary alone is
  // still a screen to report.
  if screens.is_empty() {
    if let Some(m) = &primary {
      screens.push(screen_from(m, monitor_id(m.name(), 0), true));
    }
  }
  if !screens.iter().any(|s| s.is_primary) {
    if let Some(first) = screens.first_mut() {
      first.is_primary = true;
    }
  }
  screens.sort_by_key(|s| !s.is_primary);
  *SCREENS.lock().unwrap() = screens;
  mark_screens_read();
}

fn mark_screens_read() {
  let (read, cv) = &SCREENS_READ;
  *read.lock().unwrap() = true;
  cv.notify_all();
}

/// Waits (at most `limit`) until the event loop has read the monitors once.
fn wait_for_first_screens(limit: Duration) -> bool {
  let (read, cv) = &SCREENS_READ;
  let guard = read.lock().unwrap();
  let (guard, _) = cv.wait_timeout_while(guard, limit, |read| !*read).unwrap();
  *guard
}

/// Event-loop thread: refreshes the screens through `window` and records
/// which one it is on (the screen at its monitor's position).
pub fn refresh_window_screen(window_id: u32, window: &Window) {
  refresh_screens(window.available_monitors(), window.primary_monitor());
  let Some(current) = window.current_monitor() else {
    return;
  };
  let scale = current.scale_factor();
  let p = current.position().to_logical::<f64>(scale);
  let (x, y) = (p.x.round() as i32, p.y.round() as i32);
  let id = SCREENS
    .lock()
    .unwrap()
    .iter()
    .find(|s| s.x == x && s.y == y)
    .map(|s| s.id);
  if let Some(id) = id {
    records()
      .lock()
      .unwrap()
      .entry(window_id)
      .or_default()
      .screen = id;
  }
}

/// The cached screens. Called from the runtime's thread; until the event
/// loop has read the monitors for the first time (just after it starts),
/// this waits for that read instead of answering with no screens.
pub fn screens() -> Vec<LaufeyScreen> {
  wait_for_first_screens(FIRST_SCREENS_WAIT);
  SCREENS.lock().unwrap().clone()
}

pub fn window_screen(window_id: u32) -> i64 {
  records()
    .lock()
    .unwrap()
    .get(&window_id)
    .map(|r| r.screen)
    .unwrap_or(0)
}

// --- App lifetime ---

pub fn set_quit_on_last_window_closed(quit: bool) {
  QUIT_ON_LAST_WINDOW.store(quit, Ordering::SeqCst);
}

pub fn mark_quitting() {
  QUITTING.store(true, Ordering::SeqCst);
}

static EXIT_REQUESTED: std::sync::atomic::AtomicBool =
  std::sync::atomic::AtomicBool::new(false);
static EXIT_CODE: std::sync::atomic::AtomicI32 =
  std::sync::atomic::AtomicI32::new(0);

/// exit_app (API 46): the app asked to end with `code`. The first request's
/// code wins. Also marks the app quitting.
pub fn mark_exit_requested(code: i32) {
  if EXIT_REQUESTED
    .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
    .is_ok()
  {
    EXIT_CODE.store(code, Ordering::SeqCst);
  }
  mark_quitting();
}

/// The code exit_app asked for, if it was called.
pub fn requested_exit_code() -> Option<i32> {
  EXIT_REQUESTED
    .load(Ordering::SeqCst)
    .then(|| EXIT_CODE.load(Ordering::SeqCst))
}

/// Whether closing the last window should end the event loop.
pub fn should_end_loop_after_last_window() -> bool {
  QUITTING.load(Ordering::SeqCst) || QUIT_ON_LAST_WINDOW.load(Ordering::SeqCst)
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn constraints_normalize_and_clamp() {
    assert_eq!(set_constraints(901, [400, 300, 200, 0]), [400, 300, 400, 0]);
    assert_eq!(get_constraints(901), [400, 300, 400, 0]);
    assert_eq!(clamp([400, 300, 900, 700], 100, 100), (400, 300));
    assert_eq!(clamp([400, 300, 900, 700], 3000, 3000), (900, 700));
    assert_eq!(clamp([0, 0, 0, 0], 5, 6), (5, 6));
    forget(901);
    assert_eq!(get_constraints(901), [0, 0, 0, 0]);
  }

  static SEEN: Mutex<Vec<(u32, u32, u32)>> = Mutex::new(Vec::new());
  unsafe extern "C" fn on_state(
    _: *mut c_void,
    id: u32,
    state: u32,
    previous: u32,
  ) {
    SEEN.lock().unwrap().push((id, state, previous));
  }

  #[test]
  fn report_dedups_and_tracks_normal_bounds() {
    set_state_handler(Some((on_state, 0)));
    let t0 = Instant::now();
    let id = 902;
    report_with(id, 0, (10, 20, 800, 600), t0);
    report_with(id, 0, (50, 60, 800, 600), t0 + Duration::from_secs(1));
    // Zoom animation frames, then maximized.
    report_with(id, 0, (40, 50, 900, 700), t0 + Duration::from_secs(5));
    report_with(
      id,
      STATE_MAXIMIZED,
      (0, 0, 1440, 900),
      t0 + Duration::from_millis(5100),
    );
    report_with(
      id,
      STATE_MAXIMIZED,
      (0, 0, 1440, 900),
      t0 + Duration::from_millis(5200),
    );
    assert_eq!(get_state(id), STATE_MAXIMIZED);
    assert_eq!(normal_bounds(id, (0, 0, 1440, 900)), (50, 60, 800, 600));
    let seen: Vec<_> = SEEN
      .lock()
      .unwrap()
      .iter()
      .copied()
      .filter(|e| e.0 == id)
      .collect();
    assert_eq!(seen, vec![(id, STATE_MAXIMIZED, 0)]);
    report_with(id, 0, (50, 60, 800, 600), t0 + Duration::from_secs(6));
    assert_eq!(normal_bounds(id, (1, 2, 3, 4)), (1, 2, 3, 4));
    forget(id);
    set_state_handler(None);
  }

  #[test]
  fn lifetime_flags() {
    assert!(should_end_loop_after_last_window());
    set_quit_on_last_window_closed(false);
    assert!(!should_end_loop_after_last_window());
    set_quit_on_last_window_closed(true);
  }

  #[test]
  fn screens_waits_for_the_first_read() {
    // Before the event loop has read the monitors a caller waits (bounded)
    // instead of getting an empty list; the read wakes it.
    let waiter = std::thread::spawn(|| {
      let started = Instant::now();
      let read = wait_for_first_screens(Duration::from_secs(10));
      (read, started.elapsed())
    });
    std::thread::sleep(Duration::from_millis(100));
    mark_screens_read();
    let (read, waited) = waiter.join().unwrap();
    assert!(read);
    assert!(waited < Duration::from_secs(5));
    // Once read, it answers at once.
    let started = Instant::now();
    assert!(wait_for_first_screens(Duration::from_secs(10)));
    assert!(started.elapsed() < Duration::from_secs(1));
  }

  #[test]
  fn monitor_ids_are_js_safe_and_distinct() {
    let a = monitor_id(Some("DP-1".into()), 0);
    let b = monitor_id(Some("DP-1".into()), 1);
    assert!(a > 0 && b > 0 && a != b);
    assert!(a < (1i64 << 53));
  }
}
