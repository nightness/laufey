// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Window state, size constraints, screens, title bar / backdrop and app
//! lifetime (API 38). See `docs/window-management.md`.

use std::collections::HashMap;
use std::ffi::{c_int, c_void};
use std::sync::{Arc, Mutex, OnceLock};

use crate::{api, ffi, try_api, LaufeyBackendApi, Window};

// --- Constants (mirror laufey.h) ---

pub const LAUFEY_WINDOW_STATE_MAXIMIZED: u32 = 1 << 0;
pub const LAUFEY_WINDOW_STATE_MINIMIZED: u32 = 1 << 1;
pub const LAUFEY_WINDOW_STATE_FULLSCREEN: u32 = 1 << 2;

pub const LAUFEY_WINDOW_ACTION_MAXIMIZE: c_int = 1;
pub const LAUFEY_WINDOW_ACTION_UNMAXIMIZE: c_int = 2;
pub const LAUFEY_WINDOW_ACTION_MINIMIZE: c_int = 3;
pub const LAUFEY_WINDOW_ACTION_RESTORE: c_int = 4;
pub const LAUFEY_WINDOW_ACTION_ENTER_FULLSCREEN: c_int = 5;
pub const LAUFEY_WINDOW_ACTION_LEAVE_FULLSCREEN: c_int = 6;

pub const LAUFEY_TITLEBAR_DEFAULT: c_int = 0;
pub const LAUFEY_TITLEBAR_HIDDEN: c_int = 1;
pub const LAUFEY_TITLEBAR_HIDDEN_INSET: c_int = 2;

pub const LAUFEY_BACKDROP_NONE: c_int = 0;
pub const LAUFEY_BACKDROP_MICA: c_int = 1;
pub const LAUFEY_BACKDROP_ACRYLIC: c_int = 2;
pub const LAUFEY_BACKDROP_MICA_ALT: c_int = 3;
pub const LAUFEY_BACKDROP_VIBRANCY: c_int = 4;

pub const LAUFEY_WINDOW_CAP_STATE: u32 = 1 << 0;
pub const LAUFEY_WINDOW_CAP_STATE_EVENTS: u32 = 1 << 1;
pub const LAUFEY_WINDOW_CAP_SIZE_CONSTRAINTS: u32 = 1 << 2;
pub const LAUFEY_WINDOW_CAP_SCREENS: u32 = 1 << 3;
pub const LAUFEY_WINDOW_CAP_DISPLAY_EVENTS: u32 = 1 << 4;
pub const LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN: u32 = 1 << 5;
pub const LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN_INSET: u32 = 1 << 6;
pub const LAUFEY_WINDOW_CAP_TRAFFIC_LIGHT_POSITION: u32 = 1 << 7;
pub const LAUFEY_WINDOW_CAP_BACKDROP_MICA: u32 = 1 << 8;
pub const LAUFEY_WINDOW_CAP_BACKDROP_ACRYLIC: u32 = 1 << 9;
pub const LAUFEY_WINDOW_CAP_BACKDROP_MICA_ALT: u32 = 1 << 10;
pub const LAUFEY_WINDOW_CAP_VIBRANCY: u32 = 1 << 11;
pub const LAUFEY_WINDOW_CAP_NORMAL_BOUNDS: u32 = 1 << 12;
pub const LAUFEY_WINDOW_CAP_KEEP_ALIVE: u32 = 1 << 13;
pub const LAUFEY_WINDOW_CAP_SET_POSITION: u32 = 1 << 14;
// API 39: drag and drop and native file dialogs.
pub const LAUFEY_WINDOW_CAP_FILE_DROP: u32 = 1 << 15;
pub const LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS: u32 = 1 << 16;
pub const LAUFEY_WINDOW_CAP_FILE_DRAG_OUT: u32 = 1 << 17;
pub const LAUFEY_WINDOW_CAP_FILE_DIALOGS: u32 = 1 << 18;
pub const LAUFEY_WINDOW_CAP_FILE_DIALOG_FILES_AND_DIRECTORIES: u32 = 1 << 19;
pub const LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL: u32 = 1 << 20;

// --- Types ---

/// Where a window is: maximized, minimized and / or fullscreen. All false is
/// the normal state.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct WindowState {
  pub maximized: bool,
  pub minimized: bool,
  pub fullscreen: bool,
}

