// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for laufey_ui_tasks.h (dispatch_ui_task's delivery guarantee,
// over a fake UI queue) and the platform-independent half of auth sessions
// (laufey_auth_session.h: validation, the one-session slot, exactly-once
// results, cancellation). Plain asserts, no framework: run via
// `ctest --test-dir webview/build` (or cef/build). No OS API is touched.

#include "laufey_auth_session.h"
#include "laufey_ui_tasks.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using laufey_common::AuthSession;
using laufey_common::AuthSessionBegin;
using laufey_common::AuthSessionBusyForTesting;
using laufey_common::AuthSessionCallback;
using laufey_common::AuthSessionCancelCurrent;
using laufey_common::AuthSessionWindowClosing;
using laufey_common::ParseAuthSessionCallback;
using laufey_common::UiTaskDispatcher;
using laufey_common::ValidateAuthSessionUrl;

static int g_failures = 0;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                  \
      g_failures++;                                                         \
    }                                                                       \
  } while (0)

// --- UI tasks ----------------------------------------------------------------

// A fake UI thread: a queue drained by a thread of its own until stopped,
// like an event loop that may end with tasks still queued.
class FakeLoop {
 public:
  FakeLoop() : thread_([this] { Run(); }) {}
  ~FakeLoop() {
    Stop();
    thread_.join();
  }
  // Post a platform task; false once the loop refuses posts.
  bool Post(void (*task)(void*), void* data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (refuse_)
      return false;
    queue_.push_back({task, data});
    cv_.notify_all();
    return true;
  }
  // Run `fn` on the loop thread and wait for it.
  void Sync(std::function<void()> fn) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    auto* call = new std::function<void()>([&] {
      fn();
      std::lock_guard<std::mutex> lock(m);
      done = true;
      cv.notify_all();
    });
    Post(
        [](void* d) {
          auto* f = static_cast<std::function<void()>*>(d);
          (*f)();
          delete f;
        },
        call);
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return done; });
  }
  // Hold the loop: queued tasks wait until Resume.
  void Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = true;
  }
  void Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = false;
    cv_.notify_all();
  }
  // End the loop with whatever is queued left unrun (like a platform queue
  // nothing drains after the loop returned); later posts are still accepted
  // and never run, as GCD / g_idle_add would.
  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cv_.notify_all();
  }
  void RefusePosts() {
    std::lock_guard<std::mutex> lock(mutex_);
    refuse_ = true;
  }
  // Leaked platform tasks are freed here (only their Ref; never run).
  size_t leaked() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }
  std::thread::id id() const {
    return thread_.get_id();
  }

 private:
  struct Item {
    void (*task)(void*);
    void* data;
  };
  void Run() {
    for (;;) {
      Item item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock,
                 [&] { return stopped_ || (!paused_ && !queue_.empty()); });
        if (stopped_)
          return;
        item = queue_.front();
        queue_.pop_front();
      }
      item.task(item.data);
    }
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  bool paused_ = false;
  bool stopped_ = false;
  bool refuse_ = false;
  std::thread thread_;
};

struct TaskProbe {
  std::atomic<int> calls{0};
  std::atomic<bool> ran{false};
  std::thread::id thread;
  std::mutex m;
  std::condition_variable cv;
  void Wait() {
    std::unique_lock<std::mutex> lock(m);
    cv.wait_for(lock, std::chrono::seconds(5), [&] { return calls > 0; });
  }
};

static void ProbeTask(void* data, bool ran) {
  auto* p = static_cast<TaskProbe*>(data);
  std::lock_guard<std::mutex> lock(p->m);
  p->thread = std::this_thread::get_id();
  p->ran = ran;
  p->calls++;
  p->cv.notify_all();
}

