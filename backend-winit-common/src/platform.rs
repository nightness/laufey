// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Platform features (API 45) for the winit / servo backends: the same JSON
//! object the CEF and WebView backends build in backend-common
//! (`platform_features.cc`; docs/platform-features.md), probed in Rust. On
//! Linux it reads the session bus (zbus) and, for the XEmbed tray, the X
//! server (x11rb); nothing branches on `XDG_CURRENT_DESKTOP`, which is only
//! a hint for a reason's wording. Winit has no web engine, so
//! `cookieEncryption` and `cookieEncryptionWait` are always `null`, and
//! `kwallet` (which only Chromium's cookie store would use) too. Its badge is
//! the Dock tile's on macOS and a window-title prefix elsewhere: it never
//! sends a Linux launcher badge, so `badge` is never `"launcher-entry"`.
//!
//! The tray host is read live on every call (the winit event loop doesn't
//! iterate GLib, and the GTK thread's GLib loop, which serves the tray, isn't
//! used to follow NameOwnerChanged), so a watcher that starts late counts on
//! the next call.

use std::collections::BTreeMap;

/// What the probe found (the fields of the JSON object).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PlatformFeatures {
  pub os: &'static str,
  pub session_type: Option<String>,
  pub desktop_hint: Option<String>,
  pub session_bus: bool,
  pub tray_watcher: bool,
  pub tray_xembed: bool,
  /// The appindicator library the tray is drawn with loads (Linux).
  pub tray_library: bool,
  pub secret_service: &'static str,
  pub secret_prompter: bool,
  /// The server that owns org.freedesktop.Notifications now (its
  /// GetServerInformation name, "unknown" when it doesn't say).
  pub notification_server: Option<String>,
  /// Nothing owns it, but D-Bus could start one.
  pub notification_activatable: bool,
  pub portal_versions: BTreeMap<String, u32>,
}

/// Why the Winit backend's badge is a title prefix on Linux.
const BADGE_TITLE_REASON: &str =
  "the Winit backend shows the badge as a window-title prefix (it sends no \
   launcher badge)";

impl PlatformFeatures {
  /// Whether a tray icon can be seen here.
  pub fn tray_host(&self) -> bool {
    self.os != "linux"
      || (self.tray_library && (self.tray_watcher || self.tray_xembed))
  }

  /// Why not, when it can't.
  pub fn tray_reason(&self) -> Option<String> {
    if self.tray_host() {
      return None;
    }
    if !self.tray_library {
      return Some(
        "no tray library: install libayatana-appindicator3 (or \
         libappindicator3)"
          .into(),
      );
    }
    // Neutral wording: no desktop is named outside the hint's suffix, which
    // is the reason's last part (a runtime that may not show the hint cuts
    // the reason there), as backend-common words it.
    let mut reason = String::from("no tray host (StatusNotifierWatcher)");
    if self.session_type.as_deref() == Some("x11") {
      reason.push_str(" and no XEmbed system tray");
    }
    reason.push_str(" on this session; some desktops need an extension");
    if let Some(hint) = &self.desktop_hint {
      let gnome = hint.split(':').any(|d| d == "GNOME");
      reason.push_str(&format!(
        " (XDG_CURRENT_DESKTOP={hint}{})",
        if gnome {
          "; GNOME shows tray icons only with the AppIndicator extension \
           enabled"
        } else {
          ""
        }
      ));
    }
    Some(reason)
  }

  /// Why notifications may not show (Linux, no server running).
  pub fn notification_reason(&self) -> Option<String> {
    if self.os != "linux" || self.notification_server.is_some() {
      return None;
    }
    if !self.session_bus {
      return Some("no D-Bus session bus".into());
    }
    if self.notification_activatable {
      return Some(
        "no notification server is running; D-Bus can start one for \
         org.freedesktop.Notifications (it is tried when notifications are \
         first used)"
          .into(),
      );
    }
    let mut reason = String::from(
      "no notification server: nothing owns org.freedesktop.Notifications on \
       the session bus",
    );
    if let Some(hint) = &self.desktop_hint {
      reason.push_str(&format!(" (XDG_CURRENT_DESKTOP={hint})"));
    }
    Some(reason)
  }