impl WindowState {
  /// From `LAUFEY_WINDOW_STATE_*` bits (unknown bits are ignored).
  pub fn from_bits(bits: u32) -> Self {
    Self {
      maximized: bits & LAUFEY_WINDOW_STATE_MAXIMIZED != 0,
      minimized: bits & LAUFEY_WINDOW_STATE_MINIMIZED != 0,
      fullscreen: bits & LAUFEY_WINDOW_STATE_FULLSCREEN != 0,
    }
  }

  pub fn bits(&self) -> u32 {
    let mut bits = 0;
    if self.maximized {
      bits |= LAUFEY_WINDOW_STATE_MAXIMIZED;
    }
    if self.minimized {
      bits |= LAUFEY_WINDOW_STATE_MINIMIZED;
    }
    if self.fullscreen {
      bits |= LAUFEY_WINDOW_STATE_FULLSCREEN;
    }
    bits
  }

  pub fn is_normal(&self) -> bool {
    !self.maximized && !self.minimized && !self.fullscreen
  }
}

/// A change of [`WindowState`], with the state before it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WindowStateEvent {
  pub window_id: u32,
  pub state: WindowState,
  pub previous: WindowState,
}

/// A window's size limits, in [`Window::set_size`] units. 0 on an axis means
/// "no limit" there.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct SizeConstraints {
  pub min_width: i32,
  pub min_height: i32,
  pub max_width: i32,
  pub max_height: i32,
}

/// A rectangle in screen space (the [`Window::get_position`] space).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Rect {
  pub x: i32,
  pub y: i32,
  pub width: i32,
  pub height: i32,
}

/// One display. Every rectangle is in the [`Window::get_position`] space of
/// this backend, so a window position can be compared with it directly.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Screen {
  /// Identifies the display while it stays connected. Within the JS safe
  /// integer range.
  pub id: i64,
  pub bounds: Rect,
  /// The bounds minus the menu bar, dock, taskbar and panels.
  pub work_area: Rect,
  /// Physical pixels per DIP.
  pub scale_factor: f64,
  pub is_primary: bool,
}

impl From<&ffi::laufey_screen> for Screen {
  fn from(s: &ffi::laufey_screen) -> Self {
    Screen {
      id: s.id,
      bounds: Rect {
        x: s.x,
        y: s.y,
        width: s.width,
        height: s.height,
      },
      work_area: Rect {
        x: s.work_x,
        y: s.work_y,
        width: s.work_width,
        height: s.work_height,
      },
      scale_factor: s.scale_factor,
      is_primary: s.is_primary,
    }
  }
}

/// Title bar styles for [`Window::set_titlebar_style`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TitlebarStyle {
  Default,
  /// Transparent title bar, content under it, system buttons overlaid.
  Hidden,
  /// `Hidden` with the macOS traffic lights inset.
  HiddenInset,
}

impl TitlebarStyle {
  fn raw(self) -> c_int {
    match self {
      TitlebarStyle::Default => LAUFEY_TITLEBAR_DEFAULT,
      TitlebarStyle::Hidden => LAUFEY_TITLEBAR_HIDDEN,
      TitlebarStyle::HiddenInset => LAUFEY_TITLEBAR_HIDDEN_INSET,
    }
  }
}

/// macOS vibrancy materials (NSVisualEffectMaterial).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VibrancyMaterial {
  Titlebar = 3,
  Selection = 4,
  Menu = 5,
  Popover = 6,
  Sidebar = 7,
  HeaderView = 10,
  Sheet = 11,
  WindowBackground = 12,
  Hud = 13,
  FullscreenUi = 15,
  Tooltip = 17,
  ContentBackground = 18,
  UnderWindowBackground = 21,
  UnderPageBackground = 22,
}

/// What [`Window::set_backdrop`] puts behind the page.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backdrop {
  None,
  /// Windows 11 Mica.
  Mica,
  /// Windows 11 Acrylic.
  Acrylic,
  /// Windows 11 tabbed Mica.
  MicaAlt,
  /// macOS NSVisualEffectView.
  Vibrancy(VibrancyMaterial),
}

impl Backdrop {
  fn raw(self) -> (c_int, c_int) {
    match self {
      Backdrop::None => (LAUFEY_BACKDROP_NONE, 0),
      Backdrop::Mica => (LAUFEY_BACKDROP_MICA, 0),
      Backdrop::Acrylic => (LAUFEY_BACKDROP_ACRYLIC, 0),
      Backdrop::MicaAlt => (LAUFEY_BACKDROP_MICA_ALT, 0),
      Backdrop::Vibrancy(m) => (LAUFEY_BACKDROP_VIBRANCY, m as c_int),
    }
  }
}

