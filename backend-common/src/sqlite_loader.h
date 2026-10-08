// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The few SQLite calls laufey makes, from the system libsqlite3 loaded at
// run time (no build dependency: every distribution CEF runs on ships it,
// NSS itself needs it). Chromium's own SQLite is linked into libcef with
// renamed symbols, so the two never meet. Linux only. Used by
// cookie_db_linux.cc and its test.

#ifndef LAUFEY_SQLITE_LOADER_H_
#define LAUFEY_SQLITE_LOADER_H_

#include <dlfcn.h>

#include <type_traits>

namespace laufey_common {

struct Sqlite {
  // Opaque handles.
  struct Db;
  struct Stmt;

  static constexpr int kOk = 0;
  static constexpr int kRow = 100;
  static constexpr int kDone = 101;
  static constexpr int kOpenReadOnly = 0x00000001;
  static constexpr int kOpenReadWrite = 0x00000002;
  static constexpr int kOpenCreate = 0x00000004;

  int (*open_v2)(const char*, Db**, int, const char*) = nullptr;
  int (*close_v2)(Db*) = nullptr;
  int (*prepare_v2)(Db*, const char*, int, Stmt**, const char**) = nullptr;
  int (*step)(Stmt*) = nullptr;
  int (*column_int)(Stmt*, int) = nullptr;
  int (*finalize)(Stmt*) = nullptr;
  const char* (*errmsg)(Db*) = nullptr;
  int (*exec)(Db*, const char*, void*, void*, char**) = nullptr;

  bool loaded() const {
    return open_v2 != nullptr;
  }
};

// The library's functions, loaded once (never unloaded). `loaded()` is
// false when there is no libsqlite3.
inline const Sqlite& LoadSqlite() {
  static const Sqlite sqlite = [] {
    Sqlite s;
    void* lib = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      lib = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      return s;
    Sqlite t;
    bool ok = true;
    auto sym = [&](auto& fn, const char* name) {
      void* p = dlsym(lib, name);
      ok = ok && p != nullptr;
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
    };
    sym(t.open_v2, "sqlite3_open_v2");
    sym(t.close_v2, "sqlite3_close_v2");
    sym(t.prepare_v2, "sqlite3_prepare_v2");
    sym(t.step, "sqlite3_step");
    sym(t.column_int, "sqlite3_column_int");
    sym(t.finalize, "sqlite3_finalize");
    sym(t.errmsg, "sqlite3_errmsg");
    sym(t.exec, "sqlite3_exec");
    if (!ok) {
      dlclose(lib);
      return s;
    }
    return t;
  }();
  return sqlite;
}

}  // namespace laufey_common

#endif  // LAUFEY_SQLITE_LOADER_H_
