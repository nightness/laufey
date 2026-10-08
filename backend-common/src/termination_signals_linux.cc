// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// SIGTERM, SIGINT and SIGHUP end the app through its own quit, the path
// quit() and closing the last window take, on both Linux backends
// (laufey_backend_common.h). Without them the CEF host would take Chromium's
// handlers (chrome/browser/shutdown_signal_handlers_posix.cc), which for
// SIGTERM end the browser process at once with _exit(0), without the
// runtime's shutdown or CefShutdown; and the WebKitGTK host would be killed
// by the default action, without the runtime's shutdown or the web storage
// flush.
//
// The quit runs on the UI thread, as an idle source on the default main
// context, which the UI thread runs. The signal itself is not taken there:
// a UI thread stuck in a call that never returns to the loop (CEF blocked
// in xcb_wait_for_reply under cef_window_create_top_level, the X server not
// answering, before the first window exists) would never see it, and the
// process would ignore SIGTERM until SIGKILL. So the handler only writes the
// signal to a pipe; a watcher thread reads it, hands the quit to the UI
// thread, and, if the UI thread has not taken it within the quit deadline
// (LAUFEY_SIGNAL_QUIT_DEADLINE_SECS, 10 by default; 0 waits for ever), ends
// the process itself, by that signal (its default action), after flushing
// what it can without the UI thread (stdio not held by another thread). A
// quit the UI thread has taken is never cut short.

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "laufey_backend_common.h"

namespace laufey_common {

namespace {

constexpr int kSignals[] = {SIGTERM, SIGINT, SIGHUP};
constexpr long kDefaultQuitDeadlineSeconds = 10;

// The signal pipe: the handler writes the signal number to [1] (non-blocking
// so it never waits), the watcher thread reads [0]. Created once.
int g_pipe[2] = {-1, -1};

std::mutex g_mutex;
std::condition_variable g_cv;
// Guarded by g_mutex.
bool g_installed = false;
bool g_quit_taken = false;
void (*g_quit)() = nullptr;
long g_deadline_seconds = kDefaultQuitDeadlineSeconds;

// A value that isn't a whole number of seconds keeps the default.
long QuitDeadlineSecondsFromEnv() {
  const char* env = getenv("LAUFEY_SIGNAL_QUIT_DEADLINE_SECS");
  if (!env || !*env)
    return kDefaultQuitDeadlineSeconds;
  char* end = nullptr;
  errno = 0;
  long secs = strtol(env, &end, 10);
  if (errno != 0 || *end != '\0' || secs < 0)
    return kDefaultQuitDeadlineSeconds;
  return secs;
}

void SetDefaultAction(int sig) {
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
}

// Async-signal-safe: sigaction and write only. The first signal gives all
// three their default action back (a second one ends a quit that hangs) and
// hands the signal to the watcher thread.
void OnSignal(int sig) {
  const int saved_errno = errno;
  for (int s : kSignals)
    SetDefaultAction(s);
  unsigned char byte = static_cast<unsigned char>(sig);
  ssize_t written = write(g_pipe[1], &byte, 1);
  (void)written;
  errno = saved_errno;
}

// On the UI thread: the quit is taken (the watcher stops counting), the
// handlers are removed, and the app quits.
gboolean QuitOnUiThread(gpointer) {
  void (*quit)() = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_installed)
      return G_SOURCE_REMOVE;  // Removed meanwhile: the loop is over.
    g_quit_taken = true;
    quit = g_quit;
  }
  g_cv.notify_all();
  RemoveTerminationSignalHandlers();
  if (quit)
    quit();
  return G_SOURCE_REMOVE;
}

void FlushStdio(FILE* f) {
  // Never wait for a stream another thread holds (the stuck UI thread, say).
  if (ftrylockfile(f) != 0)
    return;
  fflush_unlocked(f);
  funlockfile(f);
}