/// What this backend can do on this OS (see `LAUFEY_WINDOW_CAP_*`).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct WindowCapabilities {
  pub bits: u32,
}

impl WindowCapabilities {
  pub fn has(&self, cap: u32) -> bool {
    self.bits & cap == cap
  }
  pub fn state(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_STATE)
  }
  pub fn state_events(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_STATE_EVENTS)
  }
  pub fn size_constraints(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_SIZE_CONSTRAINTS)
  }
  pub fn screens(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_SCREENS)
  }
  pub fn display_events(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_DISPLAY_EVENTS)
  }
  pub fn titlebar_hidden(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN)
  }
  pub fn titlebar_hidden_inset(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_TITLEBAR_HIDDEN_INSET)
  }
  pub fn traffic_light_position(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_TRAFFIC_LIGHT_POSITION)
  }
  pub fn mica(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_BACKDROP_MICA)
  }
  pub fn acrylic(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_BACKDROP_ACRYLIC)
  }
  pub fn mica_alt(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_BACKDROP_MICA_ALT)
  }
  pub fn vibrancy(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_VIBRANCY)
  }
  pub fn normal_bounds(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_NORMAL_BOUNDS)
  }
  pub fn keep_alive(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_KEEP_ALIVE)
  }
  pub fn set_position(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_SET_POSITION)
  }
  /// The file-drop handler fires (API 39).
  pub fn file_drop(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_FILE_DROP)
  }
  /// ENTER / OVER already carry the paths (not only the count).
  pub fn file_drop_enter_paths(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_FILE_DROP_ENTER_PATHS)
  }
  /// `start_file_drag` works.
  pub fn file_drag_out(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_FILE_DRAG_OUT)
  }
  /// `show_file_dialog` works.
  pub fn file_dialogs(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_FILE_DIALOGS)
  }
  /// One open dialog can pick files and directories (macOS).
  pub fn file_dialog_files_and_directories(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_FILE_DIALOG_FILES_AND_DIRECTORIES)
  }
  /// A dialog given a window is modal to it.
  pub fn file_dialog_modal(&self) -> bool {
    self.has(LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL)
  }
}

// --- Free functions ---

/// What this backend can do on this OS (none on backends older than API 38).
/// Any thread.
pub fn window_capabilities() -> WindowCapabilities {
  window_capabilities_with(api())
}

fn window_capabilities_with(api: &LaufeyBackendApi) -> WindowCapabilities {
  WindowCapabilities {
    bits: match api.window_capabilities {
      Some(f) => unsafe { f(api.backend_data) },
      None => 0,
    },
  }
}

/// The connected displays, primary first (empty when the backend can't tell).
/// Any thread.
pub fn screens() -> Vec<Screen> {
  screens_with(api())
}

fn screens_with(api: &LaufeyBackendApi) -> Vec<Screen> {
  let Some(f) = api.get_screens else {
    return Vec::new();
  };
  // The count can change between the two calls (a display plugged in);
  // retry with the new size a few times.
  let mut buf: Vec<ffi::laufey_screen> = Vec::new();
  for _ in 0..4 {
    let cap = buf.len();
    let n = unsafe { f(api.backend_data, buf.as_mut_ptr(), cap) };
    if n <= cap {
      buf.truncate(n);
      return buf.iter().map(Screen::from).collect();
    }
    buf = vec![unsafe { std::mem::zeroed() }; n];
  }
  buf.iter().map(Screen::from).collect()
}

/// Whether the event loop ends when the last window closes (the default).
/// A tray / menu-bar app passes `false` and ends through [`crate::quit`].
pub fn set_quit_on_last_window_closed(quit: bool) {
  let api = api();
  if let Some(f) = api.set_quit_on_last_window_closed {
    unsafe { f(api.backend_data, quit) };
  }
}

type DisplayHandler = Arc<dyn Fn() + Send + Sync>;

fn display_handler() -> &'static Mutex<Option<DisplayHandler>> {
  static STORE: OnceLock<Mutex<Option<DisplayHandler>>> = OnceLock::new();
  STORE.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn display_changed_trampoline(_user_data: *mut c_void) {
  let handler = display_handler().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler();
  }
}

