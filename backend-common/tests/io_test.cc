// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the portable part of laufey_io.h (io.cc): the file-drop
// dispatch, drag-out path checks, file URIs, the file dialog slot and option
// copying, default-path splitting, CF_HTML and clipboard format lists. Plain
// asserts, no framework: run via `ctest --test-dir webview/build` (or
// cef/build). Exits non-zero on the first failure.

#include "laufey_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

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

struct DropCall {
  uint32_t window_id;
  int phase;
  double x, y;
  bool paths_null;
  std::vector<std::string> paths;
  size_t count;
};
std::vector<DropCall> g_drops;

void OnDrop(void* user_data, uint32_t window_id, int phase, double x, double y,
            const char* const* paths, size_t count) {
  EXPECT(user_data == &g_drops);
  DropCall c{window_id, phase, x, y, paths == nullptr, {}, count};
  if (paths) {
    for (size_t i = 0; i < count; i++)
      c.paths.emplace_back(paths[i]);
  }
  g_drops.push_back(c);
}

void TestFileDrop() {
  std::vector<std::string> two = {"/a", "/b c"};
  // No handler: nothing delivered.
  EXPECT(!HasFileDropHandler());
  EXPECT(!DispatchFileDrop(1, LAUFEY_DRAG_DROP, 0, 0, two, 2));
  SetFileDropHandler(OnDrop, &g_drops);
  EXPECT(HasFileDropHandler());

  // ENTER without paths: the count only.
  EXPECT(DispatchFileDrop(7, LAUFEY_DRAG_ENTER, 10, 20, {}, 3));
  EXPECT(g_drops.size() == 1);
  EXPECT(g_drops[0].window_id == 7 && g_drops[0].phase == LAUFEY_DRAG_ENTER);
  EXPECT(g_drops[0].paths_null && g_drops[0].count == 3);
  EXPECT(g_drops[0].x == 10 && g_drops[0].y == 20);

  // OVER with paths: the paths, count = their number.
  EXPECT(DispatchFileDrop(7, LAUFEY_DRAG_OVER, 11, 21, two, 9));
  EXPECT(!g_drops[1].paths_null && g_drops[1].count == 2);
  EXPECT(g_drops[1].paths == two);

  // LEAVE: never paths, count 0.
  EXPECT(DispatchFileDrop(7, LAUFEY_DRAG_LEAVE, 0, 0, two, 2));
  EXPECT(g_drops[2].paths_null && g_drops[2].count == 0);

  // DROP: paths never NULL, even when none resolved.
  EXPECT(DispatchFileDrop(7, LAUFEY_DRAG_DROP, 5, 6, {}, 1));
  EXPECT(!g_drops[3].paths_null && g_drops[3].count == 0);
  EXPECT(DispatchFileDrop(7, LAUFEY_DRAG_DROP, 5, 6, two, 2));
  EXPECT(g_drops[4].paths == two);

  // Unknown phase: refused.
  EXPECT(!DispatchFileDrop(7, 9, 0, 0, two, 2));
  EXPECT(g_drops.size() == 5);

  // The cap.
  std::vector<std::string> many(LAUFEY_MAX_DROP_PATHS + 10, "/x");
  EXPECT(DispatchFileDrop(1, LAUFEY_DRAG_DROP, 0, 0, many, many.size()));
  EXPECT(g_drops.back().count == LAUFEY_MAX_DROP_PATHS);
  EXPECT(DispatchFileDrop(1, LAUFEY_DRAG_ENTER, 0, 0, {}, 1u << 30));
  EXPECT(g_drops.back().count == LAUFEY_MAX_DROP_PATHS);

  // The test hook takes C arrays (NULL entries skipped).
  const char* raw[] = {"/one", nullptr, "/two"};
  EXPECT(TestTriggerFileDrop(3, LAUFEY_DRAG_DROP, 1, 2, raw, 3));
  EXPECT((g_drops.back().paths == std::vector<std::string>{"/one", "/two"}));

  SetFileDropHandler(nullptr, nullptr);
  EXPECT(!TestTriggerFileDrop(3, LAUFEY_DRAG_DROP, 1, 2, raw, 3));
}

