// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// UI-thread tasks with a delivery guarantee: the shared half behind the C
// ABI's dispatch_ui_task / is_ui_thread (API >= 42).
//
// post_ui_task hands a task to the platform's queue (a GCD block, a window
// message, a GLib idle source, a CEF task) and forgets it. Once the event
// loop has ended nothing drains that queue, so a runtime thread waiting for
// such a task would wait forever, and the backend's shutdown, which waits
// for the runtime thread, with it. The dispatcher keeps every task it posted
// until it runs; when the backend's loop ends (Close), every task that has
// not run yet is called with `ran` false instead, and later dispatches are
// answered at once. A queued platform task that still runs after Close finds
// nothing to do: it carries an id, not the task.
//
// Header-only (no backend-common link needed, so iOS can use it too).

#ifndef LAUFEY_UI_TASKS_H_
#define LAUFEY_UI_TASKS_H_

#include <laufey.h>
#include "laufey_sync_call.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__) && defined(__OBJC__)
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

namespace laufey_common {

class UiTaskDispatcher {
 public:
  // Posts `task(data)` to the UI thread's queue; false if it could not be
  // queued (the task will never run).
  using PostFn = std::function<bool(void (*task)(void*), void* data)>;

  UiTaskDispatcher() = default;
  UiTaskDispatcher(const UiTaskDispatcher&) = delete;
  UiTaskDispatcher& operator=(const UiTaskDispatcher&) = delete;

  // On the UI thread, before its loop runs: the thread is_ui_thread reports
  // and the platform queue tasks go to. Tasks dispatched before Bind are
  // held and posted by Bind.
  void Bind(PostFn post) {
    std::vector<uint64_t> held;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ui_thread_ = std::this_thread::get_id();
      bound_ = true;
      post_ = std::move(post);
      held.swap(unposted_);
    }
    for (uint64_t id : held)
      Post(id);
  }

  // True on the thread Bind ran on.
  bool IsUiThread() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bound_ && ui_thread_ == std::this_thread::get_id();
  }

  // See dispatch_ui_task in laufey.h. Any thread.
  void Dispatch(laufey_ui_task_fn task, void* data) {
    if (!task)
      return;
    uint64_t id;
    bool post_now;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        id = 0;
        post_now = false;
      } else {
        id = ++next_id_;
        pending_.emplace(id, Pending{task, data});
        post_now = bound_;
        if (!post_now)
          unposted_.push_back(id);
      }
    }
    if (id == 0) {
      task(data, false);
      return;
    }
    if (post_now)
      Post(id);
  }

  // Like Dispatch, but queued through `post` instead of the bound queue: for
  // a caller that keeps its own transport (GCD's main queue, which AppKit
  // also drains inside its modal loops; GLib's default context). Works
  // before Bind. The delivery guarantee is Dispatch's: the task runs once,
  // or Close answers it with `ran` false.
  void DispatchVia(const PostFn& post, laufey_ui_task_fn task, void* data) {
    if (!task)
      return;
    uint64_t id = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!closed_) {
        id = ++next_id_;
        pending_.emplace(id, Pending{task, data});
      }
    }
    if (id == 0) {
      task(data, false);
      return;
    }
    auto* ref = new Ref{this, id};
    if (!post || !post(&UiTaskDispatcher::Run, ref)) {
      delete ref;
      Cancel(id);
    }
  }

  // The backend's loop has ended (or the backend runs without one): every
  // task that has not run is called with `ran` false, on this thread, and
  // later dispatches are answered at once. Idempotent.
  void Close() {
    std::unordered_map<uint64_t, Pending> cancelled;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
      cancelled.swap(pending_);
      unposted_.clear();
      post_ = nullptr;
    }
    for (auto& [id, p] : cancelled)
      p.task(p.data, false);
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  // Tasks dispatched and not yet run or cancelled (tests).
  size_t pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
  }

  // The process-wide dispatcher the backends use.
  static UiTaskDispatcher& Get() {
    static UiTaskDispatcher* instance = new UiTaskDispatcher();  // never freed
    return *instance;
  }

 private:
  struct Pending {
    laufey_ui_task_fn task;
    void* data;
  };

  struct Ref {
    UiTaskDispatcher* self;
    uint64_t id;
  };

  void Post(uint64_t id) {
    PostFn post;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      post = post_;
    }
    auto* ref = new Ref{this, id};
    if (!post || !post(&UiTaskDispatcher::Run, ref)) {
      delete ref;
      Cancel(id);
    }
  }

  // The platform task: run the task if it is still pending.
  static void Run(void* raw) {
    Ref* ref = static_cast<Ref*>(raw);
    UiTaskDispatcher* self = ref->self;
    uint64_t id = ref->id;
    delete ref;
    Pending p{nullptr, nullptr};
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      auto it = self->pending_.find(id);
      if (it == self->pending_.end())
        return;  // cancelled by Close
      p = it->second;
      self->pending_.erase(it);
    }
    p.task(p.data, true);
  }

  void Cancel(uint64_t id) {
    Pending p{nullptr, nullptr};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = pending_.find(id);
      if (it == pending_.end())
        return;
      p = it->second;
      pending_.erase(it);
    }
    p.task(p.data, false);
  }

  mutable std::mutex mutex_;
  std::thread::id ui_thread_;
  bool bound_ = false;
  bool closed_ = false;
  uint64_t next_id_ = 0;
  PostFn post_;
  std::unordered_map<uint64_t, Pending> pending_;
  std::vector<uint64_t> unposted_;
};