/// Register the (single, process-wide) handler for display changes: a display
/// added or removed, or a change of arrangement, work area or scale. Call
/// [`screens`] in it for the new layout. Fires on the backend UI thread.
/// Replaces any previous handler.
pub fn on_display_changed<F>(handler: F)
where
  F: Fn() + Send + Sync + 'static,
{
  *display_handler().lock().unwrap() = Some(Arc::new(handler));
  let api = api();
  if let Some(f) = api.set_display_changed_handler {
    unsafe {
      f(
        api.backend_data,
        Some(display_changed_trampoline),
        std::ptr::null_mut(),
      )
    };
  }
}

// --- Window-state events ---

type StateHandler = Arc<dyn Fn(WindowStateEvent) + Send + Sync>;

fn state_handlers() -> &'static Mutex<HashMap<u32, StateHandler>> {
  static STORE: OnceLock<Mutex<HashMap<u32, StateHandler>>> = OnceLock::new();
  STORE.get_or_init(|| Mutex::new(HashMap::new()))
}

unsafe extern "C" fn window_state_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  state: u32,
  previous: u32,
) {
  // Clone out and release the lock before calling (the handler may call
  // back into the backend).
  let handler = state_handlers().lock().unwrap().get(&window_id).cloned();
  if let Some(handler) = handler {
    handler(WindowStateEvent {
      window_id,
      state: WindowState::from_bits(state),
      previous: WindowState::from_bits(previous),
    });
  }
}

fn ensure_state_handler() {
  static ONCE: OnceLock<()> = OnceLock::new();
  ONCE.get_or_init(|| {
    if let Some(api) = try_api() {
      if let Some(f) = api.set_window_state_handler {
        unsafe {
          f(
            api.backend_data,
            Some(window_state_trampoline),
            std::ptr::null_mut(),
          )
        };
      }
    }
  });
}

/// Register a handler for `window_id`'s state changes (maximize, unmaximize,
/// minimize, restore, enter / leave fullscreen). Fires on the backend UI
/// thread after the OS applied the change.
pub fn on_window_state_change<F>(window_id: u32, handler: F)
where
  F: Fn(WindowStateEvent) + Send + Sync + 'static,
{
  ensure_state_handler();
  state_handlers()
    .lock()
    .unwrap()
    .insert(window_id, Arc::new(handler));
}

// --- Window methods ---

impl Window {
  fn apply_state_action(&self, action: c_int) {
    let api = api();
    if let Some(f) = api.set_window_state {
      unsafe { f(api.backend_data, self.id(), action) };
    }
  }

  /// Maximize the window (zoom on macOS). Asynchronous where the OS animates
  /// it; [`Window::on_state_change`] reports when it took effect.
  pub fn maximize(&self) {
    self.apply_state_action(LAUFEY_WINDOW_ACTION_MAXIMIZE);
  }

  pub fn unmaximize(&self) {
    self.apply_state_action(LAUFEY_WINDOW_ACTION_UNMAXIMIZE);
  }

  pub fn minimize(&self) {
    self.apply_state_action(LAUFEY_WINDOW_ACTION_MINIMIZE);
  }

  /// Un-minimize: back to the state the window had before it was minimized.
  pub fn restore(&self) {
    self.apply_state_action(LAUFEY_WINDOW_ACTION_RESTORE);
  }

  pub fn set_fullscreen(&self, fullscreen: bool) {
    self.apply_state_action(if fullscreen {
      LAUFEY_WINDOW_ACTION_ENTER_FULLSCREEN
    } else {
      LAUFEY_WINDOW_ACTION_LEAVE_FULLSCREEN
    });
  }

  /// The window's state as the OS reports it now (normal on backends older
  /// than API 38).
  pub fn get_state(&self) -> WindowState {
    let api = api();
    match api.get_window_state {
      Some(f) => {
        WindowState::from_bits(unsafe { f(api.backend_data, self.id()) })
      }
      None => WindowState::default(),
    }
  }

  pub fn is_maximized(&self) -> bool {
    self.get_state().maximized
  }

  pub fn is_minimized(&self) -> bool {
    self.get_state().minimized
  }

  pub fn is_fullscreen(&self) -> bool {
    self.get_state().fullscreen
  }

  /// See [`on_window_state_change`].
  pub fn on_state_change<F>(self, handler: F) -> Self
  where
    F: Fn(WindowStateEvent) + Send + Sync + 'static,
  {
    on_window_state_change(self.id(), handler);
    self
  }

