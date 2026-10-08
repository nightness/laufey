// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The portable half of drag and drop, file dialogs and the clipboard (API 39).
// See laufey_io.h.

#include "laufey_io.h"

#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "laufey_backend_common.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace laufey_common {

namespace {

// --- UTF-8 ------------------------------------------------------------------

bool IsValidUtf8(const char* s, size_t len) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  size_t i = 0;
  while (i < len) {
    unsigned char c = p[i];
    if (c < 0x80) {
      if (c == 0)
        return false;
      i++;
      continue;
    }
    size_t n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
      n = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      n = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      n = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    // The continuation bytes p[i + 1 .. i + n] must all be in range.
    if (i + n >= len)
      return false;
    for (size_t k = 1; k <= n; k++) {
      unsigned char cc = p[i + k];
      if ((cc & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    // Overlong encodings, surrogates and values beyond U+10FFFF.
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
        (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += n + 1;
  }
  return true;
}

bool CopyUtf8(const char* s, std::string* out) {
  if (!s) {
    out->clear();
    return true;
  }
  size_t len = strlen(s);
  if (!IsValidUtf8(s, len))
    return false;
  out->assign(s, len);
  return true;
}

// --- Paths -------------------------------------------------------------------

// 0: missing, 1: directory, 2: anything else that exists.
int PathKind(const std::string& p) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesW(Utf8ToWide(p).c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES)
    return 0;
  return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 2;
#else
  struct stat st;
  if (stat(p.c_str(), &st) != 0)
    return 0;
  return S_ISDIR(st.st_mode) ? 1 : 2;
#endif
}

bool IsSeparator(char c) {
#ifdef _WIN32
  return c == '\\' || c == '/';
#else
  return c == '/';
#endif
}

// --- File-drop handler
// --------------------------------------------------------

std::mutex g_drop_mutex;
laufey_file_drop_fn g_drop_handler = nullptr;
void* g_drop_user_data = nullptr;

// --- File dialog slot
// ---------------------------------------------------------

struct DialogSlot {
  uint32_t id = 0;  // 0: no dialog open
  laufey_file_dialog_result_fn callback = nullptr;
  void* user_data = nullptr;
  FileDialogPlatform* platform = nullptr;
  // The last finished dialog's platform object, deleted when the next one
  // finishes: its completion handler may still be on the stack when Finish
  // runs, so it can't be deleted there.
  std::unique_ptr<FileDialogPlatform> retired;
};

std::mutex g_dialog_mutex;
DialogSlot g_dialog;
std::atomic<uint32_t> g_next_dialog_id{1};

// --- Clipboard change handler
// --------------------------------------------------

std::mutex g_clip_mutex;
laufey_clipboard_change_fn g_clip_handler = nullptr;
void* g_clip_user_data = nullptr;

void ClipboardWatch(bool on) {
#if defined(__APPLE__)
  ClipboardWatchMac(on);
#elif defined(_WIN32)
  ClipboardWatchWin(on);
#elif defined(__linux__)
  ClipboardWatchLinux(on);
#else
  (void)on;
#endif
}

}  // namespace

// ============================================================================
// File drop
// ============================================================================

void SetFileDropHandler(laufey_file_drop_fn handler, void* user_data) {
  std::lock_guard<std::mutex> lock(g_drop_mutex);
  g_drop_handler = handler;
  g_drop_user_data = handler ? user_data : nullptr;
}

bool HasFileDropHandler() {
  std::lock_guard<std::mutex> lock(g_drop_mutex);
  return g_drop_handler != nullptr;
}

bool DispatchFileDrop(uint32_t window_id, int phase, double x, double y,
                      const std::vector<std::string>& paths, size_t count) {
  if (phase < LAUFEY_DRAG_ENTER || phase > LAUFEY_DRAG_DROP)
    return false;
  laufey_file_drop_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(g_drop_mutex);
    handler = g_drop_handler;
    user_data = g_drop_user_data;
  }
  if (!handler)
    return false;
  if (phase == LAUFEY_DRAG_LEAVE) {
    handler(user_data, window_id, phase, x, y, nullptr, 0);
    return true;
  }
  size_t n = paths.size() < LAUFEY_MAX_DROP_PATHS ? paths.size()
                                                  : LAUFEY_MAX_DROP_PATHS;
  std::vector<const char*> ptrs;
  ptrs.reserve(n);
  for (size_t i = 0; i < n; i++)
    ptrs.push_back(paths[i].c_str());
  if (phase == LAUFEY_DRAG_DROP) {
    // A drop always carries its paths (possibly none, when the source
    // offered files the engine could not resolve).
    static const char* const kNone[1] = {nullptr};
    handler(user_data, window_id, phase, x, y, n ? ptrs.data() : kNone, n);
    return true;
  }
  if (n > 0) {
    handler(user_data, window_id, phase, x, y, ptrs.data(), n);
  } else {
    size_t c = count < LAUFEY_MAX_DROP_PATHS ? count : LAUFEY_MAX_DROP_PATHS;
    handler(user_data, window_id, phase, x, y, nullptr, c);
  }
  return true;
}

bool TestTriggerFileDrop(uint32_t window_id, int phase, double x, double y,
                         const char* const* paths, size_t count) {
  std::vector<std::string> copied;
  if (paths) {
    for (size_t i = 0; i < count && i < LAUFEY_MAX_DROP_PATHS; i++) {
      if (paths[i])
        copied.emplace_back(paths[i]);
    }
  }
  return DispatchFileDrop(window_id, phase, x, y, copied, count);
}

std::vector<std::string> UriListToPaths(const std::string& list) {
  std::vector<std::string> paths;
  size_t start = 0;
  while (start < list.size() && paths.size() < LAUFEY_MAX_DROP_PATHS) {
    size_t end = list.find('\n', start);
    if (end == std::string::npos)
      end = list.size();
    std::string line = list.substr(start, end - start);
    start = end + 1;
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == ' ' || line.back() == '\0'))
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    std::string path;
    if (FileUriToPath(line, &path))
      paths.push_back(std::move(path));
  }
  return paths;
}

