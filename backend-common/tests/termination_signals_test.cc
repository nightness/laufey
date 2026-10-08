// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// InstallTerminationSignalHandlers (laufey_backend_common.h), the Linux
// hosts' SIGTERM / SIGINT / SIGHUP handling. For each signal, in a child
// process running the default main context:
//   - the first signal calls the quit callback once, on the thread running
//     the loop, and the process is not killed;
//   - it gives all three signals their default action back: a second one
//     (the same or another) ends the process with that signal;
// and RemoveTerminationSignalHandlers gives them back without a signal.
// With the UI thread stuck in a nested call that never returns to the loop
// (CEF blocked in xcb_wait_for_reply before its first window), the signal
// still ends the process: by the signal, within the quit deadline
// (LAUFEY_SIGNAL_QUIT_DEADLINE_SECS), also when the stuck thread holds
// stderr's stdio lock (in a write of its own); and a quit that the loop has
// taken but that runs past the deadline is not cut short.

#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <thread>

#include "laufey_backend_common.h"

using namespace laufey_common;

namespace {

int g_failures = 0;

void Expect(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "laufey_termination_signals_test: FAIL %s\n", what);
    g_failures++;
  }
}

int g_quits = 0;
std::thread::id g_loop_thread;
bool g_quit_on_loop_thread = false;

void OnQuit() {
  g_quits++;
  g_quit_on_loop_thread = std::this_thread::get_id() == g_loop_thread;
}

// Child: handlers in, `first` raised while the loop runs; exits 10 + quits
// (with 20 added when the quit ran off the loop thread) once the loop has
// seen it, after raising `second` (0: none).
[[noreturn]] void Child(int first, int second) {
  g_loop_thread = std::this_thread::get_id();
  InstallTerminationSignalHandlers(OnQuit);
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  struct Ctx {
    GMainLoop* loop;
    int first;
  } ctx{loop, first};
  g_idle_add(
      [](gpointer p) -> gboolean {
        auto* c = static_cast<Ctx*>(p);
        kill(getpid(), c->first);
        return G_SOURCE_REMOVE;
      },
      &ctx);
  g_timeout_add(
      500,
      [](gpointer p) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(p));
        return G_SOURCE_REMOVE;
      },
      loop);
  g_main_loop_run(loop);
  if (second != 0) {
    kill(getpid(), second);
    // Delivery to this thread is synchronous; a handled signal falls through.
    usleep(200 * 1000);
  }
  _exit(10 + g_quits + (g_quit_on_loop_thread ? 0 : 20));
}

int Run(int first, int second) {
  std::fflush(stderr);
  pid_t pid = fork();
  if (pid == 0)
    Child(first, second);
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status))
    return -WTERMSIG(status);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1000;
}

// Child: handlers in; the loop's first callback blocks in a nested call
// that never returns (poll on nothing, like xcb_wait_for_reply on an X
// server that does not answer) while another thread sends `sig`. Exits 0 if
// the block ever ends (it does not).
[[noreturn]] void StuckChild(int sig) {
  InstallTerminationSignalHandlers(OnQuit);
  std::thread([sig] {
    usleep(300 * 1000);
    kill(getpid(), sig);
  }).detach();
  g_idle_add(
      [](gpointer) -> gboolean {
        for (;;)
          poll(nullptr, 0, -1);
        return G_SOURCE_REMOVE;
      },
      nullptr);
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  _exit(0);
}

// Child: as StuckChild, but the UI thread takes stderr's stdio lock before it
// blocks (a thread stuck writing to stderr), and the signal is SIGTERM. The
// watcher thread must not wait for that lock. Exits 0 if the block ever ends.
[[noreturn]] void StuckHoldingStderrChild() {
  InstallTerminationSignalHandlers(OnQuit);
  std::thread([] {
    usleep(300 * 1000);
    kill(getpid(), SIGTERM);
  }).detach();
  g_idle_add(
      [](gpointer) -> gboolean {
        flockfile(stderr);
        for (;;)
          poll(nullptr, 0, -1);
        return G_SOURCE_REMOVE;
      },
      nullptr);
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  _exit(0);
}

