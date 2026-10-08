// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// A shared library for argv_init_array_test.cc: its .init_array function
// reads argv as Deno's runtime library does when it loads
// (ext/node/ops/process.rs takes strlen(argv[argc - 1])). The C runtime
// passes it the process's own argc and argv, whatever the host did to its
// copy. It records instead of crashing: the argc it got, the first NULL
// entry below argc, and the length of argv[argc - 1].

#include <cstddef>
#include <cstring>

namespace {

int g_argc = -1;
int g_first_null = -1;
size_t g_last_len = 0;

void ReadArgv(int argc, char** argv, char** /*envp*/) {
  g_argc = argc;
  for (int i = 0; i < argc; ++i) {
    if (!argv[i]) {
      g_first_null = i;
      return;
    }
  }
  if (argc > 0)
    g_last_len = std::strlen(argv[argc - 1]);
}

__attribute__((section(".init_array"), used)) void (*g_read_argv)(
    int,
    char**,
    char**) = ReadArgv;

}  // namespace

extern "C" __attribute__((visibility("default"))) void laufey_argv_probe(
    int* argc,
    int* first_null,
    size_t* last_len) {
  *argc = g_argc;
  *first_null = g_first_null;
  *last_len = g_last_len;
}
