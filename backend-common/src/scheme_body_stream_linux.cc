// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// See laufey_scheme_body_stream.h.

#include "laufey_scheme_body_stream.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace laufey_common {

class SchemeBodyQueue {
 public:
  explicit SchemeBodyQueue(size_t max_queued) : max_queued_(max_queued) {}
  ~SchemeBodyQueue() {
    if (context_)
      g_main_context_unref(context_);
  }

  void SetContext(GMainContext* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_)
      g_main_context_unref(context_);
    context_ = context ? g_main_context_ref(context) : nullptr;
  }

  intptr_t Write(const uint8_t* data, size_t len) {
    intptr_t result;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_ || failed_ || ended_)
        return -1;
      if (len == 0)
        return 0;
      if (queued_ + len > max_queued_) {
        // The reader isn't keeping up: fail the response rather than hold
        // an unbounded amount (the page's read rejects).
        failed_ = true;
        error_ = "laufey: the page is not reading this response";
        chunks_.clear();
        front_ = 0;
        queued_ = 0;
        result = -1;
      } else {
        chunks_.emplace_back(data, data + len);
        queued_ += len;
        result = static_cast<intptr_t>(len);
      }
    }
    Wake();
    return result;
  }

  void End() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (ended_ || failed_)
        return;
      ended_ = true;
    }
    Wake();
  }

  void Fail(const char* message) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (ended_ || failed_)
        return;
      failed_ = true;
      error_ = message ? message : "laufey: the response failed";
      chunks_.clear();
      front_ = 0;
      queued_ = 0;
    }
    Wake();
  }

  // The reader is gone (WebKit closed / finalized the stream).
  void Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    chunks_.clear();
    front_ = 0;
    queued_ = 0;
    cv_.notify_all();
  }

  bool Readable() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadableLocked();
  }

  // Non-blocking read: bytes (>0), 0 at EOF, -1 with `error` set
  // (G_IO_ERROR_WOULD_BLOCK when nothing is there yet).
  gssize Read(void* buffer, gsize count, GError** error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadLocked(buffer, count, error);
  }

  // Blocking read for synchronous readers: waits until something is
  // readable or `cancellable` is cancelled.
  gssize ReadBlocking(void* buffer, gsize count, GCancellable* cancellable,
                      GError** error) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!ReadableLocked() && !closed_) {
      if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return -1;
      cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    return ReadLocked(buffer, count, error);
  }

  size_t queued() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_;
  }
  bool closed() {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

 private:
  bool ReadableLocked() const {
    return queued_ > 0 || ended_ || failed_;
  }

  gssize ReadLocked(void* buffer, gsize count, GError** error) {
    if (failed_) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, error_.c_str());
      return -1;
    }
    if (closed_) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                          "stream is closed");
      return -1;
    }
    if (queued_ == 0) {
      if (ended_)
        return 0;
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK,
                          "no data yet");
      return -1;
    }
    gsize copied = 0;
    auto* out = static_cast<uint8_t*>(buffer);
    while (copied < count && !chunks_.empty()) {
      std::vector<uint8_t>& front = chunks_.front();
      size_t n = std::min<size_t>(count - copied, front.size() - front_);
      std::memcpy(out + copied, front.data() + front_, n);
      copied += n;
      front_ += n;
      if (front_ == front.size()) {
        chunks_.pop_front();
        front_ = 0;
      }
    }
    queued_ -= copied;
    return static_cast<gssize>(copied);
  }

  // Readability changed: wake a blocked synchronous reader and the reader's
  // main context (its pollable source re-checks Readable()).
  void Wake() {
    GMainContext* context = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cv_.notify_all();
      if (context_)
        context = g_main_context_ref(context_);
    }
    if (context) {
      g_main_context_wakeup(context);
      g_main_context_unref(context);
    }
  }

  const size_t max_queued_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::vector<uint8_t>> chunks_;
  size_t front_ = 0;   // bytes of chunks_.front() already read
  size_t queued_ = 0;  // bytes in chunks_ not read yet
  bool ended_ = false;
  bool failed_ = false;
  bool closed_ = false;
  std::string error_;
  GMainContext* context_ = nullptr;
};

}  // namespace laufey_common

// --- The GInputStream
// ----------------------------------------------------------

using laufey_common::SchemeBodyQueue;

namespace {

struct QueueHolder {
  std::shared_ptr<SchemeBodyQueue> queue;
};

}  // namespace

G_BEGIN_DECLS

#define LAUFEY_TYPE_SCHEME_BODY_STREAM (laufey_scheme_body_stream_get_type())
G_DECLARE_FINAL_TYPE(LaufeySchemeBodyStream, laufey_scheme_body_stream, LAUFEY,
                     SCHEME_BODY_STREAM, GInputStream)

struct _LaufeySchemeBodyStream {
  GInputStream parent_instance;
  QueueHolder* holder;
};

static void laufey_scheme_body_stream_pollable_init(
    GPollableInputStreamInterface* iface);