// Child: handlers in; the quit callback, once the loop has taken it, takes
// 2.5 s (past a 1 s deadline) and then ends the loop; exits 30 + quits.
[[noreturn]] void SlowQuitChild() {
  static GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  InstallTerminationSignalHandlers([] {
    g_quits++;
    usleep(2500 * 1000);
    g_main_loop_quit(loop);
  });
  g_idle_add(
      [](gpointer) -> gboolean {
        kill(getpid(), SIGTERM);
        return G_SOURCE_REMOVE;
      },
      nullptr);
  g_main_loop_run(loop);
  _exit(30 + g_quits);
}

long long NowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

// Forks `child` with LAUFEY_SIGNAL_QUIT_DEADLINE_SECS=1 and waits up to
// `limit_ms` for it: its status as Run's, -2000 if it was still running
// (then killed). `*elapsed_ms` is how long it took.
template <typename F>
int RunWithDeadline(F child, long long limit_ms, long long* elapsed_ms) {
  std::fflush(stderr);
  const long long start = NowMs();
  pid_t pid = fork();
  if (pid == 0) {
    setenv("LAUFEY_SIGNAL_QUIT_DEADLINE_SECS", "1", 1);
    child();
  }
  int status = 0;
  for (;;) {
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid)
      break;
    if (NowMs() - start > limit_ms) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      *elapsed_ms = NowMs() - start;
      return -2000;
    }
    usleep(20 * 1000);
  }
  *elapsed_ms = NowMs() - start;
  if (WIFSIGNALED(status))
    return -WTERMSIG(status);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1000;
}

}  // namespace

int main() {
  const int signals[] = {SIGTERM, SIGINT, SIGHUP};
  for (int sig : signals) {
    // Handled once, on the loop's thread; the process lives on.
    Expect(Run(sig, 0) == 11, "the first signal quits once on the UI thread");
    // The second one takes the default action.
    Expect(Run(sig, sig) == -sig, "a second signal takes its default action");
  }
  // After one of them, the others are back to their default action too.
  Expect(Run(SIGTERM, SIGINT) == -SIGINT, "SIGTERM gives SIGINT back");
  Expect(Run(SIGHUP, SIGTERM) == -SIGTERM, "SIGHUP gives SIGTERM back");

  // Removed without a signal: the default action.
  {
    std::fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
      InstallTerminationSignalHandlers(OnQuit);
      RemoveTerminationSignalHandlers();
      kill(getpid(), SIGTERM);
      usleep(200 * 1000);
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    Expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
           "removed handlers give SIGTERM its default action");
  }

  // A UI thread that never gets back to the loop: the signal still ends the
  // process, by that signal, about a second (the deadline) after it came.
  for (int sig : signals) {
    long long elapsed = 0;
    int result = RunWithDeadline([sig] { StuckChild(sig); }, 8000, &elapsed);
    if (result != -sig)
      std::fprintf(stderr,
                   "laufey_termination_signals_test: stuck UI thread, signal "
                   "%d: result %d after %lld ms\n",
                   sig, result, elapsed);
    Expect(result == -sig,
           "a signal ends the process when the UI thread is stuck");
    Expect(elapsed < 5000, "within the quit deadline");
  }
  // The same with the stuck thread holding stderr's lock.
  {
    long long elapsed = 0;
    int result =
        RunWithDeadline([] { StuckHoldingStderrChild(); }, 8000, &elapsed);
    if (result != -SIGTERM)
      std::fprintf(stderr,
                   "laufey_termination_signals_test: stuck UI thread holding "
                   "stderr: result %d after %lld ms\n",
                   result, elapsed);
    Expect(result == -SIGTERM,
           "a signal ends the process when the stuck thread holds stderr");
    Expect(elapsed < 5000, "within the quit deadline, stderr held");
  }
  // A quit the loop took is not cut short by the deadline.
  {
    long long elapsed = 0;
    int result = RunWithDeadline([] { SlowQuitChild(); }, 8000, &elapsed);
    if (result != 31)
      std::fprintf(stderr,
                   "laufey_termination_signals_test: slow quit: result %d "
                   "after %lld ms\n",
                   result, elapsed);
    Expect(result == 31, "a slow quit the loop took runs to its end");
  }

  if (g_failures) {
    std::fprintf(stderr, "laufey_termination_signals_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("laufey_termination_signals_test: ok\n");
  return 0;
}