bool FileUriToPath(const std::string& uri, std::string* path) {
  static const char kScheme[] = "file://";
  if (uri.compare(0, sizeof(kScheme) - 1, kScheme) != 0)
    return false;
  std::string rest = uri.substr(sizeof(kScheme) - 1);
  // file://localhost/x and file:///x; any other host is not local.
  if (rest.compare(0, 9, "localhost") == 0)
    rest = rest.substr(9);
  if (rest.empty() || rest[0] != '/')
    return false;
  std::string decoded;
  decoded.reserve(rest.size());
  for (size_t i = 0; i < rest.size(); i++) {
    char c = rest[i];
    if (c == '%' && i + 2 < rest.size()) {
      auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9')
          return h - '0';
        if (h >= 'a' && h <= 'f')
          return h - 'a' + 10;
        if (h >= 'A' && h <= 'F')
          return h - 'A' + 10;
        return -1;
      };
      int hi = hex(rest[i + 1]);
      int lo = hex(rest[i + 2]);
      if (hi < 0 || lo < 0)
        return false;
      char d = static_cast<char>(hi * 16 + lo);
      if (d == '\0')
        return false;
      decoded.push_back(d);
      i += 2;
    } else if (c == '?' || c == '#') {
      break;
    } else {
      decoded.push_back(c);
    }
  }
#ifdef _WIN32
  // file:///C:/dir/x -> C:\dir\x
  if (decoded.size() >= 3 && decoded[0] == '/' && decoded[2] == ':')
    decoded = decoded.substr(1);
  for (char& ch : decoded) {
    if (ch == '/')
      ch = '\\';
  }
#endif
  if (!IsValidUtf8(decoded.data(), decoded.size()))
    return false;
  *path = std::move(decoded);
  return true;
}

// ============================================================================
// Drag out
// ============================================================================

bool ValidateDragPaths(const char* const* paths, size_t count,
                       std::vector<std::string>* out) {
  out->clear();
  if (!paths || count == 0 || count > LAUFEY_MAX_DROP_PATHS)
    return false;
  for (size_t i = 0; i < count; i++) {
    std::string p;
    if (!paths[i] || !CopyUtf8(paths[i], &p) || !IsAbsolutePath(p) ||
        PathKind(p) == 0)
      return false;
    out->push_back(std::move(p));
  }
  return true;
}

void DragOutRequest::Finish(int result) {
  if (finished)
    return;
  finished = true;
  if (callback)
    callback(user_data, result);
}

// ============================================================================
// File dialogs
// ============================================================================