// Runs `fn` on the UI thread and blocks until it has run, or until it can no
// longer run: every synchronous UI-thread hop of the backends
// (cef_invoke_sync, gtk_invoke_sync, GtkRunSync, the WebView2 and macOS
// getters) goes through here, so none of them outlives the loop. True if
// `fn` ran; false when the loop had ended (Close) or the task could not be
// queued — the caller then answers with its defaults. Before Bind the task
// is held, and posted by Bind.
//
// `via`, when given, is the transport to queue it through instead of the
// bound queue (UiTaskDispatcher::DispatchVia).
//
// Never call it on the UI thread: the caller runs `fn` inline there (each
// backend checks its own notion of the thread first, which also holds before
// Bind).
template <typename F>
bool RunOnUiThreadAndWait(
    F& fn,
    const UiTaskDispatcher::PostFn& via = nullptr,
    UiTaskDispatcher& dispatcher = UiTaskDispatcher::Get()) {
  // `ctx` lives in this frame; Done() is the task's last access to it
  // (laufey_sync_call.h).
  struct Ctx {
    explicit Ctx(F* f) : fn(f) {}
    F* fn;
    bool ran = false;
    SyncCall call;
  };
  Ctx ctx(&fn);
  laufey_ui_task_fn task = [](void* data, bool ran) {
    auto* c = static_cast<Ctx*>(data);
    if (ran) {
      (*c->fn)();
      c->ran = true;
    }
    c->call.Done();
  };
  if (via)
    dispatcher.DispatchVia(via, task, &ctx);
  else
    dispatcher.Dispatch(task, &ctx);
  ctx.call.Wait();
  return ctx.ran;
}

#if defined(__APPLE__) && defined(__OBJC__)
// Runs `block` on the main thread and waits for it; inline when already
// there (a getter called from a handler on the main thread must not
// deadlock). Over GCD's main queue, which AppKit also drains inside its
// modal loops, and through the dispatcher, so the wait ends with the loop:
// false when `block` did not run (the caller answers with its defaults).
inline bool RunOnMainSync(void (^block)(void)) {
  if (pthread_main_np()) {
    block();
    return true;
  }
  auto run = [block] { block(); };
  return RunOnUiThreadAndWait(run, [](void (*task)(void*), void* data) {
    dispatch_async_f(dispatch_get_main_queue(), data, task);
    return true;
  });
}
#endif

}  // namespace laufey_common

#endif  // LAUFEY_UI_TASKS_H_