void TestFileUris() {
  std::string p;
#ifdef _WIN32
  EXPECT(FileUriToPath("file:///C:/Users/a%20b/x.txt", &p));
  EXPECT(p == "C:\\Users\\a b\\x.txt");
#else
  EXPECT(FileUriToPath("file:///home/a%20b/x.txt", &p));
  EXPECT(p == "/home/a b/x.txt");
  EXPECT(FileUriToPath("file://localhost/tmp/y", &p));
  EXPECT(p == "/tmp/y");
  EXPECT(FileUriToPath("file:///tmp/%C3%A9", &p));
  EXPECT(p == "/tmp/\xC3\xA9");
#endif
  EXPECT(!FileUriToPath("https://example.com/x", &p));
  EXPECT(!FileUriToPath("file://otherhost/x", &p));
  EXPECT(!FileUriToPath("file:///tmp/%00x", &p));  // embedded NUL
  EXPECT(!FileUriToPath("file:///tmp/%FFx", &p));  // invalid UTF-8
  EXPECT(!FileUriToPath("file:///tmp/%zz", &p));   // bad escape
}

void TestUriList() {
#ifndef _WIN32
  // CRLF (RFC 2483) or LF, comments and non-file URIs skipped, a trailing
  // NUL (some X11 sources include it) ignored.
  EXPECT((UriListToPaths("file:///a%20b.txt\r\n# comment\r\n"
                         "https://example.com/x\r\nfile:///tmp/c\r\n") ==
          std::vector<std::string>{"/a b.txt", "/tmp/c"}));
  EXPECT((UriListToPaths("file:///one\nfile://localhost/two") ==
          std::vector<std::string>{"/one", "/two"}));
  EXPECT((UriListToPaths(std::string("file:///z\r\n\0", 11)) ==
          std::vector<std::string>{"/z"}));
#endif
  EXPECT(UriListToPaths("").empty());
  EXPECT(UriListToPaths("# only a comment\r\n").empty());
  EXPECT(UriListToPaths("https://example.com/\r\nfile://host/x\r\n").empty());
  // At most LAUFEY_MAX_DROP_PATHS.
  std::string many;
  for (int i = 0; i < LAUFEY_MAX_DROP_PATHS + 5; i++)
#ifdef _WIN32
    many += "file:///C:/f" + std::to_string(i) + "\r\n";
#else
    many += "file:///f" + std::to_string(i) + "\r\n";
#endif
  EXPECT(UriListToPaths(many).size() == LAUFEY_MAX_DROP_PATHS);
}

std::string TempDir() {
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, buf);
  std::string d(buf, n);
  while (!d.empty() && (d.back() == '\\' || d.back() == '/'))
    d.pop_back();
  return d;
#else
  return "/tmp";
#endif
}

void TestDragPaths() {
  std::vector<std::string> out;
  std::string dir = TempDir();
  const char* ok[] = {dir.c_str()};
  EXPECT(ValidateDragPaths(ok, 1, &out));
  EXPECT(out.size() == 1 && out[0] == dir);
  EXPECT(!ValidateDragPaths(nullptr, 1, &out));
  EXPECT(!ValidateDragPaths(ok, 0, &out));
  const char* relative[] = {"relative/path"};
  EXPECT(!ValidateDragPaths(relative, 1, &out));
  std::string missing = dir + "/laufey-io-test-does-not-exist";
  const char* gone[] = {missing.c_str()};
  EXPECT(!ValidateDragPaths(gone, 1, &out));
  const char* with_null[] = {dir.c_str(), nullptr};
  EXPECT(!ValidateDragPaths(with_null, 2, &out));
  const char* bad_utf8[] = {"/tmp/\xFF"};
  EXPECT(!ValidateDragPaths(bad_utf8, 1, &out));
  EXPECT(!ValidateDragPaths(ok, LAUFEY_MAX_DROP_PATHS + 1, &out));

  // DragOutRequest reports exactly once.
  int calls = 0, last = -1;
  struct Ctx {
    int* calls;
    int* last;
  } ctx{&calls, &last};
  DragOutRequest req;
  req.callback = [](void* ud, int result) {
    auto* c = static_cast<Ctx*>(ud);
    (*c->calls)++;
    *c->last = result;
  };
  req.user_data = &ctx;
  req.Finish(LAUFEY_DRAG_RESULT_CANCELLED);
  req.Finish(LAUFEY_DRAG_RESULT_DROPPED);
  EXPECT(calls == 1 && last == LAUFEY_DRAG_RESULT_CANCELLED);
}

