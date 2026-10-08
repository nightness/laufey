// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// A shared library for exit_guard_test.cc's "late" case, loaded with dlopen
// after the exit guard is installed, as the runtime and the graphics stack
// are. It stands for a library whose exit-time teardown is registered after
// the guard: a function-local static (WebKit's GBM device, Mesa's state)
// built on first use, whose destructor exit() runs. Those run before
// anything registered earlier (exit handlers run in reverse order). Its
// destructor takes away memory the UI thread reads, as unloading libgbm's
// backend did under a WebKitGTK paint. It also calls exit() itself, as the
// runtime library does (Deno.exit()), so the call binds the way the
// runtime's does.

#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>

namespace {

struct Teardown {
  void* page = nullptr;
  long size = 0;
  ~Teardown() {
    // The memory the UI thread reads goes away; a UI thread still running
    // faults on its next read. Give it time to.
    mprotect(page, static_cast<size_t>(size), PROT_NONE);
    usleep(300 * 1000);
  }
};

}  // namespace

// Builds the static whose destructor tears `page` down at exit.
extern "C" __attribute__((visibility("default"))) void laufey_test_arm_teardown(
    void* page, long size) {
  static Teardown teardown;
  teardown.page = page;
  teardown.size = size;
}

extern "C" __attribute__((visibility("default"))) void
laufey_test_exit_from_library(int code) {
  exit(code);
}
