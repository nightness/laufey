// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The on_cancel half of a custom-scheme exchange (laufey_scheme_cancel_fn in
// laufey.h): the engine gave up on a request (the fetch was aborted, the
// document replaced, the window closed) before the runtime finished it.
//
// The contract the backends keep through this gate: on_cancel is called at
// most once per exchange, never once scheme_response_finish has returned,
// and the exchange stays valid for the whole call. The engine reports a
// cancel on its own thread while the runtime may be finishing the exchange
// on another, so Finish waits for a cancel call in progress before the
// exchange goes away; a runtime that finishes the exchange from inside its
// on_cancel (on the same thread) is not waited for.
//
// Header-only.

#ifndef LAUFEY_SCHEME_CANCEL_H_
#define LAUFEY_SCHEME_CANCEL_H_

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace laufey_common {

class SchemeCancelGate {
 public:
  // The engine cancelled the request: runs `report` (the runtime's
  // on_cancel) unless the exchange was already finished or cancelled. Any
  // thread.
  void Cancel(const std::function<void()>& report) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (finished_ || reported_)
        return;
      reported_ = true;
      calling_ = true;
      caller_ = std::this_thread::get_id();
    }
    report();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      calling_ = false;
    }
    cv_.notify_all();
  }

  // The runtime finished the exchange: no cancel is reported from now on,
  // and one in progress on another thread has returned when this does.
  void Finish() {
    std::unique_lock<std::mutex> lock(mutex_);
    finished_ = true;
    if (calling_ && caller_ == std::this_thread::get_id())
      return;  // finished from inside on_cancel
    cv_.wait(lock, [this] { return !calling_; });
  }

  bool cancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reported_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool finished_ = false;
  bool reported_ = false;
  bool calling_ = false;
  std::thread::id caller_;
};

}  // namespace laufey_common

#endif  // LAUFEY_SCHEME_CANCEL_H_
