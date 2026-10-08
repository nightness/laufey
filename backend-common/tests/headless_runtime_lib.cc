// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// A stand-in runtime library for headless_launch_test.cc: laufey_runtime_start
// runs for LAUFEY_TEST_HEADLESS_MS milliseconds (a forked worker, the
// updater's helper), then writes "done" to LAUFEY_TEST_HEADLESS_MARKER and
// returns. It has no backend to call.

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define LAUFEY_TEST_EXPORT extern "C" __declspec(dllexport)
#else
#define LAUFEY_TEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

LAUFEY_TEST_EXPORT int laufey_runtime_init(const void* /*api*/) {
  return 0;
}

LAUFEY_TEST_EXPORT int laufey_runtime_start() {
  const char* ms = std::getenv("LAUFEY_TEST_HEADLESS_MS");
  std::this_thread::sleep_for(
      std::chrono::milliseconds(ms ? std::atol(ms) : 0));
  if (const char* marker = std::getenv("LAUFEY_TEST_HEADLESS_MARKER")) {
    if (FILE* f = std::fopen(marker, "w")) {
      std::fputs("done", f);
      std::fclose(f);
    }
  }
  return 0;
}

LAUFEY_TEST_EXPORT void laufey_runtime_shutdown() {}
