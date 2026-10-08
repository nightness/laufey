// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Whether a CEF profile holds cookies encrypted with the OS key
// (ReadProfileCookieKeys, laufey_platform_features.h): the profile's own
// cookie database, read before CefInitialize with the system SQLite, opened
// read-only. Anything that keeps it from being read counts as "it may":
// basic would delete such cookies for good, while waiting for the key only
// waits.

#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "laufey_platform_features.h"
#include "sqlite_loader.h"

namespace laufey_common {

namespace {

// Runs `sql` (one int column, at most one row): the value, -1 for no row.
// False (and `error`) when it fails.
bool QueryInt(const Sqlite& sqlite, Sqlite::Db* db, const char* sql, int* value,
              std::string* error) {
  Sqlite::Stmt* stmt = nullptr;
  if (sqlite.prepare_v2(db, sql, -1, &stmt, nullptr) != Sqlite::kOk) {
    *error = sqlite.errmsg(db);
    sqlite.finalize(stmt);
    return false;
  }
  int rc = sqlite.step(stmt);
  bool ok = rc == Sqlite::kRow || rc == Sqlite::kDone;
  if (rc == Sqlite::kRow)
    *value = sqlite.column_int(stmt, 0);
  else if (rc == Sqlite::kDone)
    *value = -1;
  else
    *error = sqlite.errmsg(db);
  sqlite.finalize(stmt);
  return ok;
}

ProfileCookieKeys ReadCookieDb(const std::string& path, std::string* detail) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    if (errno == ENOENT || errno == ENOTDIR)
      return ProfileCookieKeys::kNone;
    *detail = path + ": " + std::strerror(errno);
    return ProfileCookieKeys::kUnknown;
  }
  const Sqlite& sqlite = LoadSqlite();
  if (!sqlite.loaded()) {
    *detail = "no libsqlite3 to read " + path;
    return ProfileCookieKeys::kUnknown;
  }
  Sqlite::Db* db = nullptr;
  int rc = sqlite.open_v2(path.c_str(), &db, Sqlite::kOpenReadOnly, nullptr);
  if (rc != Sqlite::kOk) {
    *detail = path + ": " + (db ? sqlite.errmsg(db) : "can't open");
    sqlite.close_v2(db);
    return ProfileCookieKeys::kUnknown;
  }
  ProfileCookieKeys result = ProfileCookieKeys::kUnknown;
  std::string error;
  int tables = 0;
  int v11 = 0;
  // No cookies table: nothing to lose. Then any row Chromium encrypted with
  // the OS key ("v11", the bytes 76 31 31).
  if (!QueryInt(sqlite, db,
                "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND "
                "name = 'cookies'",
                &tables, &error)) {
    *detail = path + ": " + error;
  } else if (tables <= 0) {
    result = ProfileCookieKeys::kNone;
  } else if (!QueryInt(sqlite, db,
                       "SELECT 1 FROM cookies WHERE "
                       "substr(encrypted_value, 1, 3) = x'763131' LIMIT 1",
                       &v11, &error)) {
    *detail = path + ": " + error;
  } else {
    result = v11 == 1 ? ProfileCookieKeys::kOsKey : ProfileCookieKeys::kNone;
  }
  sqlite.close_v2(db);
  return result;
}

}  // namespace

ProfileCookieKeys ReadProfileCookieKeys(const std::string& root_cache_dir,
                                        std::string* detail) {
  std::string scratch;
  if (!detail)
    detail = &scratch;
  detail->clear();
  if (root_cache_dir.empty())
    return ProfileCookieKeys::kNone;
  // The default profile's cookie database: Default/Cookies on Linux, or
  // Default/Network/Cookies where Chromium moved it to the network
  // service's directory.
  ProfileCookieKeys result = ProfileCookieKeys::kNone;
  for (const char* rel : {"/Default/Cookies", "/Default/Network/Cookies"}) {
    std::string why;
    ProfileCookieKeys keys = ReadCookieDb(root_cache_dir + rel, &why);
    if (keys == ProfileCookieKeys::kOsKey)
      return keys;
    if (keys == ProfileCookieKeys::kUnknown) {
      result = keys;
      *detail = why;
    }
  }
  return result;
}

}  // namespace laufey_common
