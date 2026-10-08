# Clipboard

laufey reads and writes the operating system's clipboard as plain text, HTML
(with a plain-text alternative) and PNG images, lists the kinds of content it
holds, and reports when it changes.

```rust
// Text, mirroring navigator.clipboard.readText() / writeText().
laufey::write_clipboard_text("hello from laufey");
let text: Option<String> = laufey::read_clipboard_text();

// HTML, with the plain text other apps paste.
laufey::write_clipboard_html("<b>hello</b>", Some("hello"));
let html: Option<String> = laufey::read_clipboard_html();

// PNG images.
laufey::write_clipboard_image(&png_bytes);
let png: Option<Vec<u8>> = laufey::read_clipboard_image();

// What's there, and when it changes.
let formats = laufey::read_clipboard_formats(); // ["text/plain", "text/html"]
laufey::on_clipboard_change(|| println!("clipboard changed"));
```

Passing an empty string to `write_clipboard_text` clears the clipboard.

## API

C ABI. The text pair dates from API 27; the rest is API ≥ 39:

```c
char* (*read_clipboard_text)(void* backend_data);       // string_free
void (*write_clipboard_text)(void* backend_data, const char* text);

uint32_t (*clipboard_capabilities)(void* backend_data);
char* (*read_clipboard_html)(void* backend_data);       // string_free
bool (*write_clipboard_html)(void* backend_data, const char* html,
                             const char* text_or_null);
uint8_t* (*read_clipboard_image)(void* backend_data, size_t* len_out);
                                                        // buffer_free
bool (*write_clipboard_image)(void* backend_data, const uint8_t* png,
                              size_t len);
char* (*read_clipboard_formats)(void* backend_data);    // string_free
void (*set_clipboard_change_handler)(void* backend_data,
                                     laufey_clipboard_change_fn handler,
                                     void* user_data);
void (*buffer_free)(void* backend_data, void* buffer);
```

- **Any thread.** From API 39 every clipboard call, the text pair included, may
  be made from any thread: macOS runs it on the main queue and Linux on the GTK
  thread, and Windows on the calling thread, retrying for a moment while another
  app has the clipboard open. A read waits for the app that owns the clipboard
  to answer, so keep it off latency-critical paths.
- **Size cap.** A read larger than `LAUFEY_CLIPBOARD_MAX_READ_BYTES` (64 MiB)
  answers `NULL`, so a huge copy in another app can't exhaust this one's memory.
- **HTML** is UTF-8 as the source app wrote it, a fragment or a whole document.
  On Windows the `CF_HTML` header is stripped and the fragment returned; on
  Linux a UTF-16 `text/html` (as Firefox writes it) is converted.
- **Images** go out as PNG plus the platform's native image format, so every app
  can paste them: TIFF on macOS, `CF_DIBV5` on Windows (which Windows also
  synthesizes as `CF_DIB` / `CF_BITMAP`), and every format gdk-pixbuf can write
  on Linux. A write that isn't a decodable PNG is refused. A read returns PNG
  bytes: the PNG itself when the source offered one, otherwise the image
  converted (TIFF and other `NSImage` types on macOS, a DIB on Windows, JPEG,
  BMP or GIF on Linux).
- **Formats** are MIME types separated by `\n`: `text/plain`, `text/html`,
  `image/png` (any image), `text/uri-list` (files), `text/rtf`. An empty string
  is an empty clipboard.
