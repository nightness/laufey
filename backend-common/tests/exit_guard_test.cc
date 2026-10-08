// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// InstallUiExitGuard (laufey_io.h): a process that exits from another thread
// (the runtime's Deno.exit()) while the UI thread is busy must not keep
// running UI code while exit tears the libraries down, must not leave a
// later UI dispatch waiting forever, and must end even if exit hangs.
//
// Each case runs in a child process (this binary with the case's name):
//   busy     the UI thread spins a GLib idle source; a second thread calls
//            exit(); a handler that runs after the guard's checks that the
//            UI thread has stopped.
//   nested   the same, with the UI thread inside a nested main loop (as in
//            gtk_dialog_run) when exit comes.
//   dispatch after the UI thread is parked, a task that queued behind the
//            park and every later dispatch are answered with `ran` false,
//            at once.
//   watchdog an exit handler that never returns: the process still ends,
//            with exit's status, about 5 seconds later; with
//            LAUFEY_EXIT_WATCHDOG_SECS=1 about a second later, and with
//            LAUFEY_EXIT_WATCHDOG_SECS=0 not at all (no watchdog).
//   late     a library loaded after the guard (dlopen, as the runtime and
//            the graphics stack are) registers exit-time teardown that takes
//            away memory the busy UI thread reads, then calls exit() from
//            another thread. Exit handlers run in reverse order, so that
//            teardown runs before any handler the guard registered: the UI
//            thread must be parked before it, or it faults (as a WebKitGTK
//            paint did on libgbm's unloaded backend).
//   forked   a child forked without exec (from another thread, as a library
//            may) inherits the guard but not the UI thread: its exit()
//            parks nothing and returns at once, with its status.

#include <dlfcn.h>
#include <glib.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "laufey_io.h"
#include "laufey_ui_tasks.h"

namespace {

using laufey_common::UiTaskDispatcher;

std::atomic<uint64_t> g_ticks{0};

[[noreturn]] void Fail(const char* what) {
  std::fprintf(stderr, "laufey_exit_guard_test: %s\n", what);
  _exit(1);
}

void BindGlibDispatcher() {
  UiTaskDispatcher::Get().Bind([](void (*task)(void*), void* data) {
    struct Task {
      void (*task)(void*);
      void* data;
    };
    g_idle_add(
        [](gpointer p) -> gboolean {
          auto* t = static_cast<Task*>(p);
          t->task(t->data);
          delete t;
          return G_SOURCE_REMOVE;
        },
        new Task{task, data});
    return true;
  });
}

void ExpectParked() {
  uint64_t a = g_ticks.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint64_t b = g_ticks.load();
  if (a != b)
    Fail("the UI thread kept running during exit");
}

// Registered before the guard, so it runs after it (exit handlers are LIFO).
void CheckParked() {
  ExpectParked();
  _exit(0);
}

std::atomic<int> g_queued_ran{-1};

void CheckDispatchAfterPark() {
  ExpectParked();
  UiTaskDispatcher& d = UiTaskDispatcher::Get();
  if (!d.closed())
    Fail("the dispatcher is still open after the UI thread was parked");
  // The task queued behind the park was answered by the guard's Close.
  if (g_queued_ran.load() != 0)
    Fail("a task queued behind the park was not answered with ran=false");
  // A dispatch from here on is answered inline, not queued.
  auto start = std::chrono::steady_clock::now();
  int ran = -1;
  d.Dispatch([](void* data, bool r) { *static_cast<int*>(data) = r ? 1 : 0; },
             &ran);
  if (ran != 0)
    Fail("a dispatch after parking was not answered with ran=false");
  auto run = [] {};
  if (laufey_common::RunOnUiThreadAndWait(run))
    Fail("a synchronous UI call after parking reported that it ran");
  if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
    Fail("a dispatch after parking waited");
  _exit(0);
}

void HangForever() {
  for (;;)
    pause();
}

void Busy() {
  g_idle_add(
      [](gpointer) -> gboolean {
        g_ticks++;
        return G_SOURCE_CONTINUE;
      },
      nullptr);
}

// The "late" case's shared page and library (exit_guard_late_lib.cc).
volatile int* g_shared = nullptr;
void (*g_exit_from_library)(int) = nullptr;

void BusyReadingShared() {
  g_idle_add(
      [](gpointer) -> gboolean {
        g_ticks += static_cast<uint64_t>(*g_shared);
        return G_SOURCE_CONTINUE;
      },
      nullptr);
}

void LoadLateLibrary() {
  long size = sysconf(_SC_PAGESIZE);
  void* page = mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED)
    Fail("mmap failed");
  g_shared = static_cast<volatile int*>(page);
  *g_shared = 1;
  void* lib = dlopen(LAUFEY_EXIT_GUARD_LATE_LIB, RTLD_NOW | RTLD_LOCAL);
  if (!lib)
    Fail(dlerror());
  auto arm = reinterpret_cast<void (*)(void*, long)>(
      dlsym(lib, "laufey_test_arm_teardown"));
  g_exit_from_library = reinterpret_cast<void (*)(int)>(
      dlsym(lib, "laufey_test_exit_from_library"));
  if (!arm || !g_exit_from_library)
    Fail("the late library lacks its entry points");
  arm(page, size);
}