G_DEFINE_TYPE_WITH_CODE(
    LaufeySchemeBodyStream, laufey_scheme_body_stream, G_TYPE_INPUT_STREAM,
    G_IMPLEMENT_INTERFACE(G_TYPE_POLLABLE_INPUT_STREAM,
                          laufey_scheme_body_stream_pollable_init))

G_END_DECLS

static SchemeBodyQueue* QueueOf(gpointer stream) {
  return LAUFEY_SCHEME_BODY_STREAM(stream)->holder->queue.get();
}

static gssize StreamRead(GInputStream* stream, void* buffer, gsize count,
                         GCancellable* cancellable, GError** error) {
  return QueueOf(stream)->ReadBlocking(buffer, count, cancellable, error);
}

static gboolean StreamClose(GInputStream* stream, GCancellable*, GError**) {
  QueueOf(stream)->Close();
  return TRUE;
}

static void StreamFinalize(GObject* object) {
  auto* self = LAUFEY_SCHEME_BODY_STREAM(object);
  if (self->holder) {
    self->holder->queue->Close();
    delete self->holder;
    self->holder = nullptr;
  }
  G_OBJECT_CLASS(laufey_scheme_body_stream_parent_class)->finalize(object);
}

static void laufey_scheme_body_stream_class_init(
    LaufeySchemeBodyStreamClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = StreamFinalize;
  G_INPUT_STREAM_CLASS(klass)->read_fn = StreamRead;
  G_INPUT_STREAM_CLASS(klass)->close_fn = StreamClose;
}

static void laufey_scheme_body_stream_init(LaufeySchemeBodyStream* self) {
  self->holder = nullptr;
}

// A source that is ready while the queue is readable; the writer wakes the
// context whenever that changes.
struct ReadySource {
  GSource source;
  QueueHolder* holder;
};

static gboolean ReadyPrepare(GSource* source, gint* timeout) {
  *timeout = -1;
  return reinterpret_cast<ReadySource*>(source)->holder->queue->Readable();
}

static gboolean ReadyCheck(GSource* source) {
  return reinterpret_cast<ReadySource*>(source)->holder->queue->Readable();
}

static gboolean ReadyDispatch(GSource* source, GSourceFunc callback,
                              gpointer user_data) {
  // The dummy callback the pollable parent installs; the parent dispatches.
  return callback ? callback(user_data) : G_SOURCE_CONTINUE;
}

static void ReadyFinalize(GSource* source) {
  auto* ready = reinterpret_cast<ReadySource*>(source);
  delete ready->holder;
  ready->holder = nullptr;
}

static GSourceFuncs kReadySourceFuncs = {
    ReadyPrepare, ReadyCheck, ReadyDispatch, ReadyFinalize, nullptr, nullptr,
};

static gboolean PollableCanPoll(GPollableInputStream*) {
  return TRUE;
}

static gboolean PollableIsReadable(GPollableInputStream* stream) {
  return QueueOf(stream)->Readable();
}

static GSource* PollableCreateSource(GPollableInputStream* stream,
                                     GCancellable* cancellable) {
  GSource* ready = g_source_new(&kReadySourceFuncs, sizeof(ReadySource));
  reinterpret_cast<ReadySource*>(ready)->holder =
      new QueueHolder{LAUFEY_SCHEME_BODY_STREAM(stream)->holder->queue};
  GSource* source =
      g_pollable_source_new_full(stream, ready, cancellable);  // refs `ready`
  g_source_unref(ready);
  return source;
}

static gssize PollableReadNonblocking(GPollableInputStream* stream,
                                      void* buffer, gsize count,
                                      GError** error) {
  return QueueOf(stream)->Read(buffer, count, error);
}

static void laufey_scheme_body_stream_pollable_init(
    GPollableInputStreamInterface* iface) {
  iface->can_poll = PollableCanPoll;
  iface->is_readable = PollableIsReadable;
  iface->create_source = PollableCreateSource;
  iface->read_nonblocking = PollableReadNonblocking;
}

namespace laufey_common {

SchemeBodyWriter::SchemeBodyWriter(size_t max_queued)
    : queue_(std::make_shared<SchemeBodyQueue>(max_queued)) {}

SchemeBodyWriter::~SchemeBodyWriter() {
  queue_->End();
}

GInputStream* SchemeBodyWriter::CreateStream() {
  GMainContext* context = g_main_context_ref_thread_default();
  queue_->SetContext(context);
  g_main_context_unref(context);
  auto* stream = LAUFEY_SCHEME_BODY_STREAM(
      g_object_new(LAUFEY_TYPE_SCHEME_BODY_STREAM, nullptr));
  stream->holder = new QueueHolder{queue_};
  return G_INPUT_STREAM(stream);
}

intptr_t SchemeBodyWriter::Write(const uint8_t* data, size_t len) {
  return queue_->Write(data, len);
}

void SchemeBodyWriter::End() {
  queue_->End();
}

void SchemeBodyWriter::Fail(const char* message) {
  queue_->Fail(message);
}

size_t SchemeBodyWriter::queued() const {
  return queue_->queued();
}

bool SchemeBodyWriter::reader_gone() const {
  return queue_->closed();
}

}  // namespace laufey_common