void TestDialogOptions() {
  FileDialogRequest r;
  EXPECT(!CopyFileDialogOptions(nullptr, &r));
  laufey_file_dialog_options_t o = {};
  o.kind = 7;
  EXPECT(!CopyFileDialogOptions(&o, &r));
  o.kind = LAUFEY_FILE_DIALOG_OPEN;
  o.flags = LAUFEY_FILE_DIALOG_MULTIPLE;
  o.title = "Pick";
  const char* img[] = {".png", "JPG", "", "a;b", "*.x", nullptr, "web\xFFp"};
  const char* any[] = {"*"};
  const char* none[] = {""};
  laufey_file_filter_t filters[] = {
      {"Images", img, 7}, {nullptr, any, 1}, {"Empty", none, 1}};
  o.filters = filters;
  o.filter_count = 3;
  EXPECT(CopyFileDialogOptions(&o, &r));
  EXPECT(r.title == "Pick" && r.default_path.empty());
  EXPECT(r.Multiple() && r.ChoosesFiles() && !r.ChoosesDirectories());
  // "Empty" had no usable extension and is dropped; the nameless one gets a
  // name.
  EXPECT(r.filters.size() == 2);
  EXPECT((r.filters[0].extensions == std::vector<std::string>{"png", "JPG"}));
  EXPECT(r.filters[1].name == "All Files");
  EXPECT((r.filters[1].extensions == std::vector<std::string>{"*"}));

  // Invalid UTF-8 in a string option fails the whole request.
  o.title = "\xC3";
  EXPECT(!CopyFileDialogOptions(&o, &r));
  o.title = nullptr;

  // Directory flags.
  o.flags = LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES;
  EXPECT(CopyFileDialogOptions(&o, &r));
  EXPECT(r.ChoosesDirectories() && !r.ChoosesFiles() && !r.Multiple());
  o.flags =
      LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES | LAUFEY_FILE_DIALOG_CHOOSE_FILES;
  EXPECT(CopyFileDialogOptions(&o, &r));
  EXPECT(r.ChoosesDirectories() && r.ChoosesFiles());
  // Save ignores the open-only flags.
  o.kind = LAUFEY_FILE_DIALOG_SAVE;
  o.flags = LAUFEY_FILE_DIALOG_MULTIPLE | LAUFEY_FILE_DIALOG_CHOOSE_DIRECTORIES;
  EXPECT(CopyFileDialogOptions(&o, &r));
  EXPECT(!r.Multiple() && !r.ChoosesDirectories() && r.ChoosesFiles());

  // Caps.
  std::vector<laufey_file_filter_t> lots(LAUFEY_FILE_DIALOG_MAX_FILTERS + 5,
                                         {"x", any, 1});
  o.filters = lots.data();
  o.filter_count = lots.size();
  EXPECT(CopyFileDialogOptions(&o, &r));
  EXPECT(r.filters.size() == LAUFEY_FILE_DIALOG_MAX_FILTERS);
}

void TestSplitDefaultPath() {
  std::string dir, name;
  std::string tmp = TempDir();
  SplitDefaultPath(tmp, &dir, &name);
  EXPECT(dir == tmp && name.empty());
#ifdef _WIN32
  std::string file = tmp + "\\report.pdf";
#else
  std::string file = tmp + "/report.pdf";
#endif
  SplitDefaultPath(file, &dir, &name);
  EXPECT(dir == tmp && name == "report.pdf");
  SplitDefaultPath("report.pdf", &dir, &name);
  EXPECT(dir.empty() && name == "report.pdf");
  SplitDefaultPath("/laufey-io-test-missing-dir/x.txt", &dir, &name);
  EXPECT(dir.empty() && name == "x.txt");
  SplitDefaultPath("", &dir, &name);
  EXPECT(dir.empty() && name.empty());
}

struct DialogResult {
  int calls = 0;
  uint32_t id = 0;
  int status = -1;
  std::vector<std::string> paths;
};

void OnDialog(void* ud, uint32_t id, int status, const char* const* paths,
              size_t count) {
  auto* r = static_cast<DialogResult*>(ud);
  r->calls++;
  r->id = id;
  r->status = status;
  r->paths.clear();
  if (paths) {
    for (size_t i = 0; i < count; i++)
      r->paths.emplace_back(paths[i]);
  } else {
    EXPECT(count == 0);
  }
}

class FakeDialog : public FileDialogPlatform {
 public:
  explicit FakeDialog(uint32_t id, int* alive) : id_(id), alive_(alive) {
    (*alive_)++;
  }
  ~FakeDialog() override {
    (*alive_)--;
  }
  bool Show() override {
    return true;
  }
  void Cancel() override {
    FileDialogFinish(id_, LAUFEY_FILE_DIALOG_CANCELLED, {});
  }
  bool TestAccept(const std::string& path) override {
    FileDialogFinish(id_, LAUFEY_FILE_DIALOG_ACCEPTED, {path});
    return true;
  }