void After(int ms, void (*fn)()) {
  std::thread([ms, fn] {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    fn();
  }).detach();
}

int RunCase(const std::string& name) {
  BindGlibDispatcher();
  if (name == "busy" || name == "nested") {
    atexit(CheckParked);
  } else if (name == "dispatch") {
    atexit(CheckDispatchAfterPark);
  } else if (name == "watchdog") {
    atexit(HangForever);
  } else if (name != "late" && name != "forked") {
    Fail("unknown case");
  }
  laufey_common::InstallUiExitGuard();
  if (name == "late") {
    LoadLateLibrary();
    BusyReadingShared();
    // As Deno.exit() does: from another thread, through the library.
    After(200, [] { g_exit_from_library(0); });
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
    return 2;  // not reached
  }
  Busy();
  if (name == "forked") {
    After(200, [] {
      auto start = std::chrono::steady_clock::now();
      pid_t pid = fork();
      if (pid == 0)
        exit(5);
      if (pid < 0)
        Fail("fork failed");
      int status = 0;
      while (waitpid(pid, &status, WNOHANG) != pid) {
        if (std::chrono::steady_clock::now() - start >
            std::chrono::seconds(8)) {
          kill(pid, SIGKILL);
          waitpid(pid, &status, 0);
          Fail("a forked child's exit() hung");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      auto took = std::chrono::steady_clock::now() - start;
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 5)
        Fail("a forked child's exit() lost its status");
      if (took > std::chrono::milliseconds(500)) {
        std::fprintf(stderr,
                     "laufey_exit_guard_test: the forked child's exit() took "
                     "%lld ms\n",
                     static_cast<long long>(
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             took)
                             .count()));
        Fail("a forked child's exit() waited for a UI thread it lacks");
      }
      _exit(0);
    });
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
    return 2;  // not reached
  }
  if (name == "nested") {
    // The UI thread enters a nested loop (a modal dialog) and is still in it
    // when exit comes.
    g_idle_add(
        [](gpointer) -> gboolean {
          GMainLoop* nested = g_main_loop_new(nullptr, FALSE);
          g_main_loop_run(nested);
          Fail("the nested loop returned");
        },
        nullptr);
  }
  if (name == "dispatch") {
    // The UI thread is held from 100 to 400 ms: exit's park (200 ms) and
    // then another task (250 ms) queue up behind each other meanwhile.
    g_timeout_add(
        100,
        [](gpointer) -> gboolean {
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
          return G_SOURCE_REMOVE;
        },
        nullptr);
    After(250, [] {
      UiTaskDispatcher::Get().Dispatch(
          [](void*, bool ran) { g_queued_ran = ran ? 1 : 0; }, nullptr);
    });
  }
  // As Deno.exit() does, from the runtime thread.
  if (name == "watchdog")
    After(200, [] { exit(7); });
  else
    After(200, [] { exit(0); });
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  return 2;  // not reached
}