[[noreturn]] void EndBySignal(int sig, long seconds) {
  char msg[160];
  int len = snprintf(msg, sizeof(msg),
                     "laufey: the UI thread did not take the quit within %ld "
                     "s; ending the process\n",
                     seconds);
  if (len > 0) {
    ssize_t written = write(STDERR_FILENO, msg, static_cast<size_t>(len));
    (void)written;
  }
  FlushStdio(stdout);
  FlushStdio(stderr);
  // The signal's own default action (OnSignal already set it), delivered to
  // this thread: the process ends by `sig`, as it would have without the
  // handlers.
  SetDefaultAction(sig);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);
  pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
  pthread_kill(pthread_self(), sig);
  _exit(128 + sig);
}

void* Watcher(void*) {
  for (;;) {
    unsigned char byte = 0;
    ssize_t n = read(g_pipe[0], &byte, 1);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return nullptr;
    const int sig = byte;
    long seconds = 0;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (!g_installed)
        continue;  // Removed after the signal came: nothing to quit.
      g_quit_taken = false;
      seconds = g_deadline_seconds;
    }
    // An idle source attached to the default context always runs on the
    // thread that iterates it (g_main_context_invoke would run it here when
    // the UI thread doesn't hold the context, as when it is stuck outside
    // the loop). Attaching wakes the context.
    GSource* source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_HIGH);
    g_source_set_callback(source, QuitOnUiThread, nullptr, nullptr);
    g_source_attach(source, nullptr);
    g_source_unref(source);
    // With write(2), not stdio: a stuck thread may hold stderr's lock (in a
    // write of its own), and the deadline below must still be kept.
    char msg[96];
    int len = snprintf(msg, sizeof(msg), "laufey: %s, quitting\n",
                       strsignal(sig));
    if (len > 0) {
      size_t n = std::min(static_cast<size_t>(len), sizeof(msg) - 1);
      ssize_t written = write(STDERR_FILENO, msg, n);
      (void)written;
    }

    std::unique_lock<std::mutex> lock(g_mutex);
    auto taken = [] { return g_quit_taken || !g_installed; };
    if (seconds == 0) {
      g_cv.wait(lock, taken);
      continue;
    }
    if (!g_cv.wait_for(lock, std::chrono::seconds(seconds), taken)) {
      lock.unlock();
      EndBySignal(sig, seconds);
    }
  }
}

bool StartWatcher() {
  if (g_pipe[0] >= 0)
    return true;
  if (pipe2(g_pipe, O_CLOEXEC) != 0) {
    g_pipe[0] = g_pipe[1] = -1;
    return false;
  }
  // The write end never blocks the handler.
  fcntl(g_pipe[1], F_SETFL, fcntl(g_pipe[1], F_GETFL) | O_NONBLOCK);
  // The watcher takes none of the signals itself: the handler runs on any
  // other thread, never holding the watcher up.
  sigset_t block, old;
  sigemptyset(&block);
  for (int s : kSignals)
    sigaddset(&block, s);
  pthread_sigmask(SIG_BLOCK, &block, &old);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  int rc = pthread_create(&thread, &attr, Watcher, nullptr);
  pthread_attr_destroy(&attr);
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
  if (rc != 0) {
    close(g_pipe[0]);
    close(g_pipe[1]);
    g_pipe[0] = g_pipe[1] = -1;
    return false;
  }
  return true;
}

}  // namespace

void InstallTerminationSignalHandlers(void (*quit)()) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_quit = quit;
    if (g_installed)
      return;
    if (!StartWatcher()) {
      std::cerr << "laufey: no signal pipe; SIGTERM, SIGINT and SIGHUP keep "
                   "their default action"
                << std::endl;
      return;
    }
    g_deadline_seconds = QuitDeadlineSecondsFromEnv();
    g_quit_taken = false;
    g_installed = true;
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = OnSignal;
  sa.sa_flags = SA_RESTART;
  // One handler at a time: another of the three waits until this one has
  // given them all their default action back.
  sigemptyset(&sa.sa_mask);
  for (int s : kSignals)
    sigaddset(&sa.sa_mask, s);
  for (int s : kSignals)
    sigaction(s, &sa, nullptr);
}

void RemoveTerminationSignalHandlers() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_installed)
      return;
    g_installed = false;
  }
  g_cv.notify_all();
  for (int s : kSignals)
    SetDefaultAction(s);
}

}  // namespace laufey_common
