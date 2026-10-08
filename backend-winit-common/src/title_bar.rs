// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Title bar preferences (API 47) for the winit / servo backends on Linux:
//! the same JSON object the CEF and WebView backends build in backend-common
//! (`title_bar.cc`, `title_bar_linux.cc`; docs/title-bar.md), read in Rust.
//! The sources, highest priority first: xdg-desktop-portal's Settings
//! (`ReadAll`, zbus), GSettings (gio), GTK's defaults. The portal's answer
//! wins key by key (on Ubuntu GNOME GSettings' button-layout can differ from
//! the portal's). The portal's `SettingChanged` is followed on a thread of
//! its own (the winit event loop runs no GLib loop).
//!
//! Winit's own client-side frame (sctk-adwaita, on a compositor without
//! server-side decorations) reads the portal's button layout and colour
//! scheme when the window is created; with `xdg-decoration` (KWin, wlroots)
//! the compositor draws the frame.
//!
//! macOS and Windows: not reported by these backends (the entry points stay
//! NULL there).

/// One source's answers; `None` for a key it didn't answer.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Settings {
  pub button_layout: Option<String>,
  pub double_click: Option<String>,
  pub font: Option<String>,
  /// Already normalised ("light", "dark", "no-preference").
  pub color_scheme: Option<String>,
  /// Already "#rrggbb".
  pub accent_color: Option<String>,
}

/// What the user set up (see laufey_title_bar.h).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Preferences {
  pub left: Vec<String>,
  pub right: Vec<String>,
  pub double_click: String,
  pub color_scheme: String,
  pub accent_color: Option<String>,
  pub font: Option<String>,
  pub source: &'static str,
}

/// GTK's decoration-layout default.
pub const GTK_DEFAULT_BUTTON_LAYOUT: &str = "menu:minimize,maximize,close";

fn known_button(item: &str) -> bool {
  matches!(
    item,
    "close" | "minimize" | "maximize" | "appmenu" | "menu" | "icon"
  )
}

/// Splits a GNOME button-layout / GTK decoration-layout into the known
/// items on each side, in order (unknown items and repeats dropped; a
/// layout without ':' is all on the left, as GTK reads it).
pub fn parse_button_layout(layout: &str) -> (Vec<String>, Vec<String>) {
  let mut placed: Vec<String> = Vec::new();
  let mut side = |s: &str| -> Vec<String> {
    let mut out = Vec::new();
    for item in s.split(',').map(str::trim) {
      if known_button(item) && !placed.iter().any(|p| p == item) {
        out.push(item.to_string());
        placed.push(item.to_string());
      }
    }
    out
  };
  match layout.split_once(':') {
    None => (side(layout), Vec::new()),
    Some((l, rest)) => {
      let left = side(l);
      // A second ':' (GTK ignores what follows it) ends the right side.
      let r = rest.split(':').next().unwrap_or("");
      (left, side(r))
    }
  }
}

/// GNOME's action-double-click-titlebar as an action; `None` if unknown.
pub fn double_click_from_gnome(value: &str) -> Option<&'static str> {
  Some(match value {
    "toggle-maximize"
    | "toggle-maximize-horizontally"
    | "toggle-maximize-vertically" => "maximize",
    "minimize" => "minimize",
    "toggle-shade" => "shade",
    "lower" => "lower",
    "menu" => "menu",
    "none" => "none",
    _ => return None,
  })
}

/// org.freedesktop.appearance color-scheme.
pub fn color_scheme_from_portal(value: u32) -> Option<&'static str> {
  Some(match value {
    0 => "no-preference",
    1 => "dark",
    2 => "light",
    _ => return None,
  })
}

/// org.gnome.desktop.interface color-scheme.
pub fn color_scheme_from_gnome(value: &str) -> Option<&'static str> {
  Some(match value {
    "default" => "no-preference",
    "prefer-dark" => "dark",
    "prefer-light" => "light",
    _ => return None,
  })
}

/// org.freedesktop.appearance accent-color as "#rrggbb" (`None` when a
/// channel is out of [0, 1]: the portal's "unset").
pub fn accent_from_portal_rgb(r: f64, g: f64, b: f64) -> Option<String> {
  if ![r, g, b].iter().all(|v| (0.0..=1.0).contains(v)) {
    return None;
  }
  let byte = |v: f64| (v * 255.0).round() as u8;
  Some(format!("#{:02x}{:02x}{:02x}", byte(r), byte(g), byte(b)))
}