static void TestUiTasksRunOnTheUiThread() {
  FakeLoop loop;
  UiTaskDispatcher d;
  CHECK(!d.IsUiThread());
  loop.Sync([&] {
    d.Bind([&](void (*t)(void*), void* data) { return loop.Post(t, data); });
  });
  CHECK(!d.IsUiThread());  // the test thread isn't the UI thread
  bool on_ui = false;
  loop.Sync([&] { on_ui = d.IsUiThread(); });
  CHECK(on_ui);

  TaskProbe p;
  d.Dispatch(ProbeTask, &p);
  p.Wait();
  CHECK(p.calls == 1);
  CHECK(p.ran);
  CHECK(p.thread == loop.id());
  CHECK(d.pending_count() == 0);

  // Dispatched from the UI thread itself: queued, not run inline.
  TaskProbe q;
  bool ran_inline = true;
  loop.Sync([&] {
    d.Dispatch(ProbeTask, &q);
    ran_inline = q.calls > 0;
  });
  q.Wait();
  CHECK(!ran_inline);
  CHECK(q.calls == 1 && q.ran);
  d.Dispatch(nullptr, nullptr);  // a NULL task is a no-op
  d.Close();
}

static void TestUiTasksDispatchedBeforeBind() {
  FakeLoop loop;
  UiTaskDispatcher d;
  TaskProbe p;
  d.Dispatch(ProbeTask, &p);  // the loop isn't bound yet: held
  CHECK(p.calls == 0);
  CHECK(d.pending_count() == 1);
  loop.Sync([&] {
    d.Bind([&](void (*t)(void*), void* data) { return loop.Post(t, data); });
  });
  p.Wait();
  CHECK(p.calls == 1 && p.ran && p.thread == loop.id());
  d.Close();
}

static void TestUiTasksCancelledWhenTheLoopEnds() {
  FakeLoop loop;
  UiTaskDispatcher d;
  loop.Sync([&] {
    d.Bind([&](void (*t)(void*), void* data) { return loop.Post(t, data); });
  });
  // Queued behind a paused loop, then the loop ends: Close answers them on
  // the closing thread with ran = false, exactly once.
  loop.Pause();
  TaskProbe a, b;
  d.Dispatch(ProbeTask, &a);
  d.Dispatch(ProbeTask, &b);
  CHECK(d.pending_count() == 2);
  loop.Stop();
  d.Close();
  CHECK(a.calls == 1 && !a.ran && a.thread == std::this_thread::get_id());
  CHECK(b.calls == 1 && !b.ran);
  CHECK(d.pending_count() == 0);
  // After Close: answered at once, never queued.
  TaskProbe c;
  d.Dispatch(ProbeTask, &c);
  CHECK(c.calls == 1 && !c.ran);
  d.Close();  // idempotent
  CHECK(c.calls == 1);
}

static void TestUiTasksQueuedPlatformTaskAfterClose() {
  // A platform task that still runs after Close (the queue kept draining)
  // finds nothing: the task was already answered with ran = false.
  FakeLoop loop;
  UiTaskDispatcher d;
  loop.Sync([&] {
    d.Bind([&](void (*t)(void*), void* data) { return loop.Post(t, data); });
  });
  loop.Pause();
  TaskProbe a;
  d.Dispatch(ProbeTask, &a);
  d.Close();
  CHECK(a.calls == 1 && !a.ran);
  loop.Resume();
  loop.Sync([] {});  // the stale platform task has run by now
  CHECK(a.calls == 1);
}

static void TestUiTasksRefusedPost() {
  FakeLoop loop;
  UiTaskDispatcher d;
  loop.Sync([&] {
    d.Bind([&](void (*t)(void*), void* data) { return loop.Post(t, data); });
  });
  loop.RefusePosts();  // e.g. CefPostTask after CEF shut down
  TaskProbe a;
  d.Dispatch(ProbeTask, &a);
  CHECK(a.calls == 1 && !a.ran);
  CHECK(d.pending_count() == 0);
  d.Close();
}

