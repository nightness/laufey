// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for laufey_scheme_body_stream.h (Linux): the WebKitGTK
// custom-scheme response body. A writer thread never blocks however much
// is unread; a reader on a GMainContext reads it asynchronously
// (g_input_stream_read_bytes_async, as WebKit does) byte for byte; a body
// past the cap fails both sides; a reader that went away fails the writer;
// a synchronous reader works too. Plain asserts, no WebKit.

#include "laufey_scheme_body_stream.h"

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
  // 4 MiB written with nobody reading: every write returns at once.
  constexpr size_t kLen = 4 * 1024 * 1024 + 7;
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

int main() {
  TestWritesNeverBlockAndArriveIntact();
  TestReaderWokenByAnotherThread();
  TestCapFailsBothSides();
  TestReaderGoneFailsTheWriter();
  TestSynchronousReaderAndEof();
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("scheme body stream tests passed\n");
  return 0;
}