- **Change events** fire on the UI thread (on Windows, laufey's I/O thread) for
  every change, including this app's own writes. `clipboard_capabilities`
  reports `LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS` where they work.

| OS      | Text                         | HTML                      | Image                            | Change events                                                                                                                                                                |
| ------- | ---------------------------- | ------------------------- | -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| macOS   | `NSPasteboardTypeString`     | `NSPasteboardTypeHTML`    | `NSPasteboardTypePNG` + TIFF     | No OS notification: the pasteboard's `changeCount` is polled twice a second, only while a handler is set.                                                                    |
| Windows | `CF_UNICODETEXT`             | `HTML Format` (`CF_HTML`) | registered `PNG` + `CF_DIBV5`    | `AddClipboardFormatListener` on laufey's I/O window (`WM_CLIPBOARDUPDATE`).                                                                                                  |
| Linux   | GTK `CLIPBOARD` text targets | `text/html`               | `image/png` + gdk-pixbuf formats | GTK's `owner-change`: X11 needs the XFixes extension; on Wayland, the data-control device's selection events, or GTK's while one of the app's windows has focus (see below). |
| Winit   | shell tools (below)          | —                         | —                                | —                                                                                                                                                                            |

The CEF and WebView backends share these implementations. Data written to the
clipboard on Linux is offered to a running clipboard manager, so it survives the
app exiting.

**Wayland.** The core protocol lets only the client with keyboard focus set or
read the clipboard. laufey's GTK connection is never that client under CEF
(Chromium's own Wayland connection owns the window's surfaces), and no app is
while it is in the background or the session is locked. So, on Wayland:

- Where the compositor offers `ext-data-control-v1` (KWin 6.2+, wlroots 0.19+
  compositors such as Sway), every clipboard operation goes through it, on a
  private Wayland connection: focused or not, both backends.
- GNOME's mutter has no data-control. Its Xwayland selection bridge copies the
  clipboard between X11 and Wayland in both directions whatever has focus, so
  there laufey is an X11 client of Xwayland's `CLIPBOARD` selection, over a
  private XCB connection served by a thread of its own (GTK and the process
  environment are left alone). The connection is opened on the first clipboard
  call, which starts Xwayland if mutter hasn't yet (the documented price: a
  moment on that first call, off the UI thread). Reads convert `TARGETS` and
  then the wanted target, following `INCR` for large transfers, every step with
  a 3 s deadline; as the owner laufey answers `TARGETS`, `TIMESTAMP` and each
  type it offers, large data through `INCR` (at most 8 transfers at once, each
  dropped after 3 s without progress). Text goes out as `UTF8_STRING` and
  `text/plain;charset=utf-8`, and as `STRING` only when it is Latin-1 (ICCCM),
  converted. Change events come from XFixes. Without Xwayland it falls back to
  GTK's Wayland clipboard, which works only while one of the app's windows has
  focus.
- Elsewhere (another compositor without data-control) GTK's Wayland clipboard is
  used: only while one of the app's windows has focus, and never under CEF.

`LAUFEY_CLIPBOARD=gtk` turns both off (GTK's default display only).

**Privacy (Linux).** The two paths above read the clipboard whatever has focus,
so an app in the background reads it while the session is unlocked, as on macOS
and Windows and as Electron does: apps that read the clipboard should do so in
response to the user. While the session is **locked** (systemd-logind's
`LockedHint` for the app's session, asked on each read with a 500 ms deadline)
every read through them is refused: text, HTML and image reads answer `NULL`,
formats an empty list, and the reason is logged once per lock
(`laufey: clipboard read refused: the session is locked`). Writes still work.
The refusal fails open: where logind can't be asked (no system bus, not in a
session, no answer within the deadline) reads are allowed. It also relies on the
screen locker setting `LockedHint`, which GNOME and KDE do but many wlroots
lockers never do (swaylock and other `ext-session-lock-v1` clients started
directly, from a keybinding or swayidle): on such sessions reads are **not**
refused while the screen is locked. Image reads, on every Linux path, take
`image/png` verbatim and otherwise only `image/jpeg`, `image/bmp` or `image/gif`
(re-encoded as PNG): no other format offered by another app reaches an image
decoder, and only those four count as `image/png` in the formats list. Apps
built on laufey (denext's `clipboard` capability, for one) should say both in
their own documentation.

The engine-free Winit backend has no web engine bundled, so it shells out to the
platform's standard clipboard tools instead — `pbcopy` / `pbpaste` on macOS,
`clip` / `Get-Clipboard` on Windows, and `wl-clipboard` (falling back to
`xclip`) on Linux — for text only (`clipboard_capabilities` reports TEXT). These
are present on a default desktop install of each platform; if the Linux tools
are missing, reads return `None` and writes are a no-op.
