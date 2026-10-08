// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// ReadProfileCookieKeys (cookie_db_linux.cc): whether a CEF profile's cookie
// database holds cookies encrypted with the OS key ("v11" rows), which
// --password-store=basic would make Chromium delete. Databases built here
// with the same system SQLite: v11 rows present, only v10 rows, no rows, no
// cookies table, no database, the network service's path, and the cases
// that must count as "may hold them" (a corrupt file, an unreadable one, one
// a running instance holds locked). Exits 77 (skipped) without libsqlite3.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "laufey_platform_features.h"
#include "sqlite_loader.h"

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;
namespace fs = std::filesystem;

namespace {

const Sqlite* g_sqlite = nullptr;

Sqlite::Db* Open(const fs::path& path) {
  Sqlite::Db* db = nullptr;
  EXPECT(g_sqlite->open_v2(path.c_str(), &db,
                           Sqlite::kOpenReadWrite | Sqlite::kOpenCreate,
                           nullptr) == Sqlite::kOk);
  return db;
}

void Exec(Sqlite::Db* db, const std::string& sql) {
  char* error = nullptr;
  int rc = g_sqlite->exec(db, sql.c_str(), nullptr, nullptr, &error);
  if (rc != Sqlite::kOk)
    std::fprintf(stderr, "sqlite: %s\n", error ? error : "?");
  EXPECT(rc == Sqlite::kOk);
}

// A cookie database shaped like Chromium's, with one row per prefix given
// ("" for a plain-text value).
void MakeCookies(const fs::path& path,
                 std::initializer_list<const char*> prefixes) {
  fs::create_directories(path.parent_path());
  fs::remove(path);
  Sqlite::Db* db = Open(path);
  Exec(db,
       "CREATE TABLE meta(key LONGVARCHAR NOT NULL UNIQUE PRIMARY KEY, value "
       "LONGVARCHAR);"
       "INSERT INTO meta VALUES('version', '24');"
       "CREATE TABLE cookies(creation_utc INTEGER NOT NULL, host_key TEXT NOT "
       "NULL, top_frame_site_key TEXT NOT NULL, name TEXT NOT NULL, value TEXT "
       "NOT NULL, encrypted_value BLOB NOT NULL, path TEXT NOT NULL);");
  int n = 0;
  for (const char* prefix : prefixes) {
    std::string hex;
    for (const char* c = prefix; *c; ++c) {
      char buf[3];
      std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(*c));
      hex += buf;
    }
    // The prefix, then ciphertext-looking bytes.
    std::string blob = *prefix ? "x'" + hex + "00ff10'" : "x''";
    Exec(db, "INSERT INTO cookies VALUES(" + std::to_string(++n) +
                 ", '127.0.0.1', '', 'c" + std::to_string(n) + "', '" +
                 (*prefix ? "" : "plain") + "', " + blob + ", '/');");
  }
  g_sqlite->close_v2(db);
}

}  // namespace

int main() {
  const Sqlite& sqlite = LoadSqlite();
  if (!sqlite.loaded()) {
    std::printf("laufey_cookie_db_test: no libsqlite3, skipped\n");
    return 77;
  }
  g_sqlite = &sqlite;
  fs::path root = fs::temp_directory_path() /
                  ("laufey-cookie-db-" + std::to_string(getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  fs::path cookies = root / "Default" / "Cookies";
  std::string detail;

  // A profile kept in memory, and one with no cookie database yet.
  EXPECT(ReadProfileCookieKeys("", &detail) == ProfileCookieKeys::kNone);
  EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
         ProfileCookieKeys::kNone);
  EXPECT(detail.empty());
  EXPECT(ReadProfileCookieKeys((root / "missing").string()) ==
         ProfileCookieKeys::kNone);

  // Rows under basic (v10) and plain-text ones: nothing basic would lose.
  MakeCookies(cookies, {"v10", "v10", ""});
  EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
         ProfileCookieKeys::kNone);
  MakeCookies(cookies, {});
  EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
         ProfileCookieKeys::kNone);

  // One row under the OS key among others: v11.
  MakeCookies(cookies, {"v10", "v11", ""});
  EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
         ProfileCookieKeys::kOsKey);
  MakeCookies(cookies, {"v11"});
  EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
         ProfileCookieKeys::kOsKey);
  // "v1" alone, or v11 elsewhere than the start, is not the prefix.
  MakeCookies(cookies, {"v1", "xv11"});
  EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
         ProfileCookieKeys::kNone);

  // A database without a cookies table (another file of that name, or one
  // Chromium created and never filled): no cookies, nothing to lose.
  {
    fs::remove(cookies);
    Sqlite::Db* db = Open(cookies);
    Exec(db, "CREATE TABLE other(x INTEGER);");
    g_sqlite->close_v2(db);
    EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
           ProfileCookieKeys::kNone);
  }

  // The network service's directory (Default/Network/Cookies) counts too.
  MakeCookies(cookies, {"v10"});
  MakeCookies(root / "Default" / "Network" / "Cookies", {"v11"});
  EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
         ProfileCookieKeys::kOsKey);
  fs::remove_all(root / "Default" / "Network");

  // Corrupt: not a database at all, or a database cut short. Counts as
  // holding OS-key cookies (err on the side that loses nothing), with the
  // reason.
  {
    FILE* file = std::fopen(cookies.c_str(), "wb");
    EXPECT(file);
    std::fputs("this is not an SQLite database, not even close.........\n",
               file);
    std::fclose(file);
    EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
           ProfileCookieKeys::kUnknown);
    EXPECT(detail.find("Cookies") != std::string::npos);
    EXPECT(detail.find("not a database") != std::string::npos);
    MakeCookies(cookies, {"v10", "v11"});
    fs::resize_file(cookies, 600);  // the header promises pages it lacks
    EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
           ProfileCookieKeys::kUnknown);
    EXPECT(!detail.empty());
  }

  // A database a running instance holds exclusively (Chromium's exclusive
  // locking mode): it can't be read now, so it may hold them.
  {
    MakeCookies(cookies, {"v10"});
    Sqlite::Db* db = Open(cookies);
    Exec(db, "PRAGMA locking_mode = EXCLUSIVE; BEGIN EXCLUSIVE;");
    EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
           ProfileCookieKeys::kUnknown);
    EXPECT(detail.find("locked") != std::string::npos);
    Exec(db, "COMMIT;");
    g_sqlite->close_v2(db);
    EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
           ProfileCookieKeys::kNone);
  }

  // A database that can't be opened (permissions; root reads anything).
  if (geteuid() != 0) {
    MakeCookies(cookies, {"v10"});
    chmod(cookies.c_str(), 0);
    EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
           ProfileCookieKeys::kUnknown);
    EXPECT(!detail.empty());
    chmod(cookies.c_str(), 0600);
    // A profile directory that can't be searched: can't tell either.
    chmod((root / "Default").c_str(), 0);
    EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
           ProfileCookieKeys::kUnknown);
    chmod((root / "Default").c_str(), 0700);
  }

  // Read-only: nothing was written next to the database (no journal).
  MakeCookies(cookies, {"v11"});
  EXPECT(ReadProfileCookieKeys(root.string(), &detail) ==
         ProfileCookieKeys::kOsKey);
  int entries = 0;
  for (const auto& entry : fs::directory_iterator(root / "Default")) {
    (void)entry;
    ++entries;
  }
  EXPECT(entries == 1);

  fs::remove_all(root);
  std::printf("laufey_cookie_db_test: OK\n");
  return 0;
}