/// org.gnome.desktop.interface accent-color (libadwaita's palette).
pub fn accent_from_gnome_name(name: &str) -> Option<&'static str> {
  Some(match name {
    "blue" => "#3584e4",
    "teal" => "#2190a4",
    "green" => "#3a944a",
    "yellow" => "#c88800",
    "orange" => "#ed5b00",
    "red" => "#e62d42",
    "pink" => "#d56199",
    "purple" => "#9141ac",
    "slate" => "#6f8396",
    _ => return None,
  })
}

/// The preferences from the portal's and GSettings' answers (the portal's
/// first, key by key), else GTK's defaults.
pub fn resolve(portal: &Settings, gsettings: &Settings) -> Preferences {
  let (layout, source) = match (&portal.button_layout, &gsettings.button_layout)
  {
    (Some(l), _) => (l.as_str(), "portal"),
    (None, Some(l)) => (l.as_str(), "gsettings"),
    (None, None) => (GTK_DEFAULT_BUTTON_LAYOUT, "default"),
  };
  let (left, right) = parse_button_layout(layout);
  let sources = [portal, gsettings];
  let double_click = sources
    .iter()
    .find_map(|s| s.double_click.as_deref().and_then(double_click_from_gnome))
    .unwrap_or("maximize");
  let first = |f: fn(&Settings) -> &Option<String>| {
    sources.iter().find_map(|s| f(s).clone())
  };
  Preferences {
    left,
    right,
    double_click: double_click.to_string(),
    color_scheme: first(|s| &s.color_scheme)
      .unwrap_or_else(|| "no-preference".to_string()),
    accent_color: first(|s| &s.accent_color),
    font: first(|s| &s.font),
    source,
  }
}

impl Preferences {
  /// The close button's side; with none, the side with more window buttons.
  pub fn side(&self) -> &'static str {
    if self.left.iter().any(|b| b == "close") {
      return "left";
    }
    if self.right.iter().any(|b| b == "close") {
      return "right";
    }
    let count = |v: &[String]| {
      v.iter()
        .filter(|b| *b == "minimize" || *b == "maximize")
        .count()
    };
    if count(&self.left) > count(&self.right) {
      "left"
    } else {
      "right"
    }
  }

  /// The JSON object (docs/title-bar.md), the same keys and order as
  /// backend-common's.
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
    let array = |v: &[String]| {
      format!(
        "[{}]",
        v.iter().map(|s| quote(s)).collect::<Vec<_>>().join(",")
      )
    };
    let or_null =
      |v: &Option<String>| v.as_deref().map(quote).unwrap_or("null".into());
    format!(
      "{{\"buttons\":{{\"left\":{},\"right\":{}}},\"side\":{},\
       \"doubleClick\":{},\"colorScheme\":{},\"accentColor\":{},\
       \"font\":{},\"source\":{}}}",
      array(&self.left),
      array(&self.right),
      quote(self.side()),
      quote(&self.double_click),
      quote(&self.color_scheme),
      or_null(&self.accent_color),
      or_null(&self.font),
      quote(self.source),
    )
  }
}

/// What the session says now (Linux).
#[cfg(target_os = "linux")]
pub fn probe() -> Preferences {
  resolve(&linux::portal(), &linux::gsettings())
}

type Handler = (unsafe extern "C" fn(*mut std::ffi::c_void), usize);

fn handler() -> &'static std::sync::Mutex<Option<Handler>> {
  static HANDLER: std::sync::Mutex<Option<Handler>> =
    std::sync::Mutex::new(None);
  &HANDLER
}

/// The change handler (set_title_bar_preferences_changed_handler, API 47):
/// the portal's SettingChanged is followed on a thread of its own from the
/// first handler on.
pub fn set_changed_handler(
  f: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
  user_data: *mut std::ffi::c_void,
) {
  *handler().lock().unwrap() = f.map(|f| (f, user_data as usize));
  #[cfg(target_os = "linux")]
  if f.is_some() {
    static STARTED: std::sync::Once = std::sync::Once::new();
    STARTED.call_once(linux::follow_changes);
  }
}