// Runs case `name` in a child (with LAUFEY_EXIT_WATCHDOG_SECS=`watchdog_secs`
// when given); true if it exited with `want` within `limit`.
bool Child(const char* self, const char* name, int want,
           std::chrono::seconds limit, std::chrono::milliseconds* took,
           const char* watchdog_secs = nullptr) {
  auto start = std::chrono::steady_clock::now();
  pid_t pid = fork();
  if (pid == 0) {
    if (watchdog_secs)
      setenv("LAUFEY_EXIT_WATCHDOG_SECS", watchdog_secs, 1);
    else
      unsetenv("LAUFEY_EXIT_WATCHDOG_SECS");
    execl(self, self, name, static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid < 0)
    return false;
  int status = 0;
  while (waitpid(pid, &status, WNOHANG) != pid) {
    if (std::chrono::steady_clock::now() - start > limit) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      std::fprintf(stderr, "laufey_exit_guard_test: %s hung\n", name);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  *took = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  if (WIFSIGNALED(status)) {
    std::fprintf(stderr, "laufey_exit_guard_test: %s died of signal %d\n", name,
                 WTERMSIG(status));
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != want) {
    std::fprintf(stderr, "laufey_exit_guard_test: %s ended with status %d\n",
                 name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return false;
  }
  return true;
}

// Runs case `name` with LAUFEY_EXIT_WATCHDOG_SECS=`watchdog_secs`; true if
// it is still running after `wait` (then killed).
bool ChildStillRunning(const char* self, const char* name,
                       const char* watchdog_secs, std::chrono::seconds wait) {
  pid_t pid = fork();
  if (pid == 0) {
    setenv("LAUFEY_EXIT_WATCHDOG_SECS", watchdog_secs, 1);
    execl(self, self, name, static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid < 0)
    return false;
  auto start = std::chrono::steady_clock::now();
  int status = 0;
  while (std::chrono::steady_clock::now() - start < wait) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      std::fprintf(stderr, "laufey_exit_guard_test: %s ended with status %d\n",
                   name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1)
    return RunCase(argv[1]);
  std::chrono::milliseconds took{0};
  for (const char* name : {"busy", "nested", "dispatch", "late", "forked"}) {
    if (!Child(argv[0], name, 0, std::chrono::seconds(10), &took))
      return 1;
  }
  // The watchdog: exit's own status, about 5 seconds after exit began.
  if (!Child(argv[0], "watchdog", 7, std::chrono::seconds(15), &took))
    return 1;
  if (took < std::chrono::seconds(4)) {
    std::fprintf(stderr, "laufey_exit_guard_test: the watchdog fired early\n");
    return 1;
  }
  // LAUFEY_EXIT_WATCHDOG_SECS=1: the same, about a second after exit began.
  std::chrono::milliseconds quick{0};
  if (!Child(argv[0], "watchdog", 7, std::chrono::seconds(10), &quick, "1"))
    return 1;
  if (quick < std::chrono::milliseconds(900) ||
      quick > std::chrono::seconds(4)) {
    std::fprintf(stderr,
                 "laufey_exit_guard_test: LAUFEY_EXIT_WATCHDOG_SECS=1 fired "
                 "after %lld ms\n",
                 static_cast<long long>(quick.count()));
    return 1;
  }
  // LAUFEY_EXIT_WATCHDOG_SECS=0: no watchdog; the hung exit is still hung
  // well past the default 5 seconds.
  if (!ChildStillRunning(argv[0], "watchdog", "0", std::chrono::seconds(7))) {
    std::fprintf(stderr,
                 "laufey_exit_guard_test: LAUFEY_EXIT_WATCHDOG_SECS=0 did not "
                 "turn the watchdog off\n");
    return 1;
  }
  std::printf(
      "laufey_exit_guard_test: ok (watchdog after %lld ms; after %lld ms "
      "with LAUFEY_EXIT_WATCHDOG_SECS=1; none with 0)\n",
      static_cast<long long>(took.count()),
      static_cast<long long>(quick.count()));
  return 0;
}
