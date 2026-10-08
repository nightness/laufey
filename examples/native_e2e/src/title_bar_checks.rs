//! Title bar preferences (API 47; LAUFEY_E2E_ONLY=title-bar,
//! scripts/native-e2e-run.sh --title-bar).
//!
//! Every backend answers the same JSON object. On macOS the buttons are on
//! the left, on Windows on the right. On Linux the answer comes from
//! xdg-desktop-portal's Settings first:
//!
//! - Under Xvfb with a private session bus (LAUFEY_E2E_TITLEBAR_STUB=1, set
//!   by the run script) this battery owns `org.freedesktop.portal.Desktop`
//!   itself with a stub Settings interface: the answer must be the stub's
//!   (source "portal"), a SettingChanged that moves the buttons must reach
//!   the change handler with the new layout, and an unrelated one must not.
//! - In a real desktop session (LAUFEY_E2E_HOST_SESSION=1) the session's
//!   own portal answers. Expectations:
//!
//! ```text
//! LAUFEY_E2E_EXPECT_TITLEBAR_SOURCE=portal|gsettings|default
//! LAUFEY_E2E_EXPECT_TITLEBAR_SIDE=left|right
//! LAUFEY_E2E_TITLEBAR_SET_CMD="…"       changes a setting (sh -c), e.g.
//!                                       `gsettings set … button-layout`
//! LAUFEY_E2E_TITLEBAR_EXPECT_AFTER="…"  a substring of the new answer
//! LAUFEY_E2E_TITLEBAR_RESTORE_CMD="…"   puts it back (the change
//!                                       handler must fire again)
//! ```
//!
//! An unset expectation is not checked.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use crate::{check, na};

fn expect(name: &str) -> Option<String> {
  std::env::var(name).ok().filter(|v| !v.is_empty())
}