 private:
  uint32_t id_;
  int* alive_;
};

void TestDialogSlot() {
  const UiRunner inline_runner = [](std::function<void()> fn) { fn(); };
  int alive = 0;
  laufey_file_dialog_options_t o = {};
  o.kind = LAUFEY_FILE_DIALOG_OPEN;

  // No callback: nothing shown.
  EXPECT(ShowFileDialogCommon(
             &o, nullptr, nullptr, inline_runner,
             [](uint32_t, const FileDialogRequest&) { EXPECT(false); }) == 0);

  // Bad options: FAILED at once.
  DialogResult bad;
  laufey_file_dialog_options_t wrong = {};
  wrong.kind = 42;
  EXPECT(ShowFileDialogCommon(
             &wrong, OnDialog, &bad, inline_runner,
             [](uint32_t, const FileDialogRequest&) { EXPECT(false); }) == 0);
  EXPECT(bad.calls == 1 && bad.status == LAUFEY_FILE_DIALOG_FAILED);

  // Open one, then a second is BUSY.
  DialogResult first;
  uint32_t shown_id = 0;
  uint32_t id =
      ShowFileDialogCommon(&o, OnDialog, &first, inline_runner,
                           [&](uint32_t did, const FileDialogRequest&) {
                             shown_id = did;
                             FileDialogAttach(did, new FakeDialog(did, &alive));
                           });
  EXPECT(id != 0 && shown_id == id && FileDialogIsOpen(id));
  DialogResult second;
  EXPECT(ShowFileDialogCommon(
             &o, OnDialog, &second, inline_runner,
             [](uint32_t, const FileDialogRequest&) { EXPECT(false); }) == 0);
  EXPECT(second.calls == 1 && second.status == LAUFEY_FILE_DIALOG_BUSY);
  EXPECT(first.calls == 0);

  // The test hook accepts through the platform's completion path.
  EXPECT(!FileDialogTestRespond(7, nullptr, inline_runner));
  EXPECT(FileDialogTestRespond(LAUFEY_TEST_DIALOG_ACCEPT, "/picked",
                               inline_runner));
  EXPECT(first.calls == 1 && first.id == id &&
         first.status == LAUFEY_FILE_DIALOG_ACCEPTED);
  EXPECT((first.paths == std::vector<std::string>{"/picked"}));
  EXPECT(!FileDialogIsOpen(id));
  // Exactly once.
  FileDialogFinish(id, LAUFEY_FILE_DIALOG_CANCELLED, {});
  EXPECT(first.calls == 1);
  // The finished platform object is kept until the next one finishes.
  EXPECT(alive == 1);

  // A second dialog, cancelled by id; a wrong id is refused.
  DialogResult third;
  uint32_t id3 =
      ShowFileDialogCommon(&o, OnDialog, &third, inline_runner,
                           [&](uint32_t did, const FileDialogRequest&) {
                             FileDialogAttach(did, new FakeDialog(did, &alive));
                           });
  EXPECT(id3 != 0 && id3 != id);
  EXPECT(!FileDialogCancel(id, inline_runner));
  EXPECT(!FileDialogCancel(0, inline_runner));
  EXPECT(FileDialogCancel(id3, inline_runner));
  EXPECT(third.calls == 1 && third.status == LAUFEY_FILE_DIALOG_CANCELLED);
  EXPECT(third.paths.empty());
  EXPECT(alive == 1);  // the first one was retired now

  // ACCEPTED with no paths reads as CANCELLED.
  DialogResult fourth;
  uint32_t id4 = ShowFileDialogCommon(
      &o, OnDialog, &fourth, inline_runner,
      [&](uint32_t did, const FileDialogRequest&) {
        FileDialogFinish(did, LAUFEY_FILE_DIALOG_ACCEPTED, {});
      });
  EXPECT(id4 != 0);
  EXPECT(fourth.calls == 1 && fourth.status == LAUFEY_FILE_DIALOG_CANCELLED);

  // Cancel before the UI thread showed it: the show callback never runs.
  std::function<void()> deferred;
  const UiRunner later = [&](std::function<void()> fn) {
    deferred = std::move(fn);
  };
  DialogResult fifth;
  bool shown = false;
  uint32_t id5 = ShowFileDialogCommon(
      &o, OnDialog, &fifth, later,
      [&](uint32_t, const FileDialogRequest&) { shown = true; });
  EXPECT(id5 != 0);
  // No platform attached yet: the test hook can't drive it.
  EXPECT(!FileDialogTestRespond(LAUFEY_TEST_DIALOG_CANCEL, nullptr, later));
  FileDialogFinish(id5, LAUFEY_FILE_DIALOG_CANCELLED, {});
  deferred();
  EXPECT(!shown && fifth.calls == 1);
}

