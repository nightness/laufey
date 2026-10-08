// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// JsCallTable (laufey_js_calls.h): bridge calls reach the runtime under
// backend-issued ids, so two windows' pages (both counting from 1) never
// collide, an answer goes back to the window and page number that made the
// call, and an id the backend never issued or already answered reaches
// nothing. Plain asserts, no framework.

#include "laufey_js_calls.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using laufey_common::JsCallRoute;
using laufey_common::JsCallTable;

int main() {
  {
    JsCallTable table;
    // Two windows' pages both number their first call 1.
    uint64_t a = table.Add(1, 1);
    uint64_t b = table.Add(2, 1);
    EXPECT(a != 0 && b != 0 && a != b);
    JsCallRoute route;
    EXPECT(table.Take(b, &route));
    EXPECT(route.window_id == 2 && route.page_call_id == 1);
    EXPECT(table.Take(a, &route));
    EXPECT(route.window_id == 1 && route.page_call_id == 1);
    // A second answer, and ids never issued, reach nothing.
    EXPECT(!table.Take(a, &route));
    EXPECT(!table.Take(0, &route));
    EXPECT(!table.Take(987654321, &route));
    EXPECT(table.size() == 0);
  }
  {
    // A page that reuses a number (or forges one) gets a fresh id; ids are
    // never reused.
    JsCallTable table;
    std::set<uint64_t> ids;
    for (int i = 0; i < 100; ++i)
      ids.insert(table.Add(7, 5));
    EXPECT(ids.size() == 100);
    table.ForgetWindow(7);
    EXPECT(table.size() == 0);
    EXPECT(ids.count(table.Add(7, 5)) == 0);
  }
  {
    // ForgetWindow drops only that window's calls.
    JsCallTable table;
    uint64_t keep = table.Add(1, 10);
    table.Add(2, 11);
    table.Add(2, 12);
    table.ForgetWindow(2);
    EXPECT(table.size() == 1);
    JsCallRoute route;
    EXPECT(table.Take(keep, &route) && route.page_call_id == 10);
  }
  {
    // Any thread: concurrent adds and takes stay consistent.
    JsCallTable table;
    std::vector<std::thread> threads;
    for (uint32_t w = 1; w <= 8; ++w) {
      threads.emplace_back([&table, w] {
        for (uint64_t i = 1; i <= 2000; ++i) {
          uint64_t id = table.Add(w, i);
          JsCallRoute route;
          EXPECT(table.Take(id, &route));
          EXPECT(route.window_id == w && route.page_call_id == i);
        }
      });
    }
    for (auto& t : threads)
      t.join();
    EXPECT(table.size() == 0);
  }
  std::printf("js_calls_test: ok\n");
  return 0;
}