static void TestUiTasksConcurrentDispatchAndClose() {
  // Every task is answered exactly once however Dispatch, the loop and
  // Close interleave.
  for (int round = 0; round < 20; round++) {
    FakeLoop loop;
    UiTaskDispatcher d;
    loop.Sync([&] {
      d.Bind([&](void (*t)(void*), void* data) { return loop.Post(t, data); });
    });
    constexpr int kThreads = 4, kPer = 200;
    std::vector<TaskProbe> probes(kThreads * kPer);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < kPer; i++)
          d.Dispatch(ProbeTask, &probes[t * kPer + i]);
      });
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50 * round));
    loop.Stop();
    d.Close();
    for (auto& th : threads)
      th.join();
    for (auto& p : probes) {
      // ProbeTask notifies under the probe's lock: once the lock is free it
      // is done with the probe, which may then be destroyed (ThreadSanitizer
      // otherwise sees the vector's teardown race a late notify_all).
      std::lock_guard<std::mutex> lock(p.m);
      CHECK(p.calls == 1);
    }
  }
}

// --- Auth session validation -------------------------------------------------

static void TestAuthSessionUrl() {
  std::string err;
  CHECK(
      ValidateAuthSessionUrl("https://example.com/authorize?a=1&b=%20", &err));
  CHECK(ValidateAuthSessionUrl("HTTP://127.0.0.1:8080/start", &err));
  CHECK(ValidateAuthSessionUrl("https://user@example.com", &err));
  CHECK(!ValidateAuthSessionUrl(nullptr, &err));
  CHECK(!ValidateAuthSessionUrl("", &err));
  CHECK(!ValidateAuthSessionUrl("myapp://x", &err));
  CHECK(!ValidateAuthSessionUrl("javascript:alert(1)", &err));
  CHECK(!ValidateAuthSessionUrl("https://", &err));
  CHECK(!ValidateAuthSessionUrl("https:///path", &err));
  CHECK(!ValidateAuthSessionUrl("https://:443/", &err));
  CHECK(!ValidateAuthSessionUrl("https://exa mple.com/", &err));
  CHECK(!ValidateAuthSessionUrl("https://example.com/\n", &err));
  CHECK(!ValidateAuthSessionUrl("https://example.com/\xc3\xa9", &err));
  std::string long_url = "https://example.com/" + std::string(9000, 'a');
  CHECK(!ValidateAuthSessionUrl(long_url.c_str(), &err));
  CHECK(err.find("long") != std::string::npos);
  // Error messages never echo the value.
  CHECK(!ValidateAuthSessionUrl("ftp://secret-token", &err));
  CHECK(err.find("secret") == std::string::npos);
}

static void TestAuthSessionCallback() {
  AuthSessionCallback cb;
  std::string err;
  CHECK(ParseAuthSessionCallback("myapp", &cb, &err));
  CHECK(!cb.https && cb.scheme == "myapp");
  CHECK(ParseAuthSessionCallback("Com.Example.App+x-1", &cb, &err));
  CHECK(cb.scheme == "com.example.app+x-1");
  CHECK(ParseAuthSessionCallback("https://Example.com/auth/done", &cb, &err));
  CHECK(cb.https && cb.host == "example.com" && cb.path == "/auth/done");
  CHECK(ParseAuthSessionCallback("https://example.com", &cb, &err));
  CHECK(cb.https && cb.path == "/");
  CHECK(!ParseAuthSessionCallback(nullptr, &cb, &err));
  CHECK(!ParseAuthSessionCallback("", &cb, &err));
  CHECK(!ParseAuthSessionCallback("1app", &cb, &err));
  CHECK(!ParseAuthSessionCallback("my app", &cb, &err));
  CHECK(!ParseAuthSessionCallback("myapp://cb", &cb, &err));
  CHECK(!ParseAuthSessionCallback("http", &cb, &err));
  CHECK(!ParseAuthSessionCallback("HTTPS", &cb, &err));
  CHECK(!ParseAuthSessionCallback("javascript", &cb, &err));
  CHECK(!ParseAuthSessionCallback("file", &cb, &err));
  CHECK(!ParseAuthSessionCallback(std::string(65, 'a').c_str(), &cb, &err));
  CHECK(!ParseAuthSessionCallback("https://example.com:8443/cb", &cb, &err));
  CHECK(!ParseAuthSessionCallback("https://u@example.com/cb", &cb, &err));
  CHECK(!ParseAuthSessionCallback("https://example.com/cb?x=1", &cb, &err));
  CHECK(!ParseAuthSessionCallback("https://example.com/cb#f", &cb, &err));
  CHECK(!ParseAuthSessionCallback("https://-bad.com/cb", &cb, &err));
  CHECK(!ParseAuthSessionCallback("https:///cb", &cb, &err));
}