bool CopyFileDialogOptions(const laufey_file_dialog_options_t* options,
                           FileDialogRequest* out) {
  if (!options)
    return false;
  if (options->kind != LAUFEY_FILE_DIALOG_OPEN &&
      options->kind != LAUFEY_FILE_DIALOG_SAVE)
    return false;
  out->kind = options->kind;
  out->flags = options->flags;
  if (!CopyUtf8(options->title, &out->title) ||
      !CopyUtf8(options->default_path, &out->default_path) ||
      !CopyUtf8(options->button_label, &out->button_label))
    return false;
  out->filters.clear();
  if (options->filters) {
    size_t total_ext = 0;
    for (size_t i = 0; i < options->filter_count &&
                       out->filters.size() < LAUFEY_FILE_DIALOG_MAX_FILTERS;
         i++) {
      const laufey_file_filter_t& f = options->filters[i];
      FileFilter copy;
      if (!CopyUtf8(f.name, &copy.name))
        return false;
      if (f.extensions) {
        for (size_t k = 0; k < f.extension_count; k++) {
          if (total_ext >= LAUFEY_FILE_DIALOG_MAX_EXTENSIONS)
            break;
          std::string ext;
          if (!f.extensions[k] || !CopyUtf8(f.extensions[k], &ext))
            continue;
          while (!ext.empty() && ext[0] == '.')
            ext.erase(0, 1);
          // Keep extensions plain: a filter pattern built from them must not
          // gain separators or wildcards the caller didn't mean.
          bool ok = !ext.empty();
          for (char c : ext) {
            if (c == ';' || c == '/' || c == '\\' || c == '|' ||
                (c == '*' && ext != "*") || c == '?')
              ok = false;
          }
          if (!ok)
            continue;
          copy.extensions.push_back(std::move(ext));
          total_ext++;
        }
      }
      if (copy.extensions.empty())
        continue;
      if (copy.name.empty())
        copy.name =
            copy.extensions[0] == "*" ? "All Files" : copy.extensions[0];
      out->filters.push_back(std::move(copy));
    }
  }
  return true;
}

FileChooserChoice ChooseFileChooser(uint32_t portal_version,
                                    const FileDialogRequest* request,
                                    const char* override_env) {
  FileChooserChoice c;
  std::string forced = override_env ? override_env : "";
  if (forced == "gtk") {
    c.reason = "LAUFEY_FILE_CHOOSER=gtk";
    return c;
  }
  if (portal_version == 0) {
    c.reason =
        "xdg-desktop-portal offers no FileChooser here (no portal runs, or "
        "the portal backend for this desktop has none, as "
        "xdg-desktop-portal-wlr alone)";
    return c;
  }
  if (request && request->ChoosesDirectories() && portal_version < 3) {
    c.reason =
        "picking a folder needs the portal's FileChooser version 3; "
        "this portal has version " +
        std::to_string(portal_version);
    return c;
  }
  c.portal = true;
  return c;
}

void SplitDefaultPath(const std::string& default_path, std::string* directory,
                      std::string* name) {
  directory->clear();
  name->clear();
  if (default_path.empty())
    return;
  if (IsAbsolutePath(default_path) && PathKind(default_path) == 1) {
    *directory = default_path;
    return;
  }
  size_t cut = std::string::npos;
  for (size_t i = default_path.size(); i > 0; i--) {
    if (IsSeparator(default_path[i - 1])) {
      cut = i - 1;
      break;
    }
  }
  if (cut == std::string::npos) {
    *name = default_path;
    return;
  }
  std::string dir = default_path.substr(0, cut == 0 ? 1 : cut);
#ifdef _WIN32
  // "C:" alone is the drive's current directory; the root is "C:\".
  if (dir.size() == 2 && dir[1] == ':')
    dir += '\\';
#endif
  *name = default_path.substr(cut + 1);
  if (IsAbsolutePath(dir) && PathKind(dir) == 1)
    *directory = dir;
}

uint32_t FileDialogBegin(laufey_file_dialog_result_fn callback,
                         void* user_data) {
  std::lock_guard<std::mutex> lock(g_dialog_mutex);
  if (g_dialog.id != 0)
    return 0;
  uint32_t id = g_next_dialog_id.fetch_add(1);
  if (id == 0)
    id = g_next_dialog_id.fetch_add(1);
  g_dialog.id = id;
  g_dialog.callback = callback;
  g_dialog.user_data = user_data;
  g_dialog.platform = nullptr;
  return id;
}

void FileDialogAttach(uint32_t dialog_id, FileDialogPlatform* platform) {
  std::lock_guard<std::mutex> lock(g_dialog_mutex);
  if (g_dialog.id == dialog_id) {
    g_dialog.platform = platform;
  } else {
    // Finished before it was attached: retire it right away.
    g_dialog.retired.reset(platform);
  }
}

