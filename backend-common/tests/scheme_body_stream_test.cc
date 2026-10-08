// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for laufey_scheme_body_stream.h (Linux): the WebKitGTK
// custom-scheme response body. A writer thread never blocks however much
// is unread; a reader on a GMainContext reads it asynchronously
// (g_input_stream_read_bytes_async, as WebKit does) byte for byte; a body
// past the cap fails both sides; a reader that went away fails the writer;
// a synchronous reader works too; and none of it logs a GLib critical (an
// async read's pollable source once did, for its child source's closure).
// Plain asserts, no WebKit.

#include "laufey_scheme_body_stream.h"
#include "laufey_scheme_cancel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using laufey_common::SchemeBodyWriter;

static int g_failures = 0;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                  \
      g_failures++;                                                         \
    }                                                                       \
  } while (0)

static uint8_t PatternAt(size_t i) {
  return static_cast<uint8_t>((i % 251) ^ ((i / 251) & 0xff));
}

// Reads a stream to its end on `context` the way WebKit does: repeated
// g_input_stream_read_bytes_async, the next one started from the callback.
struct AsyncReader {
  GMainLoop* loop;
  GInputStream* stream;
  std::vector<uint8_t> got;
  std::string error;
  bool done = false;
  int delay_ms = 0;  // pause before each read (a slow reader)

  static void OnRead(GObject* source, GAsyncResult* result, gpointer data) {
    auto* self = static_cast<AsyncReader*>(data);
    GError* error = nullptr;
    GBytes* bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source),
                                                     result, &error);
    if (!bytes) {
      self->error = error ? error->message : "unknown";
      g_clear_error(&error);
      self->Done();
      return;
    }
    gsize n = 0;
    auto* p = static_cast<const uint8_t*>(g_bytes_get_data(bytes, &n));
    self->got.insert(self->got.end(), p, p + n);
    g_bytes_unref(bytes);
    if (n == 0) {
      self->Done();
      return;
    }
    self->Next();
  }

  void Next() {
    if (delay_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    g_input_stream_read_bytes_async(stream, 8192, G_PRIORITY_DEFAULT, nullptr,
                                    OnRead, this);
  }

  void Done() {
    done = true;
    g_main_loop_quit(loop);
  }
};

static void TestWritesNeverBlockAndArriveIntact() {
  // 3 MiB written with nobody reading (below the high-water mark): every
  // write takes everything and returns at once.
  constexpr size_t kLen = 3 * 1024 * 1024 + 7;
  auto writer = std::make_shared<SchemeBodyWriter>();
  GMainContext* context = g_main_context_new();
  g_main_context_push_thread_default(context);
  GInputStream* stream = writer->CreateStream();
  CHECK(G_IS_POLLABLE_INPUT_STREAM(stream));
  CHECK(g_pollable_input_stream_can_poll(G_POLLABLE_INPUT_STREAM(stream)));
  CHECK(!g_pollable_input_stream_is_readable(G_POLLABLE_INPUT_STREAM(stream)));

  std::vector<uint8_t> body(kLen);
  for (size_t i = 0; i < kLen; i++)
    body[i] = PatternAt(i);
  auto start = std::chrono::steady_clock::now();
  bool all = true;
  for (size_t off = 0; off < kLen; off += 64 * 1024) {
    size_t n = std::min<size_t>(64 * 1024, kLen - off);
    all = all && writer->Write(body.data() + off, n) == (intptr_t)n;
  }
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count();
  CHECK(all);
  CHECK(ms < 1000);
  CHECK(writer->queued() == kLen);
  CHECK(g_pollable_input_stream_is_readable(G_POLLABLE_INPUT_STREAM(stream)));
  writer->End();

  AsyncReader reader;
  reader.loop = g_main_loop_new(context, FALSE);
  reader.stream = stream;
  reader.Next();
  g_main_loop_run(reader.loop);
  CHECK(reader.done && reader.error.empty());
  CHECK(reader.got == body);
  g_main_loop_unref(reader.loop);
  g_object_unref(stream);
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
}