  /// The JSON object `platform_features` hands out.
  pub fn to_json(&self) -> String {
    fn quote(s: &str) -> String {
      let mut out = String::from("\"");
      for c in s.chars() {
        match c {
          '"' => out.push_str("\\\""),
          '\\' => out.push_str("\\\\"),
          '\n' => out.push_str("\\n"),
          '\r' => out.push_str("\\r"),
          '\t' => out.push_str("\\t"),
          c if (c as u32) < 0x20 => {
            out.push_str(&format!("\\u{:04x}", c as u32))
          }
          c => out.push(c),
        }
      }
      out.push('"');
      out
    }
    fn opt(s: &Option<String>) -> String {
      s.as_deref().map(quote).unwrap_or_else(|| "null".into())
    }
    let portals = self
      .portal_versions
      .iter()
      .map(|(k, v)| format!("{}:{v}", quote(k)))
      .collect::<Vec<_>>()
      .join(",");
    // tray-icon on Linux (libappindicator) reports no clicks; elsewhere it
    // does. Its tooltip is the indicator's title on Linux.
    let clicks = self.os != "linux";
    // dock.rs: the Dock tile on macOS, a "(N) " title prefix elsewhere.
    let badge = if self.os == "macos" { "dock" } else { "title" };
    let badge_reason = if self.os == "linux" {
      quote(BADGE_TITLE_REASON)
    } else {
      "null".into()
    };
    format!(
      "{{\"os\":{},\"sessionType\":{},\"desktopHint\":{},\"sessionBus\":{},\
       \"trayHost\":{},\"trayReason\":{},\"trayClicks\":{clicks},\
       \"trayTooltip\":true,\"secretService\":{},\"secretServicePrompt\":{},\
       \"kwallet\":null,\"notificationServer\":{},\"notificationReason\":{},\
       \"portalVersions\":{{{portals}}},\"cookieEncryption\":null,\
       \"cookieEncryptionWait\":null,\"badge\":{},\"badgeReason\":{}}}",
      quote(self.os),
      opt(&self.session_type),
      opt(&self.desktop_hint),
      self.session_bus,
      self.tray_host(),
      opt(&self.tray_reason()),
      quote(self.secret_service),
      self.secret_prompter,
      opt(&self.notification_server),
      opt(&self.notification_reason()),
      quote(badge),
      badge_reason,
    )
  }
}

/// The display server this process's windows go to, from the display that
/// is actually there (backend-common's `DisplayBackend`): `"wayland"` when
/// `$WAYLAND_DISPLAY` names a socket that exists (absolute, else under
/// `$XDG_RUNTIME_DIR`) or `$WAYLAND_SOCKET` is set; else `"x11"` when
/// `$DISPLAY` is set; else `""`. `$XDG_SESSION_TYPE` is only a hint and is
/// not read: GDM's autologin into an Xorg session (XFCE, i3) leaves it
/// `"wayland"` with only `$DISPLAY` set.
#[cfg(any(target_os = "linux", test))]
pub(crate) fn display_backend(
  env: impl Fn(&str) -> Option<String>,
  is_socket: impl Fn(&std::path::Path) -> bool,
) -> &'static str {
  let env = |name: &str| env(name).filter(|v| !v.is_empty());
  if env("WAYLAND_SOCKET").is_some() {
    return "wayland";
  }
  if let Some(name) = env("WAYLAND_DISPLAY") {
    let path = if name.starts_with('/') {
      Some(std::path::PathBuf::from(&name))
    } else {
      env("XDG_RUNTIME_DIR").map(|dir| std::path::Path::new(&dir).join(&name))
    };
    if path.is_some_and(|p| is_socket(&p)) {
      return "wayland";
    }
  }
  if env("DISPLAY").is_some() {
    return "x11";
  }
  ""
}

/// [`display_backend`] from this process's environment and file system.
#[cfg(target_os = "linux")]
pub(crate) fn current_display_backend() -> &'static str {
  use std::os::unix::fs::FileTypeExt;
  display_backend(
    |name| std::env::var(name).ok(),
    |path| {
      std::fs::metadata(path)
        .map(|m| m.file_type().is_socket())
        .unwrap_or(false)
    },
  )
}

/// The session type: XDG_SESSION_TYPE as set, `"unknown"` when unset; a
/// graphical one (`x11` / `wayland`) names the display that is there
/// (`display_backend`, when there is one). Never made graphical by
/// `$DISPLAY` / `$WAYLAND_DISPLAY` alone: a display (Xvfb, `xvfb-run` under
/// cron or a systemd service, a forwarded X connection) says nothing about
/// who is in front of it.
#[cfg(any(target_os = "linux", test))]
fn session_type(
  xdg_session_type: Option<&str>,
  display_backend: &str,
) -> String {
  match xdg_session_type.filter(|t| !t.is_empty()) {
    None => "unknown".to_string(),
    Some("x11" | "wayland") if !display_backend.is_empty() => {
      display_backend.to_string()
    }
    Some(t) => t.to_string(),
  }
}

/// Whether someone could answer a prompt (a keyring unlock) here: the
/// session type is x11 or wayland, a display variable is set, and logind,
/// where it can say (`logind`: `Some((type, active))` for this process's
/// session), agrees it is an active x11 / wayland session.
#[cfg(any(target_os = "linux", test))]
fn graphical_session(
  session_type: &str,
  have_display: bool,
  logind: Option<(String, bool)>,
) -> bool {
  if !matches!(session_type, "x11" | "wayland") || !have_display {
    return false;
  }
  match logind {
    Some((t, active)) => active && matches!(t.as_str(), "x11" | "wayland"),
    None => true,
  }
}

