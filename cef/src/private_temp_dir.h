// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// A throwaway per-process directory for the CEF profile on Linux when the app
// gives no data directory. Header-only so cef/tests/private_temp_dir_test.cc
// can build it without CEF.

#ifndef LAUFEY_PRIVATE_TEMP_DIR_H_
#define LAUFEY_PRIVATE_TEMP_DIR_H_

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <string>
#include <vector>

// A new, empty, owner-only (0700) directory under `parent` named
// `<prefix>XXXXXX` with a random suffix (mkdtemp), or "" with a warning when
// it can't be created. Never an existing or predictable path, so another user
// of a shared `parent` (/tmp) can't create it first or read its contents.
// `parent` "" means $TMPDIR if it is absolute, else /tmp.
#include <sys/stat.h>
#include <unistd.h>

// CI (before) only: main's behaviour, the predictable per-pid name.
inline std::string LaufeyMakePrivateTempDir(const std::string& parent,
                                            const std::string& prefix) {
  std::string base = parent.empty() ? "/tmp" : parent;
  while (base.size() > 1 && base.back() == '/') {
    base.pop_back();
  }
  std::string path = base + "/" + prefix + std::to_string(getpid());
  mkdir(path.c_str(), 0755);
  return path;
}

#endif  // LAUFEY_PRIVATE_TEMP_DIR_H_