static void TestHighWaterMarkTakesNothingUntilRead() {
  // Backpressure (API 44): once kSchemeResponseHighWater bytes wait, a write
  // takes nothing and returns 0 (not -1: the response goes on); after the
  // reader takes some, writes are taken again, and the body arrives whole.
  constexpr size_t kMark = laufey_common::kSchemeResponseHighWater;
  auto writer = std::make_shared<SchemeBodyWriter>();
  GMainContext* context = g_main_context_new();
  g_main_context_push_thread_default(context);
  GInputStream* stream = writer->CreateStream();
  std::vector<uint8_t> sent;
  std::vector<uint8_t> chunk(100 * 1000);
  intptr_t r = 0;
  for (int i = 0; i < 1000; i++) {
    for (size_t k = 0; k < chunk.size(); k++)
      chunk[k] = PatternAt(sent.size() + k);
    r = writer->Write(chunk.data(), chunk.size());
    if (r != (intptr_t)chunk.size())
      break;
    sent.insert(sent.end(), chunk.begin(), chunk.end());
  }
  CHECK(r == 0);
  CHECK(writer->queued() >= kMark);
  CHECK(writer->queued() < kMark + chunk.size());
  CHECK(writer->queued() == sent.size());
  // Still full: nothing taken, the stream is still alive.
  CHECK(writer->Write(chunk.data(), 1) == 0);
  CHECK(!writer->reader_gone());
  // A single write larger than the mark is taken whole when the queue is
  // below it (after the reader catches up), see below.
  std::vector<uint8_t> got(sent.size());
  GError* error = nullptr;
  gssize n = g_pollable_input_stream_read_nonblocking(
      G_POLLABLE_INPUT_STREAM(stream), got.data(), 1024 * 1024, nullptr,
      &error);
  CHECK(n > 0 && error == nullptr);
  size_t read_so_far = static_cast<size_t>(n);
  while (writer->queued() >= kMark) {
    n = g_pollable_input_stream_read_nonblocking(
        G_POLLABLE_INPUT_STREAM(stream), got.data() + read_so_far,
        got.size() - read_so_far, nullptr, &error);
    CHECK(n > 0 && error == nullptr);
    read_so_far += static_cast<size_t>(n);
  }
  std::vector<uint8_t> big(kMark + 12345);
  for (size_t k = 0; k < big.size(); k++)
    big[k] = PatternAt(sent.size() + k);
  CHECK(writer->Write(big.data(), big.size()) == (intptr_t)big.size());
  sent.insert(sent.end(), big.begin(), big.end());
  CHECK(writer->Write(chunk.data(), 1) == 0);
  writer->End();
  got.resize(read_so_far);
  AsyncReader reader;
  reader.loop = g_main_loop_new(context, FALSE);
  reader.stream = stream;
  reader.Next();
  g_main_loop_run(reader.loop);
  CHECK(reader.done && reader.error.empty());
  got.insert(got.end(), reader.got.begin(), reader.got.end());
  CHECK(got == sent);
  g_main_loop_unref(reader.loop);
  g_object_unref(stream);
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
}

