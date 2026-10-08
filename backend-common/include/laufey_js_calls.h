// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The backend's table of JS bridge calls in flight.
//
// A page numbers its own calls (the bridge script's `callId`) and the answer
// goes back to it under that number. Those numbers are page-chosen: every
// window's page starts at 1, and any script in the page can post a message
// with any number. So the backend hands the runtime its own id instead,
// unique in the process, and keeps which window and which page number it
// answers. js_call_respond then reaches exactly the window that made the call,
// under the number that page used, and a call id the runtime never issued (or
// already answered) reaches nothing.

#ifndef LAUFEY_JS_CALLS_H_
#define LAUFEY_JS_CALLS_H_

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace laufey_common {

struct JsCallRoute {
  uint32_t window_id = 0;
  uint64_t page_call_id = 0;
};

class JsCallTable {
 public:
  // Records a call window `window_id`'s page numbered `page_call_id` and
  // returns the id the runtime sees (never 0, never reused). Any thread.
  uint64_t Add(uint32_t window_id, uint64_t page_call_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_id_++;
    calls_[id] = JsCallRoute{window_id, page_call_id};
    return id;
  }

  // Removes the call the runtime knows as `id` and returns where its answer
  // goes; false when there is no such call (never issued, or answered).
  // Any thread.
  bool Take(uint64_t id, JsCallRoute* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = calls_.find(id);
    if (it == calls_.end())
      return false;
    if (out)
      *out = it->second;
    calls_.erase(it);
    return true;
  }

  // Drops every call of a window that is gone (its answers have nowhere to
  // go). Any thread.
  void ForgetWindow(uint32_t window_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = calls_.begin(); it != calls_.end();) {
      if (it->second.window_id == window_id)
        it = calls_.erase(it);
      else
        ++it;
    }
  }

  size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_.size();
  }

 private:
  std::mutex mutex_;
  uint64_t next_id_ = 1;
  std::unordered_map<uint64_t, JsCallRoute> calls_;
};

}  // namespace laufey_common

#endif  // LAUFEY_JS_CALLS_H_
