// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Regression test (Linux): the host never changes the process's argv, so a
// library it loads afterwards (the runtime) gets intact arguments in its
// .init_array functions. A host that took D-Bus activation's argument out
// of argv in place left NULL at argv[argc - 1], where the C runtime still
// counts an argument, and Deno's initializer crashed on strlen(NULL) while
// a notification click started the app.
//
// ctest runs it as `<test> --x --laufey-dbus-activated --y`. It does what
// the hosts' main() does (CopyArgvWithoutDBusActivationArg, then edits the
// copy the way gtk_init does), loads argv_probe_lib.cc and checks what the
// probe's initializer saw. A negative control then moves the argument out
// of argv in place, as the old hosts did, and checks that a second copy of
// the probe sees the NULL. Plain asserts, no framework.

#include "laufey_notifications.h"

#include <dlfcn.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace laufey_common;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

struct ProbeResult {
  int argc = -1;
  int first_null = -1;
  size_t last_len = 0;
};

ProbeResult LoadProbe(const char* path) {
  void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!lib)
    std::fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
  EXPECT(lib);
  using ProbeFn = void (*)(int*, int*, size_t*);
  auto probe = reinterpret_cast<ProbeFn>(dlsym(lib, "laufey_argv_probe"));
  EXPECT(probe);
  ProbeResult r;
  probe(&r.argc, &r.first_null, &r.last_len);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  EXPECT(argc == 4 && std::strcmp(argv[2], kDBusActivationArg) == 0);
  const std::vector<char*> before(argv, argv + argc + 1);

  // The hosts' main(): a copy without the argument, which the host's own
  // parsers then edit (gtk_init takes the options it handled out of it).
  std::vector<char*> host_argv;
  EXPECT(CopyArgvWithoutDBusActivationArg(argc, argv, &host_argv));
  EXPECT((host_argv == std::vector<char*>{argv[0], argv[1], argv[3], nullptr}));
  host_argv.erase(host_argv.begin() + 1);
  EXPECT(std::vector<char*>(argv, argv + argc + 1) == before);

  // The runtime loads: its initializer gets argc and argv as they were.
  ProbeResult r = LoadProbe(LAUFEY_ARGV_PROBE_LIB);
  EXPECT(r.argc == argc);
  EXPECT(r.first_null == -1);
  EXPECT(r.last_len == std::strlen("--y"));

  // Negative control: the old in-place removal is what the probe catches.
  for (int i = 2; i + 1 < argc; ++i)
    argv[i] = argv[i + 1];
  argv[argc - 1] = nullptr;
  ProbeResult control = LoadProbe(LAUFEY_ARGV_PROBE_CONTROL_LIB);
  EXPECT(control.argc == argc);
  EXPECT(control.first_null == argc - 1);
  for (int i = 0; i <= argc; ++i)
    argv[i] = before[i];

  std::printf("argv_init_array_test: OK\n");
  return 0;
}