void TestCfHtml() {
  std::string fragment = "<b>caf\xC3\xA9</b>";
  std::string cf = BuildCfHtml(fragment);
  EXPECT(cf.compare(0, 12, "Version:0.9\r") == 0);
  std::string back;
  EXPECT(ExtractCfHtmlFragment(cf, &back));
  EXPECT(back == fragment);
  // A trailing NUL and slack from GlobalSize don't matter.
  std::string padded = cf + std::string("\0\0\0", 3);
  EXPECT(ExtractCfHtmlFragment(padded, &back) && back == fragment);
  // Offsets pointing past the end are rejected.
  std::string broken =
      "Version:0.9\r\nStartFragment:0000099999\r\n"
      "EndFragment:0000100000\r\n";
  EXPECT(!ExtractCfHtmlFragment(broken, &back));
  // StartHTML/EndHTML as the fallback.
  std::string html_only =
      "Version:0.9\r\nStartHTML:0000000055\r\nEndHTML:0000000060\r\nhello";
  EXPECT(ExtractCfHtmlFragment(html_only, &back) && back == "hello");
}

void TestFormats() {
  char* joined = JoinClipboardFormats(
      {"text/plain", "text/html", "text/plain", "", "image/png"});
  EXPECT(joined && std::string(joined) == "text/plain\ntext/html\nimage/png");
  free(joined);
  char* empty = JoinClipboardFormats({});
  EXPECT(empty && empty[0] == '\0');
  free(empty);
  const uint8_t png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0};
  EXPECT(LooksLikePng(png, sizeof(png)));
  EXPECT(!LooksLikePng(png, 8));
  EXPECT(!LooksLikePng(nullptr, 0));
  const uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0, 0};
  EXPECT(!LooksLikePng(jpg, sizeof(jpg)));
  EXPECT(MallocCopy(nullptr, 3) == nullptr);
  uint8_t* copy = MallocCopy(png, sizeof(png));
  EXPECT(copy && memcmp(copy, png, sizeof(png)) == 0);
  free(copy);
}

void TestObserverScript() {
  std::string s = BuildDomFileDropObserverScript();
  // A function of `send`, trusted events only, capture phase, and the
  // dragover that keeps drops coming.
  EXPECT(s.find("(function (send)") == 0);
  EXPECT(s.find("e.isTrusted") != std::string::npos);
  EXPECT(s.find("e.preventDefault()") != std::string::npos);
  EXPECT(s.find("fn, true)") != std::string::npos);
  EXPECT(s.find("e.dataTransfer.files") != std::string::npos);
  // Nothing is looked up through a page-replaceable intrinsic at event time.
  EXPECT(s.find("call.call") == std::string::npos);
  EXPECT(s.find(".apply(") == std::string::npos);

  // The WebView2 drop script: the token in the closure, the posting functions
  // bound at document creation, and no call through Function.prototype.call
  // (or apply) or chrome.webview at event time, so a page that replaces them
  // later never sees the token.
  std::string wv = BuildWebView2FileDropScript("0123abcd");
  EXPECT(wv.find("var token = '0123abcd';") != std::string::npos);
  EXPECT(wv.find("var post = call.bind(wv.postMessage);") != std::string::npos);
  EXPECT(wv.find("call.bind(wv.postMessageWithAdditionalObjects)") !=
         std::string::npos);
  EXPECT(wv.find("postX(wv, m, files)") != std::string::npos);
  EXPECT(wv.find("post(wv, m)") != std::string::npos);
  EXPECT(wv.find("call.call") == std::string::npos);
  EXPECT(wv.find(".apply(") == std::string::npos);
  EXPECT(wv.find("wv.postMessage(") == std::string::npos);
  EXPECT(wv.find(s) != std::string::npos);  // the shared observer, whole
}

}  // namespace

int main() {
  TestFileDrop();
  TestFileUris();
  TestUriList();
  TestDragPaths();
  TestDialogOptions();
  TestSplitDefaultPath();
  TestDialogSlot();
  TestCfHtml();
  TestFormats();
  TestObserverScript();
  std::printf("laufey_io_test: ok\n");
  return 0;
}