void FileDialogFinish(uint32_t dialog_id, int status,
                      const std::vector<std::string>& paths) {
  laufey_file_dialog_result_fn callback;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    if (g_dialog.id != dialog_id || dialog_id == 0)
      return;
    callback = g_dialog.callback;
    user_data = g_dialog.user_data;
    if (g_dialog.platform)
      g_dialog.retired.reset(g_dialog.platform);
    g_dialog.platform = nullptr;
    g_dialog.id = 0;
    g_dialog.callback = nullptr;
    g_dialog.user_data = nullptr;
  }
  if (!callback)
    return;
  if (status == LAUFEY_FILE_DIALOG_ACCEPTED && !paths.empty()) {
    std::vector<const char*> ptrs;
    ptrs.reserve(paths.size());
    for (const auto& p : paths)
      ptrs.push_back(p.c_str());
    callback(user_data, dialog_id, status, ptrs.data(), ptrs.size());
  } else {
    // ACCEPTED with nothing selected reads as a cancel.
    int s = status == LAUFEY_FILE_DIALOG_ACCEPTED ? LAUFEY_FILE_DIALOG_CANCELLED
                                                  : status;
    callback(user_data, dialog_id, s, nullptr, 0);
  }
}

bool FileDialogIsOpen(uint32_t dialog_id) {
  std::lock_guard<std::mutex> lock(g_dialog_mutex);
  return dialog_id != 0 && g_dialog.id == dialog_id;
}

namespace {

// Runs `fn(platform)` on the UI thread if dialog `dialog_id` (0: whichever is
// open) is still open there. Returns whether a dialog was open at the call.
bool WithOpenDialog(uint32_t dialog_id, const UiRunner& run_on_ui,
                    std::function<void(FileDialogPlatform*)> fn) {
  uint32_t target;
  {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    if (g_dialog.id == 0 || (dialog_id != 0 && g_dialog.id != dialog_id))
      return false;
    target = g_dialog.id;
  }
  run_on_ui([target, fn = std::move(fn)] {
    FileDialogPlatform* platform = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_dialog_mutex);
      if (g_dialog.id == target)
        platform = g_dialog.platform;
    }
    // Finish runs on this thread too, so the dialog can't be retired between
    // the check and the call.
    if (platform)
      fn(platform);
  });
  return true;
}

}  // namespace

bool FileDialogCancel(uint32_t dialog_id, const UiRunner& run_on_ui) {
  if (dialog_id == 0)
    return false;
  return WithOpenDialog(dialog_id, run_on_ui,
                        [](FileDialogPlatform* p) { p->Cancel(); });
}

bool FileDialogTestRespond(int action, const char* path,
                           const UiRunner& run_on_ui) {
  if (action != LAUFEY_TEST_DIALOG_CANCEL &&
      action != LAUFEY_TEST_DIALOG_ACCEPT)
    return false;
  {
    // Only once the platform dialog exists (is attached) can it be driven;
    // a test polls until then.
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    if (g_dialog.id == 0 || !g_dialog.platform)
      return false;
  }
  std::string p = path ? path : "";
  if (action == LAUFEY_TEST_DIALOG_CANCEL) {
    return WithOpenDialog(0, run_on_ui,
                          [](FileDialogPlatform* d) { d->Cancel(); });
  }
  return WithOpenDialog(0, run_on_ui, [p](FileDialogPlatform* d) {
    if (!d->TestAccept(p))
      d->Cancel();
  });
}

uint32_t ShowFileDialogCommon(
    const laufey_file_dialog_options_t* options,
    laufey_file_dialog_result_fn callback, void* user_data,
    const UiRunner& run_on_ui,
    std::function<void(uint32_t, const FileDialogRequest&)> show) {
  if (!callback)
    return 0;
  FileDialogRequest request;
  if (!CopyFileDialogOptions(options, &request)) {
    callback(user_data, 0, LAUFEY_FILE_DIALOG_FAILED, nullptr, 0);
    return 0;
  }
  uint32_t id = FileDialogBegin(callback, user_data);
  if (id == 0) {
    callback(user_data, 0, LAUFEY_FILE_DIALOG_BUSY, nullptr, 0);
    return 0;
  }
  run_on_ui([id, request = std::move(request), show = std::move(show)] {
    // cancel_file_dialog may have arrived before the dialog was shown.
    if (!FileDialogIsOpen(id))
      return;
    show(id, request);
  });
  return id;
}