  /// Set the size limits (in [`Window::set_size`] units; 0 = none on that
  /// axis). The OS enforces them while the user resizes, `set_size` clamps
  /// to them, and a window outside the new range is resized into it.
  pub fn set_size_constraints(&self, c: SizeConstraints) {
    let api = api();
    if let Some(f) = api.set_window_size_constraints {
      unsafe {
        f(
          api.backend_data,
          self.id(),
          c.min_width,
          c.min_height,
          c.max_width,
          c.max_height,
        )
      };
    }
  }

  pub fn get_size_constraints(&self) -> SizeConstraints {
    let api = api();
    let mut c = SizeConstraints::default();
    if let Some(f) = api.get_window_size_constraints {
      unsafe {
        f(
          api.backend_data,
          self.id(),
          &mut c.min_width,
          &mut c.min_height,
          &mut c.max_width,
          &mut c.max_height,
        )
      };
    }
    c
  }

  /// Set the minimum size, keeping the maximum.
  pub fn set_min_size(&self, width: i32, height: i32) {
    let mut c = self.get_size_constraints();
    c.min_width = width;
    c.min_height = height;
    self.set_size_constraints(c);
  }

  /// Set the maximum size, keeping the minimum.
  pub fn set_max_size(&self, width: i32, height: i32) {
    let mut c = self.get_size_constraints();
    c.max_width = width;
    c.max_height = height;
    self.set_size_constraints(c);
  }

  pub fn get_min_size(&self) -> (i32, i32) {
    let c = self.get_size_constraints();
    (c.min_width, c.min_height)
  }

  pub fn get_max_size(&self) -> (i32, i32) {
    let c = self.get_size_constraints();
    (c.max_width, c.max_height)
  }

  /// The id of the [`Screen`] the window is on, or `None` when unknown.
  pub fn get_screen_id(&self) -> Option<i64> {
    let api = api();
    let id = match api.get_window_screen {
      Some(f) => unsafe { f(api.backend_data, self.id()) },
      None => 0,
    };
    (id != 0).then_some(id)
  }

  /// The [`Screen`] the window is on.
  pub fn get_screen(&self) -> Option<Screen> {
    let id = self.get_screen_id()?;
    screens().into_iter().find(|s| s.id == id)
  }

  /// Change the title bar style. Returns `false` (nothing changes) where the
  /// backend can't (everything but macOS).
  pub fn set_titlebar_style(&self, style: TitlebarStyle) -> bool {
    let api = api();
    match api.set_window_titlebar_style {
      Some(f) => unsafe { f(api.backend_data, self.id(), style.raw()) },
      None => false,
    }
  }

  /// Move the macOS traffic lights to `(x, y)` from the window's top-left
  /// (`None` puts them back). Returns `false` where unsupported.
  pub fn set_traffic_light_position(
    &self,
    position: Option<(i32, i32)>,
  ) -> bool {
    let api = api();
    let (x, y) = position.unwrap_or((-1, -1));
    match api.set_window_traffic_light_position {
      Some(f) => unsafe { f(api.backend_data, self.id(), x, y) },
      None => false,
    }
  }

  /// Put a [`Backdrop`] behind the page (it shows where the page's
  /// background is transparent). Returns `false` (nothing changes) where
  /// this backend / OS can't show it; see [`window_capabilities`].
  pub fn set_backdrop(&self, backdrop: Backdrop) -> bool {
    let api = api();
    let (kind, material) = backdrop.raw();
    match api.set_window_backdrop {
      Some(f) => unsafe { f(api.backend_data, self.id(), kind, material) },
      None => false,
    }
  }