static void TestReaderWokenByAnotherThread() {
  // The reader waits on its context (nothing to read yet); a writer thread
  // feeds it slowly and ends it; the pollable source wakes the reader.
  auto writer = std::make_shared<SchemeBodyWriter>();
  GMainContext* context = g_main_context_new();
  g_main_context_push_thread_default(context);
  GInputStream* stream = writer->CreateStream();
  std::thread feeder([writer] {
    for (int i = 0; i < 50; i++) {
      uint8_t chunk[1000];
      for (int k = 0; k < 1000; k++)
        chunk[k] = PatternAt(i * 1000 + k);
      writer->Write(chunk, sizeof chunk);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    writer->End();
  });
  AsyncReader reader;
  reader.loop = g_main_loop_new(context, FALSE);
  reader.stream = stream;
  reader.delay_ms = 1;
  reader.Next();
  g_main_loop_run(reader.loop);
  feeder.join();
  CHECK(reader.error.empty());
  CHECK(reader.got.size() == 50000);
  bool same = true;
  for (size_t i = 0; i < reader.got.size(); i++)
    same = same && reader.got[i] == PatternAt(i);
  CHECK(same);
  g_main_loop_unref(reader.loop);
  g_object_unref(stream);
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
}

static void TestCapFailsBothSides() {
  // A cap of 1 MiB: the write that would pass it fails, later writes fail,
  // and the reader's next read errors (the page's read rejects).
  auto writer = std::make_shared<SchemeBodyWriter>(1024 * 1024);
  GMainContext* context = g_main_context_new();
  g_main_context_push_thread_default(context);
  GInputStream* stream = writer->CreateStream();
  std::vector<uint8_t> chunk(256 * 1024, 0x5a);
  int accepted = 0;
  intptr_t r = 0;
  for (int i = 0; i < 8; i++) {
    r = writer->Write(chunk.data(), chunk.size());
    if (r < 0)
      break;
    accepted++;
  }
  CHECK(accepted == 4);
  CHECK(r == -1);
  CHECK(writer->Write(chunk.data(), 1) == -1);
  AsyncReader reader;
  reader.loop = g_main_loop_new(context, FALSE);
  reader.stream = stream;
  reader.Next();
  g_main_loop_run(reader.loop);
  CHECK(reader.error.find("not reading") != std::string::npos);
  g_main_loop_unref(reader.loop);
  g_object_unref(stream);
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
}

static void TestReaderGoneFailsTheWriter() {
  // WebKit drops the stream (fetch aborted): the next write fails.
  auto writer = std::make_shared<SchemeBodyWriter>();
  GInputStream* stream = writer->CreateStream();
  uint8_t b[4] = {1, 2, 3, 4};
  CHECK(writer->Write(b, 4) == 4);
  CHECK(!writer->reader_gone());
  g_object_unref(stream);  // finalize closes it
  CHECK(writer->reader_gone());
  CHECK(writer->Write(b, 4) == -1);

  // An explicit close does the same.
  auto w2 = std::make_shared<SchemeBodyWriter>();
  GInputStream* s2 = w2->CreateStream();
  CHECK(g_input_stream_close(s2, nullptr, nullptr));
  CHECK(w2->Write(b, 4) == -1);
  g_object_unref(s2);
}

// The reader-gone hook behind on_cancel (WebKitGTK): fired once when WebKit
// lets the stream go before the body ended, not after a normal end.
static void TestReaderGoneHook() {
  int gone = 0;
  auto writer = std::make_shared<SchemeBodyWriter>();
  writer->SetReaderGoneHandler([&] { gone++; });
  GInputStream* stream = writer->CreateStream();
  CHECK(g_input_stream_close(stream, nullptr, nullptr));
  g_object_unref(stream);  // finalize: closed already, not reported again
  CHECK(gone == 1);

  int gone2 = 0;
  auto w2 = std::make_shared<SchemeBodyWriter>();
  w2->SetReaderGoneHandler([&] { gone2++; });
  GInputStream* s2 = w2->CreateStream();
  w2->End();  // the response ended: letting it go is no cancel
  g_object_unref(s2);
  CHECK(gone2 == 0);
}

// SchemeCancelGate: one report, none after Finish, and Finish waits for a
// report in progress on another thread (the exchange must outlive it).
static void TestCancelGate() {
  laufey_common::SchemeCancelGate gate;
  int reports = 0;
  gate.Cancel([&] { reports++; });
  gate.Cancel([&] { reports++; });
  CHECK(reports == 1 && gate.cancelled());
  gate.Finish();

  laufey_common::SchemeCancelGate finished;
  finished.Finish();
  finished.Cancel([&] { reports++; });
  CHECK(reports == 1 && !finished.cancelled());

  // Finished from inside the report, on the same thread: no deadlock.
  laufey_common::SchemeCancelGate reentrant;
  reentrant.Cancel([&] { reentrant.Finish(); });
  CHECK(reentrant.cancelled());

  // A report in progress holds Finish until it returns.
  laufey_common::SchemeCancelGate racing;
  std::atomic<bool> in_report{false}, report_done{false};
  std::thread reporter([&] {
    racing.Cancel([&] {
      in_report = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      report_done = true;
    });
  });
  while (!in_report)
    std::this_thread::yield();
  racing.Finish();
  CHECK(report_done);
  reporter.join();
}

static void TestSynchronousReaderAndEof() {
  auto writer = std::make_shared<SchemeBodyWriter>();
  GInputStream* stream = writer->CreateStream();
  std::thread feeder([writer] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint8_t hello[] = {'h', 'i'};
    writer->Write(hello, 2);
    writer->End();
  });
  char buf[16];
  GError* error = nullptr;
  gssize n = g_input_stream_read(stream, buf, sizeof buf, nullptr, &error);
  CHECK(n == 2 && buf[0] == 'h' && buf[1] == 'i');
  n = g_input_stream_read(stream, buf, sizeof buf, nullptr, &error);
  CHECK(n == 0);
  feeder.join();
  CHECK(writer->Write(reinterpret_cast<const uint8_t*>("x"), 1) == -1);
  g_object_unref(stream);

  // A cancelled synchronous read returns at once.
  auto w2 = std::make_shared<SchemeBodyWriter>();
  GInputStream* s2 = w2->CreateStream();
  GCancellable* cancel = g_cancellable_new();
  std::thread canceller([cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    g_cancellable_cancel(cancel);
  });
  n = g_input_stream_read(s2, buf, sizeof buf, cancel, &error);
  CHECK(n == -1 && error && error->code == G_IO_ERROR_CANCELLED);
  g_clear_error(&error);
  canceller.join();
  g_object_unref(cancel);
  g_object_unref(s2);

  // The writer going away ends the body.
  GInputStream* s3;
  {
    SchemeBodyWriter w3;
    s3 = w3.CreateStream();
  }
  n = g_input_stream_read(s3, buf, sizeof buf, nullptr, &error);
  CHECK(n == 0);
  g_object_unref(s3);
}

static int g_criticals = 0;

static void CountCritical(const gchar* domain, GLogLevelFlags, const gchar* msg,
                          gpointer) {
  std::fprintf(stderr, "unexpected critical (%s): %s\n",
               domain ? domain : "", msg ? msg : "");
  g_criticals++;
}

int main() {
  for (const char* domain : {"GLib", "GLib-GObject", "GLib-GIO"}) {
    g_log_set_handler(domain, G_LOG_LEVEL_CRITICAL, CountCritical, nullptr);
  }
  TestWritesNeverBlockAndArriveIntact();
  TestHighWaterMarkTakesNothingUntilRead();
  TestReaderWokenByAnotherThread();
  TestCapFailsBothSides();
  TestReaderGoneFailsTheWriter();
  TestReaderGoneHook();
  TestCancelGate();
  TestSynchronousReaderAndEof();
  CHECK(g_criticals == 0);
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("scheme body stream tests passed\n");
  return 0;
}