/// Waits until the change handler has fired more than `seen` times.
async fn changed_after(count: &AtomicUsize, seen: usize, secs: u64) -> bool {
  for _ in 0..secs * 20 {
    if count.load(Ordering::SeqCst) > seen {
      return true;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  count.load(Ordering::SeqCst) > seen
}

fn shape_ok(json: &str) -> bool {
  json.starts_with("{\"buttons\":{\"left\":[")
    && [
      "\"side\":\"",
      "\"doubleClick\":\"",
      "\"colorScheme\":\"",
      "\"source\":\"",
    ]
    .iter()
    .all(|k| json.contains(k))
}

pub(crate) async fn run() {
  #[cfg(target_os = "linux")]
  let stub = if std::env::var_os("LAUFEY_E2E_TITLEBAR_STUB").is_some() {
    match stub::start().await {
      Ok(s) => Some(s),
      Err(e) => {
        check(&format!("the stub portal owns its name ({e})"), false);
        return;
      }
    }
  } else {
    None
  };

  let Some(json) = laufey::title_bar_preferences() else {
    if cfg!(target_os = "linux")
      || std::env::var("LAUFEY_E2E_BACKEND").as_deref() != Ok("winit")
    {
      check("title_bar_preferences answers (API 47)", false);
    } else {
      na("title_bar_preferences (Winit reports it on Linux only)");
    }
    return;
  };
  eprintln!("[e2e] title bar preferences: {json}");
  check(
    "title_bar_preferences is the documented JSON object",
    shape_ok(&json),
  );

  #[cfg(target_os = "macos")]
  check(
    "macOS: the buttons are on the left, close first",
    json.contains("\"left\":[\"close\",\"minimize\",\"maximize\"]")
      && json.contains("\"side\":\"left\"")
      && json.contains("\"source\":\"os\""),
  );
  #[cfg(target_os = "windows")]
  check(
    "Windows: the buttons are on the right, a double click maximizes",
    json.contains("\"right\":[\"minimize\",\"maximize\",\"close\"]")
      && json.contains("\"side\":\"right\"")
      && json.contains("\"doubleClick\":\"maximize\""),
  );
  if let Some(want) = expect("LAUFEY_E2E_EXPECT_TITLEBAR_SOURCE") {
    check(
      &format!("the button layout comes from {want}"),
      json.contains(&format!("\"source\":\"{want}\"")),
    );
  }
  if let Some(want) = expect("LAUFEY_E2E_EXPECT_TITLEBAR_SIDE") {
    check(
      &format!("the close button is on the {want}"),
      json.contains(&format!("\"side\":\"{want}\"")),
    );
  }

  let count = Arc::new(AtomicUsize::new(0));
  let c = count.clone();
  let follows = laufey::on_title_bar_preferences_changed(move || {
    c.fetch_add(1, Ordering::SeqCst);
  });
  check("the change handler can be set", follows);

  #[cfg(target_os = "linux")]
  if let Some(stub) = stub {
    check(
      "the stub portal's layout and colour scheme are the answer",
      json.contains("\"right\":[\"minimize\",\"maximize\",\"close\"]")
        && json.contains("\"source\":\"portal\"")
        && json.contains("\"colorScheme\":\"dark\""),
    );
    let seen = count.load(Ordering::SeqCst);
    stub.set_layout("close,minimize,maximize:").await;
    let fired = changed_after(&count, seen, 5).await;
    let after = laufey::title_bar_preferences().unwrap_or_default();
    eprintln!("[e2e] after SettingChanged: {after}");
    check(
      "a SettingChanged that moves the buttons reaches the handler",
      fired
        && after.contains("\"left\":[\"close\",\"minimize\",\"maximize\"]")
        && after.contains("\"side\":\"left\""),
    );
    let seen = count.load(Ordering::SeqCst);
    stub.unrelated_change().await;
    check(
      "an unrelated SettingChanged doesn't",
      !changed_after(&count, seen, 1).await,
    );
    return;
  }

  if let Some(cmd) = expect("LAUFEY_E2E_TITLEBAR_SET_CMD") {
    let seen = count.load(Ordering::SeqCst);
    let ran = std::process::Command::new("sh")
      .args(["-c", &cmd])
      .status()
      .is_ok_and(|s| s.success());
    check(&format!("ran `{cmd}`"), ran);
    let fired = changed_after(&count, seen, 15).await;
    let after = laufey::title_bar_preferences().unwrap_or_default();
    eprintln!("[e2e] after `{cmd}`: {after}");
    check("the change reached the handler", fired);
    if let Some(want) = expect("LAUFEY_E2E_TITLEBAR_EXPECT_AFTER") {
      check(&format!("the new answer has {want}"), after.contains(&want));
    }
    if let Some(restore) = expect("LAUFEY_E2E_TITLEBAR_RESTORE_CMD") {
      let seen = count.load(Ordering::SeqCst);
      let _ = std::process::Command::new("sh")
        .args(["-c", &restore])
        .status();
      check(
        "putting it back reaches the handler too",
        changed_after(&count, seen, 15).await
          && laufey::title_bar_preferences().as_deref() == Some(json.as_str()),
      );
    }
  }
}

/// A stand-in xdg-desktop-portal Settings interface on the run's private
/// session bus.
#[cfg(target_os = "linux")]
mod stub {
  use std::collections::HashMap;
  use std::sync::{Arc, Mutex};

  use zbus::zvariant::{OwnedValue, Value};

  const PATH: &str = "/org/freedesktop/portal/desktop";
  const IFACE: &str = "org.freedesktop.portal.Settings";
  const WM: &str = "org.gnome.desktop.wm.preferences";

  type Values = Arc<Mutex<HashMap<String, HashMap<String, OwnedValue>>>>;

  struct Settings {
    values: Values,
  }

  #[zbus::interface(name = "org.freedesktop.portal.Settings")]
  impl Settings {
    fn read_all(
      &self,
      namespaces: Vec<String>,
    ) -> HashMap<String, HashMap<String, OwnedValue>> {
      let values = self.values.lock().unwrap();
      let mut out = HashMap::new();
      for ns in namespaces {
        if let Some(keys) = values.get(&ns) {
          let keys = keys
            .iter()
            .filter_map(|(k, v)| Some((k.clone(), v.try_clone().ok()?)))
            .collect::<HashMap<_, _>>();
          out.insert(ns, keys);
        }
      }
      out
    }

    #[zbus(property)]
    fn version(&self) -> u32 {
      2
    }
  }

  pub(super) struct Stub {
    conn: zbus::Connection,
    values: Values,
  }

  fn owned(v: Value<'static>) -> OwnedValue {
    OwnedValue::try_from(v).expect("a plain value")
  }

  pub(super) async fn start() -> zbus::Result<Stub> {
    let mut wm = HashMap::new();
    wm.insert(
      "button-layout".to_string(),
      owned(Value::from(":minimize,maximize,close")),
    );
    let mut appearance = HashMap::new();
    appearance.insert("color-scheme".to_string(), owned(Value::from(1u32)));
    let mut values = HashMap::new();
    values.insert(WM.to_string(), wm);
    values.insert("org.freedesktop.appearance".to_string(), appearance);
    let values: Values = Arc::new(Mutex::new(values));
    let conn = zbus::connection::Builder::session()?
      .name("org.freedesktop.portal.Desktop")?
      .serve_at(
        PATH,
        Settings {
          values: values.clone(),
        },
      )?
      .build()
      .await?;
    Ok(Stub { conn, values })
  }

  impl Stub {
    async fn change(&self, ns: &str, key: &str, value: &'static str) {
      self
        .values
        .lock()
        .unwrap()
        .entry(ns.to_string())
        .or_default()
        .insert(key.to_string(), owned(Value::from(value)));
      let _ = self
        .conn
        .emit_signal(
          None::<&str>,
          PATH,
          IFACE,
          "SettingChanged",
          &(ns, key, Value::from(value)),
        )
        .await;
    }

    pub(super) async fn set_layout(&self, layout: &'static str) {
      self.change(WM, "button-layout", layout).await;
    }

    pub(super) async fn unrelated_change(&self) {
      self.change(WM, "theme", "HighContrast").await;
    }
  }
}