// ============================================================================
// Clipboard
// ============================================================================

void SetClipboardChangeHandler(laufey_clipboard_change_fn handler,
                               void* user_data) {
  bool was_on;
  {
    std::lock_guard<std::mutex> lock(g_clip_mutex);
    was_on = g_clip_handler != nullptr;
    g_clip_handler = handler;
    g_clip_user_data = handler ? user_data : nullptr;
  }
  bool on = handler != nullptr;
  if (on != was_on)
    ClipboardWatch(on);
}

void FireClipboardChange() {
  laufey_clipboard_change_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(g_clip_mutex);
    handler = g_clip_handler;
    user_data = g_clip_user_data;
  }
  if (handler)
    handler(user_data);
}

char* JoinClipboardFormats(const std::vector<std::string>& formats) {
  std::string joined;
  std::vector<std::string> seen;
  for (const auto& f : formats) {
    bool dup = false;
    for (const auto& s : seen) {
      if (s == f)
        dup = true;
    }
    if (dup || f.empty())
      continue;
    seen.push_back(f);
    if (!joined.empty())
      joined += '\n';
    joined += f;
  }
  char* out = static_cast<char*>(malloc(joined.size() + 1));
  if (!out)
    return nullptr;
  memcpy(out, joined.c_str(), joined.size() + 1);
  return out;
}

namespace {

size_t ParseCfHtmlOffset(const std::string& s, const char* key) {
  size_t at = s.find(key);
  if (at == std::string::npos)
    return std::string::npos;
  at += strlen(key);
  size_t end = at;
  while (end < s.size() && s[end] >= '0' && s[end] <= '9')
    end++;
  if (end == at || end - at > 12)
    return std::string::npos;
  return static_cast<size_t>(std::stoull(s.substr(at, end - at)));
}

}  // namespace

std::string BuildCfHtml(const std::string& fragment) {
  // Offsets are byte offsets into the UTF-8 payload, written as fixed-width
  // decimals so the header length doesn't depend on them.
  static const char kHeader[] =
      "Version:0.9\r\n"
      "StartHTML:%010zu\r\n"
      "EndHTML:%010zu\r\n"
      "StartFragment:%010zu\r\n"
      "EndFragment:%010zu\r\n";
  static const char kPrefix[] = "<html><body>\r\n<!--StartFragment-->";
  static const char kSuffix[] = "<!--EndFragment-->\r\n</body></html>";
  char probe[256];
  int header_len = snprintf(probe, sizeof(probe), kHeader, size_t{0}, size_t{0},
                            size_t{0}, size_t{0});
  size_t start_html = static_cast<size_t>(header_len);
  size_t start_fragment = start_html + strlen(kPrefix);
  size_t end_fragment = start_fragment + fragment.size();
  size_t end_html = end_fragment + strlen(kSuffix);
  char header[256];
  snprintf(header, sizeof(header), kHeader, start_html, end_html,
           start_fragment, end_fragment);
  std::string out = header;
  out += kPrefix;
  out += fragment;
  out += kSuffix;
  return out;
}

bool ExtractCfHtmlFragment(const std::string& cf_html, std::string* html) {
  // The payload may carry a trailing NUL (and slack) from GlobalSize.
  std::string s = cf_html.substr(0, strnlen(cf_html.data(), cf_html.size()));
  size_t sf = ParseCfHtmlOffset(s, "StartFragment:");
  size_t ef = ParseCfHtmlOffset(s, "EndFragment:");
  if (sf != std::string::npos && ef != std::string::npos && sf <= ef &&
      ef <= s.size()) {
    *html = s.substr(sf, ef - sf);
    return true;
  }
  size_t sh = ParseCfHtmlOffset(s, "StartHTML:");
  size_t eh = ParseCfHtmlOffset(s, "EndHTML:");
  if (sh != std::string::npos && eh != std::string::npos && sh <= eh &&
      eh <= s.size()) {
    *html = s.substr(sh, eh - sh);
    return true;
  }
  return false;
}

bool LooksLikePng(const uint8_t* data, size_t len) {
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  return data && len > 8 && memcmp(data, kSig, 8) == 0;
}

uint8_t* MallocCopy(const void* data, size_t len) {
  if (!data || len == 0)
    return nullptr;
  uint8_t* out = static_cast<uint8_t*>(malloc(len));
  if (out)
    memcpy(out, data, len);
  return out;
}

}  // namespace laufey_common