// --- Auth session lifecycle --------------------------------------------------

struct Result {
  std::mutex m;
  std::vector<std::pair<int32_t, std::string>> got;
  size_t count() {
    std::lock_guard<std::mutex> lock(m);
    return got.size();
  }
  int32_t status(size_t i = 0) {
    std::lock_guard<std::mutex> lock(m);
    return got.at(i).first;
  }
  std::string value(size_t i = 0) {
    std::lock_guard<std::mutex> lock(m);
    return got.at(i).second;
  }
};

static void OnResult(void* user_data, int32_t status, const char* value) {
  auto* r = static_cast<Result*>(user_data);
  std::lock_guard<std::mutex> lock(r->m);
  r->got.emplace_back(status, value ? value : "<null>");
}

constexpr uint32_t kAllCaps = LAUFEY_AUTH_SESSION_CAP_SUPPORTED |
                              LAUFEY_AUTH_SESSION_CAP_EPHEMERAL |
                              LAUFEY_AUTH_SESSION_CAP_HTTPS_CALLBACK;

static void TestAuthSessionRefusals() {
  // No capability: NOT_SUPPORTED synchronously, before the arguments are
  // looked at (Windows, Linux).
  {
    Result r;
    CHECK(!AuthSessionBegin(0, nullptr, nullptr, 0, OnResult, &r));
    CHECK(r.count() == 1 && r.status() == LAUFEY_AUTH_SESSION_NOT_SUPPORTED);
    CHECK(r.value().find("RFC 8252") != std::string::npos);
  }
  struct Case {
    uint32_t caps;
    const char* url;
    const char* cb;
    uint32_t flags;
    int32_t want;
  } cases[] = {
      {kAllCaps, "nope", "myapp", 0, LAUFEY_AUTH_SESSION_INVALID},
      {kAllCaps, "https://a.com", "http", 0, LAUFEY_AUTH_SESSION_INVALID},
      {kAllCaps, "https://a.com", "myapp", 0x80, LAUFEY_AUTH_SESSION_INVALID},
      {LAUFEY_AUTH_SESSION_CAP_SUPPORTED | LAUFEY_AUTH_SESSION_CAP_EPHEMERAL,
       "https://a.com", "https://a.com/cb", 0,
       LAUFEY_AUTH_SESSION_NOT_SUPPORTED},
      {LAUFEY_AUTH_SESSION_CAP_SUPPORTED, "https://a.com", "myapp",
       LAUFEY_AUTH_SESSION_EPHEMERAL, LAUFEY_AUTH_SESSION_NOT_SUPPORTED},
  };
  for (const Case& c : cases) {
    Result r;
    CHECK(!AuthSessionBegin(c.caps, c.url, c.cb, c.flags, OnResult, &r));
    CHECK(r.count() == 1 && r.status() == c.want);
  }
  // A NULL result callback: a no-op.
  CHECK(!AuthSessionBegin(kAllCaps, "https://a.com", "myapp", 0, nullptr,
                          nullptr));
  CHECK(!AuthSessionBusyForTesting());
}

