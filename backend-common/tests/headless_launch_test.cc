// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// A headless launch (`<host> run <script>`: a forked worker, the updater's
// helper) runs its runtime to the end, however long it takes. The host
// (LAUFEY_HOST: the WebKitGTK, WKWebView, WebView2 or CEF host) is started
// with the stand-in runtime (headless_runtime_lib.cc) for 0 ms and for 12 s,
// longer than the bounded wait a windowed app's runtime gets once its loop
// has ended; each must write its marker and the host must exit 0 after it,
// not before.

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

#ifdef _WIN32

std::string MakeTempDir() {
  char base[MAX_PATH + 1];
  DWORD n = GetTempPathA(sizeof(base), base);
  if (n == 0 || n > MAX_PATH)
    return "";
  for (int i = 0; i < 1000; i++) {
    std::string dir = std::string(base) + "laufey_headless_" +
                      std::to_string(GetCurrentProcessId()) + "_" +
                      std::to_string(i);
    if (CreateDirectoryA(dir.c_str(), nullptr))
      return dir;
  }
  return "";
}

void RemoveTempDir(const std::string& dir, const std::string& marker) {
  DeleteFileA(marker.c_str());
  RemoveDirectoryA(dir.c_str());
}

// Starts the host with `env` set; its exit code, or -1 if it ran past
// `limit` (then killed), -2 if it did not start.
int RunToEnd(const char* const (&env)[3][2], std::chrono::milliseconds limit) {
  for (const auto& kv : env)
    SetEnvironmentVariableA(kv[0], kv[1]);
  std::string command = std::string("\"") + LAUFEY_HOST + "\" run probe.js";
  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  BOOL started = CreateProcessA(LAUFEY_HOST, &command[0], nullptr, nullptr,
                                FALSE, 0, nullptr, nullptr, &si, &pi);
  for (const auto& kv : env)
    SetEnvironmentVariableA(kv[0], nullptr);
  if (!started) {
    std::fprintf(stderr, "headless_launch_test: CreateProcess: %lu\n",
                 GetLastError());
    return -2;
  }
  CloseHandle(pi.hThread);
  int result = -1;
  if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(limit.count())) ==
      WAIT_OBJECT_0) {
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result = static_cast<int>(code);
  } else {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
  }
  CloseHandle(pi.hProcess);
  return result;
}

#else

std::string MakeTempDir() {
  char dir_template[] = "/tmp/laufey_headless_XXXXXX";
  const char* dir = mkdtemp(dir_template);
  return dir ? dir : "";
}

void RemoveTempDir(const std::string& dir, const std::string& marker) {
  unlink(marker.c_str());
  rmdir(dir.c_str());
}

// Starts the host with `env` set; its exit status, or -1 if it ran past
// `limit` (then killed), -2 if it did not start, -3 if a signal ended it.
int RunToEnd(const char* const (&env)[3][2], std::chrono::milliseconds limit) {
  auto start = std::chrono::steady_clock::now();
  pid_t pid = fork();
  if (pid == 0) {
    for (const auto& kv : env)
      setenv(kv[0], kv[1], 1);
    execl(LAUFEY_HOST, LAUFEY_HOST, "run", "probe.js",
          static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid < 0)
    return -2;
  int status = 0;
  while (waitpid(pid, &status, WNOHANG) != pid) {
    if (std::chrono::steady_clock::now() - start > limit) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
}

#endif

bool RunHost(long runtime_ms) {
  std::string dir = MakeTempDir();
  if (dir.empty()) {
    std::fprintf(stderr, "headless_launch_test: no temporary directory\n");
    return false;
  }
  std::string marker = dir + "/marker";
  std::string ms = std::to_string(runtime_ms);
  const char* const env[3][2] = {
      {"LAUFEY_RUNTIME_PATH", LAUFEY_HEADLESS_RUNTIME},
      {"LAUFEY_TEST_HEADLESS_MARKER", marker.c_str()},
      {"LAUFEY_TEST_HEADLESS_MS", ms.c_str()},
  };
  int code = RunToEnd(
      env, std::chrono::milliseconds(runtime_ms) + std::chrono::seconds(30));
  std::string text;
  {
    std::ifstream in(marker);
    std::stringstream read;
    read << in.rdbuf();
    text = read.str();
  }
  RemoveTempDir(dir, marker);
  if (code == -1) {
    std::fprintf(stderr, "headless_launch_test: the host hung (%ld ms)\n",
                 runtime_ms);
    return false;
  }
  if (code != 0) {
    std::fprintf(stderr,
                 "headless_launch_test: the host ended with status %d "
                 "(%ld ms)\n",
                 code, runtime_ms);
    return false;
  }
  if (text != "done") {
    std::fprintf(stderr,
                 "headless_launch_test: the host exited before its %ld ms "
                 "runtime finished\n",
                 runtime_ms);
    return false;
  }
  return true;
}

}  // namespace

int main() {
  for (long ms : {0L, 12000L}) {
    if (!RunHost(ms))
      return 1;
  }
  std::printf("headless_launch_test: ok (%s)\n", LAUFEY_HOST);
  return 0;
}