#[cfg(target_os = "linux")]
fn fire_changed() {
  let h = *handler().lock().unwrap();
  if let Some((f, user_data)) = h {
    // SAFETY: the runtime registered `f` with `user_data`.
    unsafe { f(user_data as *mut std::ffi::c_void) };
  }
}

#[cfg(target_os = "linux")]
mod linux {
  use std::collections::HashMap;
  use std::time::Duration;

  use zbus::blocking::Connection;
  use zbus::zvariant::OwnedValue;

  use super::Settings;

  const TIMEOUT: Duration = Duration::from_millis(1000);
  /// The first portal call may start xdg-desktop-portal.
  const PORTAL_START_TIMEOUT: Duration = Duration::from_millis(3000);
  /// How long a burst of changes settles before the answer is re-read.
  const SETTLE: Duration = Duration::from_millis(150);
  const PORTAL: &str = "org.freedesktop.portal.Desktop";
  const PORTAL_PATH: &str = "/org/freedesktop/portal/desktop";
  const SETTINGS: &str = "org.freedesktop.portal.Settings";
  const WM: &str = "org.gnome.desktop.wm.preferences";
  const APPEARANCE: &str = "org.freedesktop.appearance";
  const INTERFACE: &str = "org.gnome.desktop.interface";

  fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
  }

  fn connect(timeout: Duration) -> Option<Connection> {
    let have_bus = env("DBUS_SESSION_BUS_ADDRESS").is_some() || {
      use std::os::unix::fs::FileTypeExt;
      env("XDG_RUNTIME_DIR")
        .and_then(|dir| std::fs::metadata(format!("{dir}/bus")).ok())
        .is_some_and(|m| m.file_type().is_socket())
    };
    if !have_bus {
      return None;
    }
    zbus::blocking::connection::Builder::session()
      .ok()?
      .method_timeout(timeout)
      .build()
      .ok()
  }

  static PORTAL_CALLED: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

  type Namespaces = HashMap<String, HashMap<String, OwnedValue>>;

  fn string(
    ns: Option<&HashMap<String, OwnedValue>>,
    key: &str,
  ) -> Option<String> {
    String::try_from(ns?.get(key)?.try_clone().ok()?).ok()
  }

  fn boolean(
    ns: Option<&HashMap<String, OwnedValue>>,
    key: &str,
  ) -> Option<bool> {
    bool::try_from(ns?.get(key)?.try_clone().ok()?).ok()
  }

  /// An `(ddd)` value's channels.
  fn rgb(v: &OwnedValue) -> Option<(f64, f64, f64)> {
    let zbus::zvariant::Value::Structure(s) = &**v else {
      return None;
    };
    let f = s.fields();
    if f.len() != 3 {
      return None;
    }
    let c = |i: usize| f64::try_from(&f[i]).ok();
    Some((c(0)?, c(1)?, c(2)?))
  }

  /// What the portal's Settings answers (nothing without a portal).
  pub(super) fn portal() -> Settings {
    let first = !PORTAL_CALLED.swap(true, std::sync::atomic::Ordering::SeqCst);
    let timeout = if first { PORTAL_START_TIMEOUT } else { TIMEOUT };
    let Some(conn) = connect(timeout) else {
      return Settings::default();
    };
    let all: Option<Namespaces> = conn
      .call_method(
        Some(PORTAL),
        PORTAL_PATH,
        Some(SETTINGS),
        "ReadAll",
        &(vec![WM, APPEARANCE, INTERFACE],),
      )
      .ok()
      .and_then(|m| m.body().deserialize().ok());
    let Some(all) = all else {
      return Settings::default();
    };
    let wm = all.get(WM);
    let appearance = all.get(APPEARANCE);
    let iface = all.get(INTERFACE);
    let font = if boolean(wm, "titlebar-uses-system-font").unwrap_or(false) {
      string(iface, "font-name").or_else(|| string(wm, "titlebar-font"))
    } else {
      string(wm, "titlebar-font")
    };
    let color_scheme = appearance
      .and_then(|a| a.get("color-scheme"))
      .and_then(|v| u32::try_from(v.try_clone().ok()?).ok())
      .and_then(super::color_scheme_from_portal)
      .or_else(|| {
        string(iface, "color-scheme")
          .and_then(|s| super::color_scheme_from_gnome(&s))
      })
      .map(str::to_string);
    let accent_color = appearance
      .and_then(|a| a.get("accent-color"))
      .and_then(rgb)
      .and_then(|(r, g, b)| super::accent_from_portal_rgb(r, g, b))
      .or_else(|| {
        string(iface, "accent-color")
          .and_then(|s| super::accent_from_gnome_name(&s).map(str::to_string))
      });
    Settings {
      button_layout: string(wm, "button-layout"),
      double_click: string(wm, "action-double-click-titlebar"),
      font,
      color_scheme,
      accent_color,
    }
  }

  /// What GSettings answers (nothing for a schema that isn't installed).
  pub(super) fn gsettings() -> Settings {
    use gtk::gio;
    use gtk::gio::prelude::*;
    let open = |id: &str| -> Option<gio::Settings> {
      let schema = gio::SettingsSchemaSource::default()?.lookup(id, true)?;
      Some(gio::Settings::new_full(
        &schema,
        None::<&gio::SettingsBackend>,
        None,
      ))
    };
    let get = |s: &Option<gio::Settings>, key: &str| -> Option<String> {
      let s = s.as_ref()?;
      if !s.settings_schema()?.has_key(key) {
        return None;
      }
      s.value(key).str().map(str::to_string)
    };
    let wm = open(WM);
    let iface = open(INTERFACE);
    let system_font = wm.as_ref().is_some_and(|s| {
      s.settings_schema()
        .is_some_and(|sc| sc.has_key("titlebar-uses-system-font"))
        && s.boolean("titlebar-uses-system-font")
    });
    let font = if system_font {
      get(&iface, "font-name").or_else(|| get(&wm, "titlebar-font"))
    } else {
      get(&wm, "titlebar-font")
    };
    Settings {
      button_layout: get(&wm, "button-layout"),
      double_click: get(&wm, "action-double-click-titlebar"),
      font,
      color_scheme: get(&iface, "color-scheme")
        .and_then(|s| super::color_scheme_from_gnome(&s))
        .map(str::to_string),
      accent_color: get(&iface, "accent-color")
        .and_then(|s| super::accent_from_gnome_name(&s))
        .map(str::to_string),
    }
  }

  fn relevant(ns: &str, key: &str) -> bool {
    match ns {
      WM => matches!(
        key,
        "button-layout"
          | "action-double-click-titlebar"
          | "titlebar-font"
          | "titlebar-uses-system-font"
      ),
      APPEARANCE => matches!(key, "color-scheme" | "accent-color"),
      INTERFACE => matches!(key, "color-scheme" | "accent-color" | "font-name"),
      _ => false,
    }
  }

  /// Follows the portal's SettingChanged; after a burst settles, fires the
  /// handler when the answer differs from the last one reported. Subscribed
  /// before this returns, so a change made right after the handler is set
  /// is heard.
  pub(super) fn follow_changes() {
    let Some(conn) = connect(TIMEOUT) else {
      return;
    };
    let rule = zbus::MatchRule::builder()
      .msg_type(zbus::message::Type::Signal)
      .sender(PORTAL)
      .and_then(|b| b.interface(SETTINGS))
      .and_then(|b| b.member("SettingChanged"))
      .and_then(|b| b.path(PORTAL_PATH))
      .map(|b| b.build());
    let Ok(rule) = rule else { return };
    let Ok(mut messages) =
      zbus::blocking::MessageIterator::for_match_rule(rule, &conn, None)
    else {
      return;
    };
    // After subscribing, so no change in between is lost.
    let mut last = super::probe().to_json();
    let (tx, rx) = std::sync::mpsc::channel::<()>();
    // Settle: collect a burst, then re-read once.
    let _ = std::thread::Builder::new()
      .name("laufey-title-bar-settle".into())
      .spawn(move || {
        while rx.recv().is_ok() {
          std::thread::sleep(SETTLE);
          while rx.try_recv().is_ok() {}
          let json = super::probe().to_json();
          if json != last {
            last = json;
            super::fire_changed();
          }
        }
      });
    let _ = std::thread::Builder::new()
      .name("laufey-title-bar".into())
      .spawn(move || {
        let _conn = conn;
        for msg in messages.by_ref() {
          let Ok(msg) = msg else { continue };
          let Ok((ns, key, _)) =
            msg.body().deserialize::<(String, String, OwnedValue)>()
          else {
            continue;
          };
          if relevant(&ns, &key) && tx.send(()).is_err() {
            return;
          }
        }
      });
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  fn v(items: &[&str]) -> Vec<String> {
    items.iter().map(|s| s.to_string()).collect()
  }

  #[test]
  fn button_layouts() {
    assert_eq!(
      parse_button_layout(":minimize,maximize,close"),
      (v(&[]), v(&["minimize", "maximize", "close"]))
    );
    assert_eq!(
      parse_button_layout("close,minimize,maximize:"),
      (v(&["close", "minimize", "maximize"]), v(&[]))
    );
    assert_eq!(
      parse_button_layout("appmenu:close"),
      (v(&["appmenu"]), v(&["close"]))
    );
    // Unknown items, spaces and repeats; a second ':' ends the right side.
    assert_eq!(
      parse_button_layout(" icon , spacer:close,close:minimize"),
      (v(&["icon"]), v(&["close"]))
    );
    assert_eq!(parse_button_layout("close"), (v(&["close"]), v(&[])));
  }

  #[test]
  fn the_portal_wins_over_gsettings() {
    // Ubuntu GNOME: GSettings says appmenu:close, the portal (what GTK 4
    // and sandboxed apps use) says :minimize,maximize,close.
    let portal = Settings {
      button_layout: Some(":minimize,maximize,close".into()),
      ..Default::default()
    };
    let gsettings = Settings {
      button_layout: Some("appmenu:close".into()),
      double_click: Some("minimize".into()),
      color_scheme: Some("dark".into()),
      ..Default::default()
    };
    let p = resolve(&portal, &gsettings);
    assert_eq!(p.right, v(&["minimize", "maximize", "close"]));
    assert_eq!(p.source, "portal");
    // A key the portal didn't answer comes from GSettings.
    assert_eq!(p.double_click, "minimize");
    assert_eq!(p.color_scheme, "dark");
    assert_eq!(p.side(), "right");
  }

  #[test]
  fn gtk_defaults_when_nothing_answers() {
    let p = resolve(&Settings::default(), &Settings::default());
    assert_eq!(p.source, "default");
    assert_eq!(p.left, v(&["menu"]));
    assert_eq!(p.right, v(&["minimize", "maximize", "close"]));
    assert_eq!(p.double_click, "maximize");
    assert_eq!(p.color_scheme, "no-preference");
    assert_eq!(
      p.to_json(),
      "{\"buttons\":{\"left\":[\"menu\"],\"right\":[\"minimize\",\
       \"maximize\",\"close\"]},\"side\":\"right\",\"doubleClick\":\
       \"maximize\",\"colorScheme\":\"no-preference\",\"accentColor\":null,\
       \"font\":null,\"source\":\"default\"}"
    );
  }

  #[test]
  fn values() {
    assert_eq!(double_click_from_gnome("toggle-maximize"), Some("maximize"));
    assert_eq!(double_click_from_gnome("toggle-shade"), Some("shade"));
    assert_eq!(double_click_from_gnome("bogus"), None);
    assert_eq!(color_scheme_from_portal(1), Some("dark"));
    assert_eq!(color_scheme_from_portal(7), None);
    assert_eq!(color_scheme_from_gnome("prefer-light"), Some("light"));
    assert_eq!(
      accent_from_portal_rgb(0.2392, 0.6823, 0.9137).as_deref(),
      Some("#3daee9")
    );
    assert_eq!(accent_from_portal_rgb(-1.0, 0.5, 0.5), None);
    assert_eq!(accent_from_gnome_name("orange"), Some("#ed5b00"));
  }

  #[test]
  fn close_on_the_left() {
    let p = resolve(
      &Settings {
        button_layout: Some("close,maximize,minimize:".into()),
        ..Default::default()
      },
      &Settings::default(),
    );
    assert_eq!(p.side(), "left");
  }
}