static void TestAuthSessionOneAtATime() {
  Result r1, r2, r3;
  auto s1 = AuthSessionBegin(kAllCaps, "https://a.com/auth", "myapp",
                             LAUFEY_AUTH_SESSION_EPHEMERAL, OnResult, &r1);
  CHECK(s1 != nullptr);
  CHECK(s1->ephemeral());
  CHECK(s1->url() == "https://a.com/auth");
  CHECK(AuthSessionBusyForTesting());
  CHECK(r1.count() == 0);
  // A second session meanwhile is BUSY, and doesn't disturb the first.
  CHECK(
      !AuthSessionBegin(kAllCaps, "https://a.com", "myapp", 0, OnResult, &r2));
  CHECK(r2.count() == 1 && r2.status() == LAUFEY_AUTH_SESSION_BUSY);
  CHECK(AuthSessionBusyForTesting());
  // The result frees the slot before it is delivered, exactly once.
  s1->Finish(LAUFEY_AUTH_SESSION_OK, "myapp://cb?code=1");
  s1->Finish(LAUFEY_AUTH_SESSION_FAILED, "late");
  CHECK(r1.count() == 1 && r1.status() == LAUFEY_AUTH_SESSION_OK &&
        r1.value() == "myapp://cb?code=1");
  CHECK(!AuthSessionBusyForTesting());
  auto s3 =
      AuthSessionBegin(kAllCaps, "https://a.com", "myapp", 0, OnResult, &r3);
  CHECK(s3 != nullptr);
  s3->Finish(LAUFEY_AUTH_SESSION_CANCELLED, "x");
  CHECK(r3.count() == 1);
}

static void TestAuthSessionCancel() {
  // Cancel: the canceller runs, the slot is freed, CANCELLED is delivered
  // once, and the OS's own late report is dropped.
  Result r;
  auto s =
      AuthSessionBegin(kAllCaps, "https://a.com", "myapp", 0, OnResult, &r);
  CHECK(s != nullptr);
  std::atomic<int> cancels{0};
  s->SetCanceller([&] { cancels++; });
  CHECK(AuthSessionCancelCurrent("test"));
  CHECK(cancels == 1);
  CHECK(!AuthSessionBusyForTesting());
  CHECK(r.count() == 1 && r.status() == LAUFEY_AUTH_SESSION_CANCELLED &&
        r.value() == "test");
  s->Finish(LAUFEY_AUTH_SESSION_CANCELLED, "the OS reports it too");
  s->Cancel("again");
  CHECK(r.count() == 1);
  CHECK(cancels == 1);
  CHECK(!AuthSessionCancelCurrent("none running"));
  // A canceller installed after the cancel runs at once.
  std::atomic<int> late{0};
  s->SetCanceller([&] { late++; });
  CHECK(late == 1);
}

static void TestAuthSessionWindowClosing() {
  int window_a = 0, window_b = 0;
  Result r;
  auto s =
      AuthSessionBegin(kAllCaps, "https://a.com", "myapp", 0, OnResult, &r);
  s->SetWindowKey(&window_a);
  AuthSessionWindowClosing(&window_b);  // another window: no effect
  AuthSessionWindowClosing(nullptr);
  CHECK(r.count() == 0);
  AuthSessionWindowClosing(&window_a);
  CHECK(r.count() == 1 && r.status() == LAUFEY_AUTH_SESSION_CANCELLED);
  CHECK(!AuthSessionBusyForTesting());
}

static void TestAuthSessionDroppedWithoutResult() {
  // A session the OS side dropped without a result still answers (FAILED)
  // and frees the slot.
  Result r;
  {
    auto s =
        AuthSessionBegin(kAllCaps, "https://a.com", "myapp", 0, OnResult, &r);
    CHECK(s != nullptr);
  }
  CHECK(r.count() == 1 && r.status() == LAUFEY_AUTH_SESSION_FAILED);
  CHECK(!AuthSessionBusyForTesting());
}

static void TestAuthSessionRaces() {
  // Finish (OS thread) and Cancel (window close) racing: exactly one result.
  for (int i = 0; i < 200; i++) {
    Result r;
    auto s =
        AuthSessionBegin(kAllCaps, "https://a.com", "myapp", 0, OnResult, &r);
    CHECK(s != nullptr);
    std::thread a([&] { s->Finish(LAUFEY_AUTH_SESSION_OK, "myapp://x"); });
    std::thread b([&] { AuthSessionCancelCurrent("race"); });
    a.join();
    b.join();
    CHECK(r.count() == 1);
    CHECK(!AuthSessionBusyForTesting());
  }
}