/// Whether the XEmbed tray is looked for: X11 sessions with a display only.
#[cfg(any(target_os = "linux", test))]
fn probes_xembed(session_type: &str, have_display: bool) -> bool {
  session_type == "x11" && have_display
}

/// Whether the appindicator library tray-icon draws with on Linux loads
/// (libayatana-appindicator3, else libappindicator3). Loaded once.
#[cfg(target_os = "linux")]
fn tray_library() -> bool {
  static LOADED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
  *LOADED.get_or_init(|| {
    [
      "libayatana-appindicator3.so.1",
      "libappindicator3.so.1",
      "libayatana-appindicator3.so",
      "libappindicator3.so",
    ]
    .iter()
    // SAFETY: loading a shared library runs its constructors; these are
    // the libraries tray-icon itself loads. Kept loaded for the process
    // (never dlclose'd: tray-icon loads it again, and a library with GLib
    // types registered can't be unloaded safely).
    .any(|name| match unsafe { libloading::Library::new(name) } {
      Ok(library) => {
        std::mem::forget(library);
        true
      }
      Err(_) => false,
    })
  })
}

/// Probe this session.
pub fn probe() -> PlatformFeatures {
  #[cfg(target_os = "linux")]
  {
    linux::probe()
  }
  #[cfg(not(target_os = "linux"))]
  {
    PlatformFeatures {
      os: if cfg!(target_os = "macos") {
        "macos"
      } else if cfg!(target_os = "windows") {
        "windows"
      } else {
        "unknown"
      },
      secret_service: "os",
      secret_prompter: true,
      ..Default::default()
    }
  }
}

/// Whether a tray icon can be seen here, and why not. Cheaper than
/// [`probe`]: no Secret Service or portal calls.
pub fn tray_unavailable_reason() -> Option<String> {
  #[cfg(target_os = "linux")]
  {
    // The tray is drawn with GTK on the GTK thread (tray.rs): without GTK
    // there, an icon would be accepted and never shown.
    linux::probe_tray().tray_reason().or_else(|| {
      (!crate::prompt::gtk_thread::usable()).then(|| {
        "GTK could not start on the tray's thread (no display it can open)"
          .to_string()
      })
    })
  }
  #[cfg(not(target_os = "linux"))]
  {
    None
  }
}

/// Whether a notification sent now would reach a server (Linux: one owns
/// org.freedesktop.Notifications, or D-Bus starts one; tried once). Always
/// true elsewhere.
pub fn notification_server_usable() -> bool {
  #[cfg(target_os = "linux")]
  {
    linux::notification_server_usable()
  }
  #[cfg(not(target_os = "linux"))]
  {
    true
  }
}

/// A C handler and its user data.
type ChangedHandler = (unsafe extern "C" fn(*mut std::ffi::c_void), usize);

fn changed_handler() -> &'static std::sync::Mutex<Option<ChangedHandler>> {
  static HANDLER: std::sync::Mutex<Option<ChangedHandler>> =
    std::sync::Mutex::new(None);
  &HANDLER
}

/// Call the platform-features change handler, if one is set.
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
fn fire_changed() {
  let handler = *changed_handler().lock().unwrap();
  if let Some((f, user_data)) = handler {
    // SAFETY: the runtime registered `f` with this user data.
    unsafe { f(user_data as *mut std::ffi::c_void) };
  }
}

/// `set_platform_features_changed_handler` (API 45). On Linux a watcher
/// thread (started on the first handler) follows the
/// StatusNotifierWatcher's owner and fires the handler, on that thread,
/// when a tray host appears or goes away. Never fires elsewhere.
pub fn set_changed_handler(
  handler: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
  user_data: *mut std::ffi::c_void,
) {
  *changed_handler().lock().unwrap() = handler.map(|f| (f, user_data as usize));
  #[cfg(target_os = "linux")]
  if handler.is_some() {
    static STARTED: std::sync::Once = std::sync::Once::new();
    STARTED.call_once(linux::follow_tray_host);
  }
}

/// Say once on stderr why a tray icon was refused.
pub fn log_tray_refused(reason: &str) {
  static ONCE: std::sync::Once = std::sync::Once::new();
  ONCE.call_once(|| eprintln!("laufey: tray icon refused: {reason}"));
}

#[cfg(target_os = "linux")]
mod linux {
  use std::collections::{BTreeMap, HashSet};
  use std::time::Duration;

  use zbus::blocking::Connection;

  use super::PlatformFeatures;

  const TIMEOUT: Duration = Duration::from_millis(1000);
  /// The first portal call may start xdg-desktop-portal.
  const PORTAL_START_TIMEOUT: Duration = Duration::from_millis(3000);
  const WATCHER: &str = "org.kde.StatusNotifierWatcher";
  const SECRETS: &str = "org.freedesktop.secrets";
  const PORTAL: &str = "org.freedesktop.portal.Desktop";
  const PORTAL_INTERFACES: [&str; 4] =
    ["Notification", "FileChooser", "GlobalShortcuts", "Settings"];

  fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
  }

  fn has_session_bus_address() -> bool {
    if env("DBUS_SESSION_BUS_ADDRESS").is_some() {
      return true;
    }
    use std::os::unix::fs::FileTypeExt;
    env("XDG_RUNTIME_DIR")
      .and_then(|dir| std::fs::metadata(format!("{dir}/bus")).ok())
      .is_some_and(|m| m.file_type().is_socket())
  }

  fn connect(timeout: Duration) -> Option<Connection> {
    if !has_session_bus_address() {
      return None;
    }
    zbus::blocking::connection::Builder::session()
      .ok()?
      .method_timeout(timeout)
      .build()
      .ok()
  }

  fn names(conn: &Connection, method: &str) -> HashSet<String> {
    conn
      .call_method(
        Some("org.freedesktop.DBus"),
        "/org/freedesktop/DBus",
        Some("org.freedesktop.DBus"),
        method,
        &(),
      )
      .ok()
      .and_then(|m| m.body().deserialize::<Vec<String>>().ok())
      .map(|v| v.into_iter().collect())
      .unwrap_or_default()
  }

  fn has_owner(conn: &Connection, name: &str) -> bool {
    conn
      .call_method(
        Some("org.freedesktop.DBus"),
        "/org/freedesktop/DBus",
        Some("org.freedesktop.DBus"),
        "NameHasOwner",
        &(name,),
      )
      .ok()
      .and_then(|m| m.body().deserialize::<bool>().ok())
      .unwrap_or(false)
  }

  /// A method call with NO_AUTO_START: the bus never starts the destination
  /// for it (an absent one is an error, `None`).
  fn call_no_start<B, R>(
    conn: &Connection,
    dest: &str,
    path: &str,
    iface: &str,
    method: &str,
    body: &B,
  ) -> Option<R>
  where
    B: zbus::export::serde::ser::Serialize + zbus::zvariant::DynamicType,
    R: for<'d> zbus::zvariant::DynamicDeserialize<'d>,
  {
    let proxy =
      zbus::blocking::proxy::Builder::<zbus::blocking::Proxy>::new(conn)
        .destination(dest)
        .ok()?
        .path(path)
        .ok()?
        .interface(iface)
        .ok()?
        .cache_properties(zbus::proxy::CacheProperties::No)
        .build()
        .ok()?;
    proxy
      .call_with_flags(
        method,
        zbus::proxy::MethodFlags::NoAutoStart.into(),
        body,
      )
      .ok()?
  }

  /// A property, read only from a service that is running (the callers
  /// check), so the call never starts one.
  fn get_property<T>(
    conn: &Connection,
    dest: &str,
    path: &str,
    iface: &str,
    prop: &str,
  ) -> Option<T>
  where
    T: TryFrom<zbus::zvariant::OwnedValue>,
  {
    let value: zbus::zvariant::OwnedValue = conn
      .call_method(
        Some(dest),
        path,
        Some("org.freedesktop.DBus.Properties"),
        "Get",
        &(iface, prop),
      )
      .ok()?
      .body()
      .deserialize()
      .ok()?;
    T::try_from(value).ok()
  }

  fn session_type() -> String {
    super::session_type(
      env("XDG_SESSION_TYPE").as_deref(),
      super::current_display_backend(),
    )
  }

  /// This process's logind session (XDG_SESSION_ID's, else the process's
  /// own) as `(Type, Active)`, or `None` when logind can't say (no system
  /// bus, no logind, no session: a systemd user service, a container).
  /// Property reads on one system-bus connection for the probe, every call
  /// NO_AUTO_START: never starts logind.
  fn logind_session() -> Option<(String, bool)> {
    let conn = zbus::blocking::connection::Builder::system()
      .ok()?
      .method_timeout(TIMEOUT)
      .build()
      .ok()?;
    const LOGIND: &str = "org.freedesktop.login1";
    let manager = "/org/freedesktop/login1";
    let iface = "org.freedesktop.login1.Manager";
    let path: zbus::zvariant::OwnedObjectPath = match env("XDG_SESSION_ID") {
      Some(id) => {
        call_no_start(&conn, LOGIND, manager, iface, "GetSession", &(id,))
      }
      None => call_no_start(
        &conn,
        LOGIND,
        manager,
        iface,
        "GetSessionByPID",
        &(std::process::id(),),
      ),
    }?;
    let get = |prop: &str| -> Option<zbus::zvariant::OwnedValue> {
      call_no_start(
        &conn,
        LOGIND,
        path.as_str(),
        "org.freedesktop.DBus.Properties",
        "Get",
        &("org.freedesktop.login1.Session", prop),
      )
    };
    let session_type = String::try_from(get("Type")?).ok()?;
    let active = bool::try_from(get("Active")?).ok()?;
    Some((session_type, active))
  }

  /// An XEmbed system tray on the X display. Only in an X11 session
  /// ([`super::probes_xembed`]): in a Wayland session `$DISPLAY` is
  /// Xwayland's, and connecting can start an on-demand Xwayland.
  fn xembed_tray(session_type: &str) -> bool {
    use x11rb::connection::Connection as _;
    use x11rb::protocol::xproto::ConnectionExt as _;
    if !super::probes_xembed(session_type, env("DISPLAY").is_some()) {
      return false;
    }
    let Ok((conn, screen)) = x11rb::connect(None) else {
      return false;
    };
    let name = format!("_NET_SYSTEM_TRAY_S{screen}");
    let Some(atom) = conn
      .intern_atom(true, name.as_bytes())
      .ok()
      .and_then(|c| c.reply().ok())
      .map(|r| r.atom)
    else {
      return false;
    };
    if atom == x11rb::NONE {
      return false;
    }
    let owner = conn
      .get_selection_owner(atom)
      .ok()
      .and_then(|c| c.reply().ok())
      .map(|r| r.owner)
      .unwrap_or(x11rb::NONE);
    let _ = conn.flush();
    owner != x11rb::NONE
  }

  const NOTIFICATIONS: &str = "org.freedesktop.Notifications";

  /// Who owns org.freedesktop.Notifications now (never starting one), and
  /// whether D-Bus could start one.
  fn probe_notifications(
    conn: &Connection,
    owned: &HashSet<String>,
    activatable: &HashSet<String>,
    f: &mut PlatformFeatures,
  ) {
    if owned.contains(NOTIFICATIONS) {
      let name = conn
        .call_method(
          Some(NOTIFICATIONS),
          "/org/freedesktop/Notifications",
          Some(NOTIFICATIONS),
          "GetServerInformation",
          &(),
        )
        .ok()
        .and_then(|m| {
          m.body()
            .deserialize::<(String, String, String, String)>()
            .ok()
        })
        .map(|(name, ..)| name)
        .filter(|n| !n.is_empty());
      f.notification_server = Some(name.unwrap_or_else(|| "unknown".into()));
    } else {
      f.notification_activatable = activatable.contains(NOTIFICATIONS);
    }
  }

  /// Whether a notification sent now would reach a server: one owns the
  /// name, or D-Bus starts one for it. The start is tried once; an
  /// installed daemon that fails to start in this session (Sway with no
  /// daemon of its own) is remembered for the process, so no Notify waits
  /// for an activation that won't come.
  pub(super) fn notification_server_usable() -> bool {
    use std::sync::atomic::{AtomicBool, Ordering};
    static ACTIVATION_FAILED: AtomicBool = AtomicBool::new(false);
    let Some(conn) = notification_connection() else {
      return false;
    };
    if has_owner(&conn, NOTIFICATIONS) {
      return true;
    }
    if ACTIVATION_FAILED.load(Ordering::Relaxed)
      || !names(&conn, "ListActivatableNames").contains(NOTIFICATIONS)
    {
      return false;
    }
    let started = conn
      .call_method(
        Some("org.freedesktop.DBus"),
        "/org/freedesktop/DBus",
        Some("org.freedesktop.DBus"),
        "StartServiceByName",
        &(NOTIFICATIONS, 0u32),
      )
      .is_ok();
    ACTIVATION_FAILED.store(!started, Ordering::Relaxed);
    started
  }

  /// One session-bus connection for every permission query, made on the
  /// first, and again after a failed attempt or once the cached one died
  /// (the bus went away: it no longer answers a Ping). Its method timeout
  /// is 5 s: a daemon may take a few seconds to start. The lock is never
  /// held across the connect or the Ping, so a slow bus holds up only the
  /// query that waits for it.
  fn notification_connection() -> Option<Connection> {
    static CONN: std::sync::Mutex<Option<Connection>> =
      std::sync::Mutex::new(None);
    let cached = CONN.lock().unwrap().clone();
    if let Some(conn) = cached {
      if alive(&conn) {
        return Some(conn);
      }
      // Dead: drop it, unless another query already replaced it.
      let mut slot = CONN.lock().unwrap();
      if slot
        .as_ref()
        .is_some_and(|c| c.unique_name() == conn.unique_name())
      {
        *slot = None;
      }
    }
    let fresh = connect(Duration::from_secs(5))?;
    let mut slot = CONN.lock().unwrap();
    // A query that connected meanwhile keeps its connection.
    Some(slot.get_or_insert(fresh).clone())
  }

  /// The bus still answers on `conn` (org.freedesktop.DBus.Peer.Ping to the
  /// bus itself; a closed connection fails at once).
  fn alive(conn: &Connection) -> bool {
    conn
      .call_method(
        Some("org.freedesktop.DBus"),
        "/org/freedesktop/DBus",
        Some("org.freedesktop.DBus.Peer"),
        "Ping",
        &(),
      )
      .is_ok()
  }

  /// Follow the StatusNotifierWatcher's owner on a thread of its own (the
  /// winit event loop doesn't iterate GLib, and zbus needs no GLib loop, so
  /// the GTK thread's isn't used) and fire the change handler when a tray
  /// host appears or goes away.
  pub(super) fn follow_tray_host() {
    let Some(conn) = connect(TIMEOUT) else {
      return;
    };
    let _ = std::thread::Builder::new()
      .name("laufey-tray-host".into())
      .spawn(move || {
        let Ok(proxy) = zbus::blocking::fdo::DBusProxy::new(&conn) else {
          return;
        };
        let Ok(changes) =
          proxy.receive_name_owner_changed_with_args(&[(0, WATCHER)])
        else {
          return;
        };
        // Subscribed first, then read, so no change in between is lost.
        let mut present = has_owner(&conn, WATCHER);
        for change in changes {
          let Ok(args) = change.args() else { continue };
          let now = args.new_owner().is_some();
          if now != present {
            present = now;
            super::fire_changed();
          }
        }
      });
  }

  /// The tray part of a probe on one connection (`conn`: the session bus).
  fn tray_on(conn: Option<&Connection>) -> PlatformFeatures {
    let session = session_type();
    PlatformFeatures {
      os: "linux",
      tray_xembed: xembed_tray(&session),
      tray_library: super::tray_library(),
      session_type: Some(session),
      desktop_hint: env("XDG_CURRENT_DESKTOP"),
      session_bus: conn.is_some(),
      tray_watcher: conn.is_some_and(|c| has_owner(c, WATCHER)),
      secret_service: "absent",
      secret_prompter: false,
      notification_server: None,
      notification_activatable: false,
      portal_versions: BTreeMap::new(),
    }
  }

  pub(super) fn probe_tray() -> PlatformFeatures {
    tray_on(connect(TIMEOUT).as_ref())
  }

  /// The xdg-desktop-portal versions, probed once per process (the first
  /// call may start the portal).
  fn portal_cache() -> &'static std::sync::Mutex<Option<BTreeMap<String, u32>>>
  {
    static CACHE: std::sync::Mutex<Option<BTreeMap<String, u32>>> =
      std::sync::Mutex::new(None);
    &CACHE
  }

  /// One interface's `version`: `Err` when the portal itself doesn't
  /// answer (it failed to start, or hangs), so the next interface isn't
  /// asked either; `Ok(None)` when it lacks the interface.
  fn portal_version(conn: &Connection, iface: &str) -> Result<Option<u32>, ()> {
    let reply = conn.call_method(
      Some(PORTAL),
      "/org/freedesktop/portal/desktop",
      Some("org.freedesktop.DBus.Properties"),
      "Get",
      &(format!("org.freedesktop.portal.{iface}"), "version"),
    );
    match reply {
      Ok(m) => Ok(
        m.body()
          .deserialize::<zbus::zvariant::OwnedValue>()
          .ok()
          .and_then(|v| u32::try_from(v).ok()),
      ),
      Err(zbus::Error::MethodError(name, ..))
        if !matches!(
          name.as_str(),
          "org.freedesktop.DBus.Error.ServiceUnknown"
            | "org.freedesktop.DBus.Error.NoReply"
            | "org.freedesktop.DBus.Error.TimedOut"
        ) && !name
          .as_str()
          .starts_with("org.freedesktop.DBus.Error.Spawn") =>
      {
        Ok(None)
      }
      Err(_) => Err(()),
    }
  }

  fn probe_portals(conn: &Connection, known: bool) -> BTreeMap<String, u32> {
    let mut versions = BTreeMap::new();
    if !known {
      return versions;
    }
    for iface in PORTAL_INTERFACES {
      match portal_version(conn, iface) {
        Ok(Some(v)) => {
          versions.insert(iface.to_string(), v);
        }
        Ok(None) => {}
        Err(()) => break,
      }
    }
    versions
  }

  /// The full probe on one session-bus connection. Its method timeout is
  /// 1 s, or 3 s on the first probe, whose first portal call may start
  /// xdg-desktop-portal (as backend-common does).
  pub(super) fn probe() -> PlatformFeatures {
    let cached = portal_cache().lock().unwrap().clone();
    let timeout = if cached.is_some() {
      TIMEOUT
    } else {
      PORTAL_START_TIMEOUT
    };
    let conn = connect(timeout);
    let mut f = tray_on(conn.as_ref());
    let session = f.session_type.clone().unwrap_or_default();
    let have_display =
      env("WAYLAND_DISPLAY").is_some() || env("DISPLAY").is_some();
    let graphical = super::graphical_session(
      &session,
      have_display,
      if matches!(session.as_str(), "x11" | "wayland") && have_display {
        logind_session()
      } else {
        None
      },
    );
    let Some(conn) = conn else {
      f.secret_service = "no-session-bus";
      return f;
    };
    let owned = names(&conn, "ListNames");
    let activatable = names(&conn, "ListActivatableNames");
    let known = |n: &str| owned.contains(n) || activatable.contains(n);
    f.secret_service = if owned.contains(SECRETS) {
      match get_property::<bool>(
        &conn,
        SECRETS,
        "/org/freedesktop/secrets/aliases/default",
        "org.freedesktop.Secret.Collection",
        "Locked",
      ) {
        Some(false) => "available",
        _ => "locked",
      }
    } else if activatable.contains(SECRETS) {
      "activatable"
    } else {
      "absent"
    };
    let needs_gcr = known("org.gnome.keyring");
    f.secret_prompter =
      graphical && (!needs_gcr || known("org.gnome.keyring.SystemPrompter"));
    probe_notifications(&conn, &owned, &activatable, &mut f);
    f.portal_versions = match cached {
      Some(versions) => versions,
      None => {
        let versions = probe_portals(&conn, known(PORTAL));
        portal_cache()
          .lock()
          .unwrap()
          .get_or_insert(versions)
          .clone()
      }
    };
    f
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn session_type_from_env() {
    assert_eq!(session_type(Some("wayland"), ""), "wayland");
    assert_eq!(session_type(Some("wayland"), "wayland"), "wayland");
    assert_eq!(session_type(Some("x11"), "x11"), "x11");
    // GDM's autologin into Xorg (XFCE, i3): "wayland" with only $DISPLAY.
    assert_eq!(session_type(Some("wayland"), "x11"), "x11");
    assert_eq!(session_type(Some("x11"), "wayland"), "wayland");
    assert_eq!(session_type(Some("tty"), "x11"), "tty");
    assert_eq!(session_type(Some("mir"), ""), "mir");
    // Never made graphical by the display variables (Xvfb, cron, xvfb-run
    // under systemd): unset is unknown.
    assert_eq!(session_type(Some(""), "x11"), "unknown");
    assert_eq!(session_type(None, "wayland"), "unknown");
  }

  #[test]
  fn display_backend_from_the_display_that_is_there() {
    use std::collections::HashMap;
    let run = |vars: &[(&str, &str)], sockets: &[&str]| {
      let vars: HashMap<String, String> = vars
        .iter()
        .map(|(k, v)| (k.to_string(), v.to_string()))
        .collect();
      let sockets: Vec<std::path::PathBuf> =
        sockets.iter().map(std::path::PathBuf::from).collect();
      display_backend(
        |n| vars.get(n).cloned(),
        |p| sockets.iter().any(|s| s == p),
      )
    };
    // XDG_SESSION_TYPE=wayland, DISPLAY only (the Pi's XFCE / i3).
    assert_eq!(
      run(&[("XDG_SESSION_TYPE", "wayland"), ("DISPLAY", ":0")], &[]),
      "x11"
    );
    // A stale WAYLAND_DISPLAY with no socket behind it.
    assert_eq!(
      run(
        &[
          ("WAYLAND_DISPLAY", "wayland-0"),
          ("XDG_RUNTIME_DIR", "/run/user/1"),
          ("DISPLAY", ":0")
        ],
        &[]
      ),
      "x11"
    );
    assert_eq!(
      run(
        &[
          ("WAYLAND_DISPLAY", "wayland-0"),
          ("XDG_RUNTIME_DIR", "/run/user/1"),
          ("DISPLAY", ":0")
        ],
        &["/run/user/1/wayland-0"]
      ),
      "wayland"
    );
    assert_eq!(
      run(&[("WAYLAND_DISPLAY", "/tmp/w")], &["/tmp/w"]),
      "wayland"
    );
    // Relative without XDG_RUNTIME_DIR: libwayland can't find it either.
    assert_eq!(run(&[("WAYLAND_DISPLAY", "wayland-0")], &["wayland-0"]), "");
    assert_eq!(run(&[("WAYLAND_SOCKET", "5")], &[]), "wayland");
    assert_eq!(run(&[("XDG_SESSION_TYPE", "x11")], &[]), "");
    assert_eq!(run(&[("DISPLAY", "")], &[]), "");
  }

  #[test]
  fn xembed_only_in_x11_sessions() {
    assert!(probes_xembed("x11", true));
    assert!(!probes_xembed("x11", false));
    // Wayland's $DISPLAY is Xwayland's: never connect to it.
    assert!(!probes_xembed("wayland", true));
    assert!(!probes_xembed("unknown", true));
    assert!(!probes_xembed("tty", true));
  }

  #[test]
  fn graphical_only_by_session_type_and_logind() {
    // $DISPLAY alone (XDG_SESSION_TYPE unset): no one to answer a prompt.
    assert!(!graphical_session("unknown", true, None));
    assert!(!graphical_session("tty", true, None));
    // x11 / wayland with a display; logind can't say: trusted.
    assert!(graphical_session("x11", true, None));
    assert!(graphical_session("wayland", true, None));
    assert!(!graphical_session("wayland", false, None));
    // logind says: an active x11 / wayland session only.
    assert!(graphical_session("x11", true, Some(("x11".into(), true))));
    assert!(!graphical_session("x11", true, Some(("tty".into(), true))));
    assert!(!graphical_session(
      "wayland",
      true,
      Some(("wayland".into(), false))
    ));
  }

  #[test]
  fn tray_reason_and_json() {
    let mut f = PlatformFeatures {
      os: "linux",
      session_type: Some("wayland".into()),
      desktop_hint: Some("GNOME".into()),
      session_bus: true,
      secret_service: "available",
      secret_prompter: true,
      tray_library: true,
      ..Default::default()
    };
    assert!(!f.tray_host());
    let reason = f.tray_reason().unwrap();
    assert_eq!(
      reason,
      "no tray host (StatusNotifierWatcher) on this session; some desktops \
       need an extension (XDG_CURRENT_DESKTOP=GNOME; GNOME shows tray icons \
       only with the AppIndicator extension enabled)"
    );
    assert!(!reason.contains("XEmbed"));
    // No desktop is named outside the hint's suffix, the reason's last part.
    let neutral = "no tray host (StatusNotifierWatcher) on this session; \
                   some desktops need an extension";
    assert_eq!(
      &reason[..reason.find(" (XDG_CURRENT_DESKTOP=").unwrap()],
      neutral
    );
    f.desktop_hint = Some("sway".into());
    assert_eq!(
      f.tray_reason().unwrap(),
      format!("{neutral} (XDG_CURRENT_DESKTOP=sway)")
    );
    f.desktop_hint = None;
    assert_eq!(f.tray_reason().unwrap(), neutral);
    f.desktop_hint = Some("GNOME".into());
    f.session_type = Some("x11".into());
    assert!(f.tray_reason().unwrap().contains("XEmbed"));
    f.tray_xembed = true;
    assert!(f.tray_host() && f.tray_reason().is_none());
    // The library is required either way.
    f.tray_library = false;
    assert!(!f.tray_host());
    assert!(f
      .tray_reason()
      .unwrap()
      .contains("libayatana-appindicator3"));
    f.tray_library = true;
    f.tray_xembed = false;
    f.tray_watcher = true;
    f.portal_versions.insert("Settings".into(), 2);
    f.portal_versions.insert("FileChooser".into(), 4);
    f.notification_server = Some("mako".into());
    assert_eq!(
      f.to_json(),
      "{\"os\":\"linux\",\"sessionType\":\"x11\",\"desktopHint\":\"GNOME\",\
       \"sessionBus\":true,\"trayHost\":true,\"trayReason\":null,\
       \"trayClicks\":false,\"trayTooltip\":true,\
       \"secretService\":\"available\",\"secretServicePrompt\":true,\
       \"kwallet\":null,\
       \"notificationServer\":\"mako\",\"notificationReason\":null,\
       \"portalVersions\":{\"FileChooser\":4,\"Settings\":2},\
       \"cookieEncryption\":null,\"cookieEncryptionWait\":null,\
       \"badge\":\"title\",\"badgeReason\":\"the Winit backend shows the \
       badge as a window-title prefix (it sends no launcher badge)\"}"
    );
    let mac = PlatformFeatures {
      os: "macos",
      secret_service: "os",
      secret_prompter: true,
      ..Default::default()
    };
    assert!(mac.tray_host());
    assert!(mac.to_json().contains("\"trayClicks\":true"));
    // Never "launcher-entry": the Dock tile on macOS, a title prefix
    // elsewhere.
    assert!(mac
      .to_json()
      .ends_with("\"badge\":\"dock\",\"badgeReason\":null}"));
    let windows = PlatformFeatures {
      os: "windows",
      ..Default::default()
    };
    assert!(windows
      .to_json()
      .ends_with("\"badge\":\"title\",\"badgeReason\":null}"));
    assert!(mac.to_json().contains("\"sessionType\":null"));
    assert!(mac
      .to_json()
      .contains("\"notificationServer\":null,\"notificationReason\":null"));
  }

  #[test]
  fn notification_reason() {
    // Sway with no daemon: the Notification portal is offered all the same,
    // so only the bus name says whether anything shows notifications.
    let mut f = PlatformFeatures {
      os: "linux",
      session_bus: true,
      desktop_hint: Some("sway".into()),
      ..Default::default()
    };
    f.portal_versions.insert("Notification".into(), 1);
    let reason = f.notification_reason().unwrap();
    assert!(reason.contains("nothing owns org.freedesktop.Notifications"));
    assert!(reason.contains("XDG_CURRENT_DESKTOP=sway"));
    assert!(f.to_json().contains("\"notificationServer\":null"));
    f.notification_activatable = true;
    assert!(f.notification_reason().unwrap().contains("can start one"));
    f.session_bus = false;
    assert_eq!(f.notification_reason().unwrap(), "no D-Bus session bus");
    f.notification_server = Some("mako".into());
    assert_eq!(f.notification_reason(), None);
  }
}
