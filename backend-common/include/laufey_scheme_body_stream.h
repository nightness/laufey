// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The body of a custom-scheme response on WebKitGTK: a GInputStream WebKit
// reads, fed by the runtime's writes, that never blocks the writer.
//
// WebKitGTK takes a response body as a GInputStream and reads it on the GTK
// main thread. Feeding it through a pipe made the runtime's write block
// whenever 64 KiB were unread (the GTK thread busy, or WebKit not reading
// yet), and the runtime writes from its event loop thread, so one slow
// response stalled the whole app. This stream is an in-memory queue instead:
// a write appends and returns at once, WebKit reads through
// GPollableInputStream (no thread is ever parked on it), and the writer is
// told when to stop:
//   - the reader is behind: while kSchemeResponseHighWater bytes or more wait,
//     a write takes nothing and returns 0, and the writer retries
//     (backpressure, API 44);
//   - the page stopped reading (WebKit closed / dropped the stream, e.g. the
//     fetch was aborted or the document replaced): the next write fails;
//   - more than kSchemeBodyMaxQueued bytes are waiting for a reader: the
//     response fails (WebKit's read gets an error, so the page's read
//     rejects) and the write fails, the semantics of the WebView2 backend's
//     cap (wv2_scheme_stream.cc).
//
// Linux only (GIO).

#ifndef LAUFEY_SCHEME_BODY_STREAM_H_
#define LAUFEY_SCHEME_BODY_STREAM_H_

#include <gio/gio.h>

#include "laufey_scheme_cancel.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace laufey_common {

// Bytes held for a reader that isn't reading before the response fails (the
// WebView2 backend's kMaxQueued).
constexpr size_t kSchemeBodyMaxQueued = 64 * 1024 * 1024;

class SchemeBodyQueue;

// The writer's end. Thread-safe; Write / End / Fail never block.
class SchemeBodyWriter {
 public:
  explicit SchemeBodyWriter(size_t max_queued = kSchemeBodyMaxQueued,
                            size_t high_water = kSchemeResponseHighWater);
  ~SchemeBodyWriter();  // Ends the body (EOF) if not ended / failed yet.
  SchemeBodyWriter(const SchemeBodyWriter&) = delete;
  SchemeBodyWriter& operator=(const SchemeBodyWriter&) = delete;

  // A new reference to the stream to hand to WebKit (the caller owns it).
  // Call once, on the thread whose GMainContext will read it (the GTK main
  // thread): readers there are woken when data arrives.
  GInputStream* CreateStream();

  // Append `len` bytes. Returns `len`; 0 (nothing taken) while `high_water`
  // bytes or more wait for the reader, so the writer retries later
  // (backpressure, API 44); or -1 if the reader is gone or the body failed
  // (now or earlier, e.g. a single write took it past the cap).
  intptr_t Write(const uint8_t* data, size_t len);
  // End of body: the reader sees EOF after what was written.
  void End();
  // Fail the body: the reader's next read errors with `message`.
  void Fail(const char* message);
  // Called once, on the thread that lets the stream go, if the reader goes
  // away before the body ended or failed (the page aborted the request).
  void SetReaderGoneHandler(std::function<void()> handler);

  // Tests: bytes written and not read yet; whether the reader is gone.
  size_t queued() const;
  bool reader_gone() const;

 private:
  std::shared_ptr<SchemeBodyQueue> queue_;
};

}  // namespace laufey_common

#endif  // LAUFEY_SCHEME_BODY_STREAM_H_
