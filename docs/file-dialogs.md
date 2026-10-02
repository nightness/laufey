# File dialogs

laufey shows the operating system's own open, save and folder dialogs. The same
native dialog is used by every backend of an OS, CEF included (not Chromium's
`RunFileDialog`), so an app behaves the same whatever engine it runs on.

```rust
let dialog = laufey::show_file_dialog(
  window.id(), // 0: an app-level dialog
  &laufey::FileDialogOptions {
    files: true,
    multiple: true,
    title: Some("Import images".into()),
    filters: vec![laufey::FileFilter {
      name: "Images".into(),
      extensions: vec!["png".into(), "jpg".into()],
    }],
    ..Default::default()
  },
);
match dialog.outcome.await {
  laufey::FileDialogOutcome::Accepted(paths) => println!("{paths:?}"),
  _ => println!("cancelled"),
}
```

## API

C ABI (API ≥ 39):

```c
uint32_t (*show_file_dialog)(void* backend_data, uint32_t window_id,
                             const laufey_file_dialog_options_t* options,
                             laufey_file_dialog_result_fn callback,
                             void* user_data);
bool (*cancel_file_dialog)(void* backend_data, uint32_t dialog_id);

typedef void (*laufey_file_dialog_result_fn)(void* user_data,
                                             uint32_t dialog_id, int status,
                                             const char* const* paths,
                                             size_t count);
```

- **Any thread, never blocking.** `show_file_dialog` copies the options, asks
  the backend's UI thread (on Windows, laufey's I/O thread) to show the dialog
  and returns its id at once; the runtime thread never waits for the user. The
  callback fires exactly once, on that thread, when the dialog closes.
- **Results** are absolute native paths. `status` is
  `LAUFEY_FILE_DIALOG_ACCEPTED` (with one or more paths),
  `LAUFEY_FILE_DIALOG_CANCELLED` (dismissed by the user or by
  `cancel_file_dialog`), `LAUFEY_FILE_DIALOG_BUSY` or
  `LAUFEY_FILE_DIALOG_FAILED`. BUSY and FAILED are answered synchronously, on
  the calling thread, and `show_file_dialog` then returns 0.
- **One dialog at a time per app.** A request while one is open is BUSY.
- **Cancellable.** `cancel_file_dialog(id)` closes an open dialog as if the user
  cancelled it; it returns false when that dialog isn't open.
- **Modal.** A nonzero `window_id` makes the dialog modal to that window where
  `LAUFEY_WINDOW_CAP_FILE_DIALOG_MODAL` is reported: a sheet on macOS, an owned
  dialog on Windows, a transient GTK dialog on WebKitGTK. On CEF for Linux the
  dialog is app-level (Chromium's windows aren't GTK windows). An unknown window
  id shows an app-level dialog.

### Options

```c
typedef struct laufey_file_filter {
  const char* name;               // "Images"
  const char* const* extensions;  // {"png", "jpg"}; "*" = any file
  size_t extension_count;
} laufey_file_filter_t;

typedef struct laufey_file_dialog_options {
  int kind;            // LAUFEY_FILE_DIALOG_OPEN / _SAVE
  uint32_t flags;      // LAUFEY_FILE_DIALOG_* below
  const char* title;   // NULL: the platform default
  const char* default_path;
  const char* button_label;
  const laufey_file_filter_t* filters;
  size_t filter_count;
} laufey_file_dialog_options_t;
```

| Flag                                      | Meaning                                                                                                                                                            |
| ----------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `LAUFEY_FILE_DIALOG_CHOOSE_FILES`         | OPEN: pick files (the default when no CHOOSE flag is set).                                                                                                         |
| `LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES`   | OPEN: pick directories (the folder dialog). With CHOOSE_FILES: either, where `LAUFEY_WINDOW_CAP_FILE_DIALOG_FILES_AND_DIRECTORIES` (macOS); elsewhere directories. |
| `LAUFEY_FILE_DIALOG_MULTIPLE`             | OPEN: allow several.                                                                                                                                               |
| `LAUFEY_FILE_DIALOG_SHOW_HIDDEN`          | Show dot / hidden files.                                                                                                                                           |
| `LAUFEY_FILE_DIALOG_NO_OVERWRITE_CONFIRM` | SAVE: don't ask before replacing a file. Windows and GTK honor it; macOS always asks.                                                                              |

`default_path` is a directory to start in or a file path: an OPEN dialog starts
in the file's directory, and a SAVE dialog also proposes its name. A SAVE dialog
given only a name proposes that name. Every string is UTF-8; invalid UTF-8 or an
unknown kind is FAILED. At most 64 filters and 256 extensions are used; a
leading `.` on an extension is dropped, and extensions with path separators,
`;`, `|`, `?` or a `*` other than `"*"` are ignored.

Filters: Windows and GTK show them as a type menu (GTK matches extensions
case-insensitively). macOS allows the union of every filter's extensions
(`allowedFileTypes`), and a `"*"` anywhere allows any file. A SAVE dialog on
Windows adds the first filter's first extension to a typed name without one.

### Per OS

| OS      | Dialog                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| macOS   | `NSOpenPanel` / `NSSavePanel`, a sheet on the window or `beginWithCompletionHandler`. The title also shows as the panel's message (panels have no title bar on macOS 11+).                                                                                                                                                                                                                                                                                                                      |
| Windows | `IFileOpenDialog` (`FOS_PICKFOLDERS` for folders) / `IFileSaveDialog`, with `FOS_FORCEFILESYSTEM`. Shown on laufey's own I/O thread (an STA with a message loop, as Electron does), owned by the window, so its modal loop never stalls the engine's UI thread; cancel requests reach it there and press the dialog's Cancel button (`IFileDialog::Close` is not used: one made while the dialog is still being set up is swallowed, and every later `Close` on that dialog then does nothing). |
| Linux   | `GtkFileChooserNative`, which uses the xdg-desktop-portal FileChooser when GTK decides to (inside Flatpak / Snap, or with `GTK_USE_PORTAL=1`) and a GTK dialog otherwise.                                                                                                                                                                                                                                                                                                                       |
| Winit   | Not supported (`NULL`).                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |

### Testing

```c
bool (*test_file_dialog_respond)(void* backend_data, int action,
                                 const char* path);
```

Test-only: acts on the open dialog as a user would. `LAUFEY_TEST_DIALOG_CANCEL`
closes it; `LAUFEY_TEST_DIALOG_ACCEPT` first puts `path` into it (the save
dialog's folder and name; the selection or the typed name of an open dialog) and
then accepts, so the result returns through the dialog's own completion path.
Returns false when no dialog is open yet (poll until true). A dialog that won't
take a typed path (a portal dialog) is cancelled instead.
