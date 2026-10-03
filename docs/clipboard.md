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
  converted (TIFF and other `NSImage` types on macOS, a DIB on Windows, any
  pixbuf format on Linux).
- **Formats** are MIME types separated by `\n`: `text/plain`, `text/html`,
  `image/png` (any image), `text/uri-list` (files), `text/rtf`. An empty string
  is an empty clipboard.
- **Change events** fire on the UI thread (on Windows, laufey's I/O thread) for
  every change, including this app's own writes. `clipboard_capabilities`
  reports `LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS` where they work.

| OS      | Text                         | HTML                      | Image                            | Change events                                                                                                                        |
| ------- | ---------------------------- | ------------------------- | -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| macOS   | `NSPasteboardTypeString`     | `NSPasteboardTypeHTML`    | `NSPasteboardTypePNG` + TIFF     | No OS notification: the pasteboard's `changeCount` is polled twice a second, only while a handler is set.                            |
| Windows | `CF_UNICODETEXT`             | `HTML Format` (`CF_HTML`) | registered `PNG` + `CF_DIBV5`    | `AddClipboardFormatListener` on laufey's I/O window (`WM_CLIPBOARDUPDATE`).                                                          |
| Linux   | GTK `CLIPBOARD` text targets | `text/html`               | `image/png` + gdk-pixbuf formats | GTK's `owner-change`: X11 needs the XFixes extension; on Wayland GTK hears of changes only while one of the app's windows has focus. |
| Winit   | shell tools (below)          | —                         | —                                | —                                                                                                                                    |

The CEF and WebView backends share these implementations. Data written to the
clipboard on Linux is offered to a running clipboard manager, so it survives the
app exiting.

The engine-free Winit backend has no web engine bundled, so it shells out to the
platform's standard clipboard tools instead — `pbcopy` / `pbpaste` on macOS,
`clip` / `Get-Clipboard` on Windows, and `wl-clipboard` (falling back to
`xclip`) on Linux — for text only (`clipboard_capabilities` reports TEXT). These
are present on a default desktop install of each platform; if the Linux tools
are missing, reads return `None` and writes are a no-op.