  /// The bounds the window returns to when it leaves the maximized,
  /// minimized or fullscreen state (the current bounds for a normal window):
  /// position as [`Window::get_position`], size as [`Window::get_size`].
  /// What an app persists to reopen the window where the user left it.
  pub fn get_normal_bounds(&self) -> Option<Rect> {
    let api = api();
    let f = api.get_window_normal_bounds?;
    let mut r = Rect::default();
    let ok = unsafe {
      f(
        api.backend_data,
        self.id(),
        &mut r.x,
        &mut r.y,
        &mut r.width,
        &mut r.height,
      )
    };
    ok.then_some(r)
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn window_state_bits_round_trip() {
    for bits in 0..8u32 {
      assert_eq!(WindowState::from_bits(bits).bits(), bits);
    }
    let s = WindowState::from_bits(
      LAUFEY_WINDOW_STATE_MAXIMIZED | LAUFEY_WINDOW_STATE_MINIMIZED | 0x100,
    );
    assert!(s.maximized && s.minimized && !s.fullscreen);
    assert!(!s.is_normal());
    assert!(WindowState::default().is_normal());
  }

  #[test]
  fn backdrop_and_titlebar_encode_like_the_header() {
    assert_eq!(Backdrop::None.raw(), (0, 0));
    assert_eq!(Backdrop::Mica.raw(), (1, 0));
    assert_eq!(Backdrop::Acrylic.raw(), (2, 0));
    assert_eq!(Backdrop::MicaAlt.raw(), (3, 0));
    assert_eq!(
      Backdrop::Vibrancy(VibrancyMaterial::Sidebar).raw(),
      (4, ffi::LAUFEY_VIBRANCY_SIDEBAR as c_int)
    );
    assert_eq!(
      VibrancyMaterial::UnderPageBackground as c_int,
      ffi::LAUFEY_VIBRANCY_UNDER_PAGE_BACKGROUND as c_int
    );
    assert_eq!(TitlebarStyle::HiddenInset.raw(), 2);
    assert_eq!(
      LAUFEY_WINDOW_CAP_SET_POSITION,
      ffi::LAUFEY_WINDOW_CAP_SET_POSITION
    );
    assert_eq!(
      LAUFEY_WINDOW_ACTION_LEAVE_FULLSCREEN,
      ffi::LAUFEY_WINDOW_ACTION_LEAVE_FULLSCREEN as c_int
    );
  }

  unsafe extern "C" fn fake_caps(_: *mut c_void) -> u32 {
    LAUFEY_WINDOW_CAP_STATE | LAUFEY_WINDOW_CAP_VIBRANCY
  }

  #[test]
  fn capabilities_follow_the_bits() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(window_capabilities_with(&fake).bits, 0);
    fake.window_capabilities = Some(fake_caps);
    let caps = window_capabilities_with(&fake);
    assert!(caps.state() && caps.vibrancy());
    assert!(!caps.mica() && !caps.screens() && !caps.keep_alive());
  }

  static SCREEN_COUNT: std::sync::atomic::AtomicUsize =
    std::sync::atomic::AtomicUsize::new(2);

  unsafe extern "C" fn fake_screens(
    _: *mut c_void,
    out: *mut ffi::laufey_screen,
    cap: usize,
  ) -> usize {
    let n = SCREEN_COUNT.load(std::sync::atomic::Ordering::SeqCst);
    for i in 0..n.min(cap) {
      let s = &mut *out.add(i);
      s.id = 100 + i as i64;
      s.x = i as i32 * 1920;
      s.width = 1920;
      s.height = 1080;
      s.work_y = 25;
      s.work_width = 1920;
      s.work_height = 1055;
      s.scale_factor = 2.0;
      s.is_primary = i == 0;
    }
    n
  }

  #[test]
  fn screens_are_read_with_a_growing_buffer() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert!(screens_with(&fake).is_empty());
    fake.get_screens = Some(fake_screens);
    let screens = screens_with(&fake);
    assert_eq!(screens.len(), 2);
    assert_eq!(screens[0].id, 100);
    assert!(screens[0].is_primary && !screens[1].is_primary);
    assert_eq!(
      screens[1].bounds,
      Rect {
        x: 1920,
        y: 0,
        width: 1920,
        height: 1080
      }
    );
    assert_eq!(screens[0].work_area.y, 25);
    assert_eq!(screens[0].work_area.height, 1055);
    assert_eq!(screens[0].scale_factor, 2.0);
  }

  #[test]
  fn state_trampoline_routes_per_window() {
    let seen: Arc<Mutex<Vec<WindowStateEvent>>> = Arc::default();
    let sink = seen.clone();
    state_handlers()
      .lock()
      .unwrap()
      .insert(4242, Arc::new(move |ev| sink.lock().unwrap().push(ev)));
    unsafe {
      window_state_trampoline(
        std::ptr::null_mut(),
        4242,
        LAUFEY_WINDOW_STATE_FULLSCREEN,
        0,
      );
      // Another window without a handler: ignored.
      window_state_trampoline(std::ptr::null_mut(), 4243, 1, 0);
    }
    let seen = seen.lock().unwrap();
    assert_eq!(seen.len(), 1);
    assert_eq!(seen[0].window_id, 4242);
    assert!(seen[0].state.fullscreen);
    assert!(seen[0].previous.is_normal());
  }
}