// The synchronous hop behind every getter the runtime calls (cef_invoke_sync,
// gtk_invoke_sync, GtkRunSync, the WebView2 / macOS getters): runs on the UI
// thread and returns true; a caller still waiting when the loop ends, or
// calling after it ended, is released with false instead of waiting forever
// (the runtime's shutdown waits for that caller's thread).
static void TestRunOnUiThreadAndWait() {
  FakeLoop loop;
  UiTaskDispatcher d;
  loop.Sync([&] {
    d.Bind([&](void (*t)(void*), void* data) { return loop.Post(t, data); });
  });
  std::thread::id ran_on;
  auto record = [&] { ran_on = std::this_thread::get_id(); };
  CHECK(laufey_common::RunOnUiThreadAndWait(record, nullptr, d));
  CHECK(ran_on == loop.id());

  // Waiting behind a loop that ends: released, `fn` never runs.
  loop.Pause();
  std::atomic<bool> released{false};
  std::atomic<bool> ran{false};
  std::atomic<bool> result{true};
  std::thread waiter([&] {
    auto mark = [&] { ran = true; };
    result = laufey_common::RunOnUiThreadAndWait(mark, nullptr, d);
    released = true;
  });
  while (d.pending_count() == 0)
    std::this_thread::yield();
  CHECK(!released);
  loop.Stop();
  d.Close();
  waiter.join();
  CHECK(released && !result && !ran);

  // After the loop ended: answered at once.
  bool late = false;
  auto set_late = [&] { late = true; };
  CHECK(!laufey_common::RunOnUiThreadAndWait(set_late, nullptr, d));
  CHECK(!late);
}

// The same over a caller's own transport (the macOS main queue, GLib's
// default context): works before Bind, and a waiter is still released when
// the loop ends.
static void TestRunOnUiThreadAndWaitVia() {
  FakeLoop loop;
  UiTaskDispatcher d;  // never bound
  auto via = [&](void (*t)(void*), void* data) { return loop.Post(t, data); };
  std::thread::id ran_on;
  auto record = [&] { ran_on = std::this_thread::get_id(); };
  CHECK(laufey_common::RunOnUiThreadAndWait(record, via, d));
  CHECK(ran_on == loop.id());

  loop.Pause();
  std::atomic<bool> result{true};
  std::atomic<bool> ran{false};
  std::thread waiter([&] {
    auto mark = [&] { ran = true; };
    result = laufey_common::RunOnUiThreadAndWait(mark, via, d);
  });
  while (d.pending_count() == 0)
    std::this_thread::yield();
  loop.Stop();
  d.Close();
  waiter.join();
  CHECK(!result && !ran);
  CHECK(!laufey_common::RunOnUiThreadAndWait(record, via, d));
}

static void TestRunOnUiThreadAndWaitRefusedPost() {
  // CefPostTask refuses once CEF has shut down: the caller is released.
  FakeLoop loop;
  UiTaskDispatcher d;
  loop.Sync([&] {
    d.Bind([&](void (*t)(void*), void* data) { return loop.Post(t, data); });
  });
  loop.RefusePosts();
  bool ran = false;
  auto set = [&] { ran = true; };
  CHECK(!laufey_common::RunOnUiThreadAndWait(set, nullptr, d));
  CHECK(!ran);
  d.Close();
}

int main() {
  TestRunOnUiThreadAndWait();
  TestRunOnUiThreadAndWaitRefusedPost();
  TestRunOnUiThreadAndWaitVia();
  TestUiTasksRunOnTheUiThread();
  TestUiTasksDispatchedBeforeBind();
  TestUiTasksCancelledWhenTheLoopEnds();
  TestUiTasksQueuedPlatformTaskAfterClose();
  TestUiTasksRefusedPost();
  TestUiTasksConcurrentDispatchAndClose();
  TestAuthSessionUrl();
  TestAuthSessionCallback();
  TestAuthSessionRefusals();
  TestAuthSessionOneAtATime();
  TestAuthSessionCancel();
  TestAuthSessionWindowClosing();
  TestAuthSessionDroppedWithoutResult();
  TestAuthSessionRaces();
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("auth / main-thread tests passed\n");
  return 0;
}
