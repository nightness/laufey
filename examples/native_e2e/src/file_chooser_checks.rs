//! Which file chooser a Linux dialog uses (API 47; LAUFEY_E2E_ONLY=
//! file-chooser, scripts/native-e2e-run.sh --file-chooser).
//!
//! The portal's FileChooser (the desktop's own dialog) wherever
//! xdg-desktop-portal offers it, GTK's chooser otherwise; platform_features
//! says which ("fileChooser", and "fileChooserReason" for GTK's). The
//! battery checks that the report agrees with the portal's interfaces, then
//! shows a real open dialog and closes it with cancel_file_dialog (which, for
//! the portal's dialog, is Request.Close). Expectations for a real session
//! (LAUFEY_E2E_HOST_SESSION=1):
//!
//!   LAUFEY_E2E_EXPECT_FILE_CHOOSER=portal|gtk
//!   LAUFEY_E2E_FILE_CHOOSER_SHOT_CMD="…"   run (sh -c) while the dialog is
//!                                          up, e.g. a screenshot
//!   LAUFEY_E2E_FILE_CHOOSER_HOLD_MS=1500   how long the dialog stays up
//!
//! Elsewhere (macOS, Windows) only the report is checked: null.

use std::time::Duration;

use laufey::{FileDialogOptions, FileDialogOutcome};

use crate::check;

fn expect(name: &str) -> Option<String> {
  std::env::var(name).ok().filter(|v| !v.is_empty())
}

pub(crate) async fn run() {
  let Some(f) = laufey::platform_features() else {
    check("platform_features answers", false);
    return;
  };
  eprintln!("[e2e] platform features: {f}");
  if !cfg!(target_os = "linux") {
    check(
      "fileChooser is null off Linux",
      f.contains("\"fileChooser\":null"),
    );
    return;
  }
  let portal = f.contains("\"fileChooser\":\"portal\"");
  let gtk = f.contains("\"fileChooser\":\"gtk\"");
  check("fileChooser is \"portal\" or \"gtk\"", portal || gtk);
  check(
    "the portal's chooser exactly when the portal offers a FileChooser \
     (or LAUFEY_FILE_CHOOSER=gtk)",
    portal
      == (f.contains("\"FileChooser\":")
        && std::env::var("LAUFEY_FILE_CHOOSER").as_deref() != Ok("gtk")),
  );
  check(
    "GTK's chooser comes with a reason",
    !gtk || !f.contains("\"fileChooserReason\":null"),
  );
  if let Some(want) = expect("LAUFEY_E2E_EXPECT_FILE_CHOOSER") {
    check(
      &format!("the dialogs here are {want}'s"),
      f.contains(&format!("\"fileChooser\":\"{want}\"")),
    );
  }

  // A real dialog, closed by cancel_file_dialog.
  let d = laufey::show_file_dialog(
    0,
    &FileDialogOptions {
      title: Some("laufey e2e: file chooser".into()),
      ..Default::default()
    },
  );
  check("the dialog gets an id", d.id != 0);
  let hold: u64 = expect("LAUFEY_E2E_FILE_CHOOSER_HOLD_MS")
    .and_then(|v| v.parse().ok())
    .unwrap_or(1500);
  tokio::time::sleep(Duration::from_millis(hold)).await;
  if let Some(cmd) = expect("LAUFEY_E2E_FILE_CHOOSER_SHOT_CMD") {
    let _ = std::process::Command::new("sh").args(["-c", &cmd]).status();
  }
  check(
    "cancel_file_dialog closes it",
    laufey::cancel_file_dialog(d.id),
  );
  let outcome = tokio::time::timeout(Duration::from_secs(10), d.outcome)
    .await
    .unwrap_or(FileDialogOutcome::Failed);
  check(
    "the dialog settles cancelled",
    outcome == FileDialogOutcome::Cancelled,
  );
  // The slot is free again.
  let next = laufey::show_file_dialog(0, &FileDialogOptions::default());
  check("the next dialog opens", next.id != 0);
  // Cancelled once it is up: the portal closes a request it has exported
  // and named (a dialog cancelled in the instant before that is closed when
  // the answer naming it arrives, which needs the app still running).
  tokio::time::sleep(Duration::from_millis(1500)).await;
  laufey::cancel_file_dialog(next.id);
  let _ = tokio::time::timeout(Duration::from_secs(10), next.outcome).await;
  tokio::time::sleep(Duration::from_millis(500)).await;
}
