// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Quitting ends the GTK dialogs (laufey_backend_common.h, RunGtkDialog /
// CancelGtkDialogsForQuit), on a private Xvfb:
//   - a confirm that is up ends as a cancel (0) when the UI thread asks;
//   - a prompt that is up ends as a cancel (0, no text) when another thread
//     asks (as quit() from the runtime thread does);
//   - an alert with a confirm nested in it (a dialog shown from within the
//     first one's loop): both end, the alert as dismissed (1), the confirm
//     as a cancel (0);
//   - once the app is quitting, a dialog isn't shown: it returns at once as
//     a cancel.
// Each must be over within a few seconds; a dialog left up hangs the test,
// which a watchdog fails. Exits 77 (skipped) without Xvfb.

#include <fcntl.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "laufey_backend_common.h"
#include "laufey_window.h"

using namespace laufey_common;

namespace {

pid_t g_xvfb = -1;

[[noreturn]] void Finish(int code) {
  if (g_xvfb > 0)
    kill(g_xvfb, SIGTERM);
  std::fflush(stdout);
  std::_Exit(code);
}

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      Finish(1);                                                             \
    }                                                                        \
  } while (0)

const char* g_case = "start";

void OnWatchdog(int) {
  static const char msg[] =
      "laufey_dialog_quit_test: a dialog was still up after 20 s\n";
  (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
  (void)!write(STDERR_FILENO, g_case, strlen(g_case));
  (void)!write(STDERR_FILENO, "\n", 1);
  if (g_xvfb > 0)
    kill(g_xvfb, SIGTERM);
  _exit(1);
}

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

// Starts a private Xvfb and points DISPLAY at it; its pid, or -1.
pid_t StartXvfb() {
  gchar* xvfb = g_find_program_in_path("Xvfb");
  if (!xvfb)
    return -1;
  int fds[2];
  EXPECT(pipe(fds) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int null = open("/dev/null", O_RDWR);
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    std::string fd = std::to_string(fds[1]);
    execl(xvfb, xvfb, "-displayfd", fd.c_str(), "-nolisten", "tcp", "-screen",
          "0", "800x600x24", static_cast<char*>(nullptr));
    _exit(127);
  }
  g_free(xvfb);
  close(fds[1]);
  pollfd p = {fds[0], POLLIN, 0};
  char buf[32] = {};
  if (poll(&p, 1, 10000) != 1 || read(fds[0], buf, sizeof(buf) - 1) <= 0) {
    kill(pid, SIGKILL);
    return -1;
  }
  close(fds[0]);
  std::string display = std::string(":") + strtok(buf, "\n");
  setenv("DISPLAY", display.c_str(), 1);
  return pid;
}

gboolean CancelNow(gpointer) {
  CancelGtkDialogsForQuit();
  return G_SOURCE_REMOVE;
}

int g_nested_result = -1;

gboolean ShowNestedConfirm(gpointer) {
  g_nested_result =
      ShowDialogLinux(LAUFEY_DIALOG_CONFIRM, "Nested", "inner", "", nullptr);
  return G_SOURCE_REMOVE;
}

}  // namespace

int main() {
  unsetenv("WAYLAND_DISPLAY");
  g_xvfb = StartXvfb();
  if (g_xvfb < 0) {
    std::printf("laufey_dialog_quit_test: Xvfb missing, skipped\n");
    return 77;
  }
  EXPECT(gtk_init_check(nullptr, nullptr));
  signal(SIGALRM, OnWatchdog);
  alarm(20);

  // A confirm, cancelled from the UI thread.
  {
    g_case = "confirm, cancelled on the UI thread";
    g_timeout_add(300, CancelNow, nullptr);
    auto t = Clock::now();
    int result =
        ShowDialogLinux(LAUFEY_DIALOG_CONFIRM, "Confirm", "quit?", "", nullptr);
    EXPECT(result == 0);
    EXPECT(SecondsSince(t) < 5);
  }

  // A prompt, cancelled from another thread.
  {
    g_case = "prompt, cancelled from another thread";
    std::thread quitter([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      CancelGtkDialogsForQuit();
    });
    char* text = reinterpret_cast<char*>(1);
    auto t = Clock::now();
    int result = ShowDialogLinux(LAUFEY_DIALOG_PROMPT, "Prompt", "name?",
                                 "default", &text);
    quitter.join();
    EXPECT(result == 0);
    EXPECT(text == nullptr);
    EXPECT(SecondsSince(t) < 5);
  }

  // An alert with a confirm nested in it: one cancel ends both.
  {
    g_case = "alert with a nested confirm";
    g_timeout_add(200, ShowNestedConfirm, nullptr);
    g_timeout_add(600, CancelNow, nullptr);
    auto t = Clock::now();
    int result =
        ShowDialogLinux(LAUFEY_DIALOG_ALERT, "Alert", "outer", "", nullptr);
    EXPECT(result == 1);
    EXPECT(g_nested_result == 0);
    EXPECT(SecondsSince(t) < 5);
  }

  // Quitting: not shown at all, nothing has to cancel it.
  {
    g_case = "confirm asked for while quitting";
    MarkQuitting();
    auto t = Clock::now();
    int result =
        ShowDialogLinux(LAUFEY_DIALOG_CONFIRM, "Confirm", "late", "", nullptr);
    EXPECT(result == 0);
    EXPECT(SecondsSince(t) < 1);
    char* text = reinterpret_cast<char*>(1);
    result =
        ShowDialogLinux(LAUFEY_DIALOG_PROMPT, "Prompt", "late", "x", &text);
    EXPECT(result == 0);
    EXPECT(text == nullptr);
    EXPECT(SecondsSince(t) < 1);
  }

  alarm(0);
  std::printf("laufey_dialog_quit_test: ok\n");
  Finish(0);
}
