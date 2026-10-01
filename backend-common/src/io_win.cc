// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Windows drag-out (DoDragDrop with a CF_HDROP data object) and file dialogs
// (IFileOpenDialog / IFileSaveDialog), shared by the WebView2 and CEF
// backends, plus the I/O window both use.
//
// The I/O window is a message-only window on the backend's UI thread. Work is
// posted to it as a window message, which every modal loop on that thread (a
// file dialog's, DoDragDrop's, a MessageBox's) dispatches, so cancel_file_
// dialog and the test hook reach an open dialog even while the backend's own
// task queue (CEF's) is not running tasks inside the native modal loop. It is
// also the clipboard format listener.

#include "laufey_backend_common.h"
#include "laufey_io.h"

#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace laufey_common {

namespace {

constexpr UINT kRunTask = WM_APP + 71;

HWND g_io_hwnd = nullptr;
DWORD g_io_thread = 0;
bool g_clipboard_listening = false;
// WinRunOnIoThreadAfter's tasks, by timer id (I/O thread only).
std::map<UINT_PTR, std::function<void()>> g_timers;
UINT_PTR g_next_timer = 0x4C00;

LRESULT CALLBACK IoWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == kRunTask) {
    std::unique_ptr<std::function<void()>> task(
        reinterpret_cast<std::function<void()>*>(lp));
    if (task && *task)
      (*task)();
    return 0;
  }
  if (msg == WM_TIMER) {
    KillTimer(hwnd, wp);
    auto it = g_timers.find(wp);
    if (it != g_timers.end()) {
      std::function<void()> task = std::move(it->second);
      g_timers.erase(it);
      if (task)
        task();
    }
    return 0;
  }
  if (msg == WM_CLIPBOARDUPDATE) {
    FireClipboardChange();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void WinIoInit() {
  static std::once_flag once;
  std::call_once(once, [] {
    // The I/O thread: an STA with OLE (for IFileDialog and DoDragDrop) and its
    // own message loop, so a native modal loop running there (a file dialog,
    // a drag) never stalls the engine's UI thread, whatever that thread's own
    // pump does with foreign windows.
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    std::thread([&] {
      OleInitialize(nullptr);
      WNDCLASSEXW wc = {};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = IoWndProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"LaufeyIoWindow";
      RegisterClassExW(&wc);
      HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
      {
        std::lock_guard<std::mutex> lock(m);
        g_io_thread = GetCurrentThreadId();
        g_io_hwnd = hwnd;
        ready = true;
      }
      cv.notify_one();
      if (!hwnd)
        return;
      MSG msg;
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }).detach();
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return ready; });
  });
}

bool WinRunOnIoThread(std::function<void()> task) {
  WinIoInit();
  if (!g_io_hwnd)
    return false;
  if (GetCurrentThreadId() == g_io_thread) {
    task();
    return true;
  }
  auto* heap = new std::function<void()>(std::move(task));
  if (!PostMessageW(g_io_hwnd, kRunTask, 0, reinterpret_cast<LPARAM>(heap))) {
    delete heap;
    return false;
  }
  return true;
}

void WinRunOnIoThreadAfter(unsigned ms, std::function<void()> task) {
  WinRunOnIoThread([ms, task = std::move(task)]() mutable {
    UINT_PTR id = ++g_next_timer;
    g_timers[id] = std::move(task);
    SetTimer(g_io_hwnd, id, ms, nullptr);
  });
}

void ClipboardWatchWin(bool on) {
  WinRunOnIoThread([on] {
    if (on == g_clipboard_listening)
      return;
    if (on) {
      g_clipboard_listening = AddClipboardFormatListener(g_io_hwnd) != FALSE;
    } else {
      RemoveClipboardFormatListener(g_io_hwnd);
      g_clipboard_listening = false;
    }
  });
}

// ===========================================================================
// Drag out
// ===========================================================================

namespace {

bool g_dragging = false;

class FileDropSource : public IDropSource {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
      *out = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return ++refs_;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (n == 0)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape,
                                              DWORD key_state) override {
    if (escape)
      return DRAGDROP_S_CANCEL;
    if (!(key_state & (MK_LBUTTON | MK_RBUTTON)))
      return DRAGDROP_S_DROP;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

// DROPFILES + double-NUL-terminated wide paths.
HGLOBAL BuildHDrop(const std::vector<std::string>& paths) {
  std::wstring list;
  for (const auto& p : paths) {
    list += Utf8ToWide(p);
    list.push_back(L'\0');
  }
  list.push_back(L'\0');
  size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!mem)
    return nullptr;
  auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
  if (!df) {
    GlobalFree(mem);
    return nullptr;
  }
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  memcpy(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES), list.data(),
         list.size() * sizeof(wchar_t));
  GlobalUnlock(mem);
  return mem;
}

bool SetHGlobal(IDataObject* obj, CLIPFORMAT format, HGLOBAL mem) {
  FORMATETC fe = {format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM sm = {};
  sm.tymed = TYMED_HGLOBAL;
  sm.hGlobal = mem;
  if (FAILED(obj->SetData(&fe, &sm, TRUE))) {
    GlobalFree(mem);
    return false;
  }
  return true;
}

// A premultiplied 32bpp top-down DIB section from PNG bytes, for the drag
// image. Null on failure.
HBITMAP PngToBitmap(const std::vector<uint8_t>& png, SIZE* size) {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    return nullptr;
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()),
                                          static_cast<DWORD>(png.size()))))
    return nullptr;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(factory->CreateDecoderFromStream(
          stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) ||
      FAILED(factory->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)))
    return nullptr;
  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  if (w == 0 || h == 0 || w > 1024 || h > 1024)
    return nullptr;
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = static_cast<LONG>(w);
  bmi.bmiHeader.biHeight = -static_cast<LONG>(h);
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp =
      CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp)
    return nullptr;
  if (FAILED(conv->CopyPixels(nullptr, w * 4, w * h * 4,
                              static_cast<BYTE*>(bits)))) {
    DeleteObject(bmp);
    return nullptr;
  }
  size->cx = static_cast<LONG>(w);
  size->cy = static_cast<LONG>(h);
  return bmp;
}

// The shell's large icon for `path` as a 32bpp premultiplied DIB section.
HBITMAP FileIconBitmap(const std::string& path, SIZE* size) {
  SHFILEINFOW info = {};
  if (!SHGetFileInfoW(Utf8ToWide(path).c_str(), 0, &info, sizeof(info),
                      SHGFI_ICON | SHGFI_LARGEICON) ||
      !info.hIcon)
    return nullptr;
  int w = GetSystemMetrics(SM_CXICON), h = GetSystemMetrics(SM_CYICON);
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp) {
    HGDIOBJ old = SelectObject(dc, bmp);
    DrawIconEx(dc, 0, 0, info.hIcon, w, h, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    size->cx = w;
    size->cy = h;
  }
  DeleteDC(dc);
  DestroyIcon(info.hIcon);
  return bmp;
}

bool LeftButtonDown() {
  int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void RunDrag(HWND hwnd, std::unique_ptr<DragOutRequest> req) {
  // DoDragDrop needs OLE (not just COM) on this thread. The UI thread is STA;
  // OleInitialize there returns S_OK / S_FALSE, and the matching uninit is
  // never needed (process lifetime).
  static HRESULT ole = OleInitialize(nullptr);
  if (FAILED(ole) || !IsWindow(hwnd) || g_dragging || !LeftButtonDown()) {
    req->Finish(LAUFEY_DRAG_RESULT_FAILED);
    return;
  }
  ComPtr<IDataObject> data;
  if (FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr,
                                IID_PPV_ARGS(&data)))) {
    req->Finish(LAUFEY_DRAG_RESULT_FAILED);
    return;
  }
  HGLOBAL hdrop = BuildHDrop(req->paths);
  if (!hdrop || !SetHGlobal(data.Get(), CF_HDROP, hdrop)) {
    req->Finish(LAUFEY_DRAG_RESULT_FAILED);
    return;
  }
  // Tell targets the files are offered as a copy.
  CLIPFORMAT effect_fmt = static_cast<CLIPFORMAT>(
      RegisterClipboardFormatW(L"Preferred DropEffect"));
  if (HGLOBAL eff = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
    if (auto* p = static_cast<DWORD*>(GlobalLock(eff))) {
      *p = DROPEFFECT_COPY;
      GlobalUnlock(eff);
      SetHGlobal(data.Get(), effect_fmt, eff);
    } else {
      GlobalFree(eff);
    }
  }
  // The drag image.
  ComPtr<IDragSourceHelper> helper;
  if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&helper)))) {
    SIZE size = {};
    HBITMAP bmp =
        req->icon_png.empty() ? nullptr : PngToBitmap(req->icon_png, &size);
    if (!bmp)
      bmp = FileIconBitmap(req->paths[0], &size);
    if (bmp) {
      SHDRAGIMAGE image = {};
      image.sizeDragImage = size;
      image.ptOffset = {size.cx / 2, size.cy / 2};
      image.hbmpDragImage = bmp;
      image.crColorKey = CLR_NONE;
      // On success the helper owns the bitmap.
      if (FAILED(helper->InitializeFromBitmap(&image, data.Get())))
        DeleteObject(bmp);
    }
  }
  auto* source = new FileDropSource();
  DWORD effect = DROPEFFECT_NONE;
  g_dragging = true;
  HRESULT hr = DoDragDrop(data.Get(), source, DROPEFFECT_COPY, &effect);
  g_dragging = false;
  source->Release();
  if (hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE) {
    req->Finish(LAUFEY_DRAG_RESULT_DROPPED);
  } else if (hr == DRAGDROP_S_DROP || hr == DRAGDROP_S_CANCEL) {
    req->Finish(LAUFEY_DRAG_RESULT_CANCELLED);
  } else {
    req->Finish(LAUFEY_DRAG_RESULT_FAILED);
  }
}

}  // namespace

void StartFileDragWin(void* hwnd, DragOutRequest* req) {
  HWND h = static_cast<HWND>(hwnd);
  bool posted = WinRunOnIoThread(
      [h, req] { RunDrag(h, std::unique_ptr<DragOutRequest>(req)); });
  if (!posted) {
    req->Finish(LAUFEY_DRAG_RESULT_FAILED);
    delete req;
  }
}

// ===========================================================================
// File dialogs
// ===========================================================================

namespace {

const UiRunner& IoRunner() {
  static const UiRunner runner = [](std::function<void()> fn) {
    WinRunOnIoThread(std::move(fn));
  };
  return runner;
}

std::string ItemPath(IShellItem* item) {
  PWSTR wide = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide)) || !wide)
    return "";
  std::string out = WideToUtf8(wide);
  CoTaskMemFree(wide);
  return out;
}

class WinFileDialog : public FileDialogPlatform {
 public:
  WinFileDialog(uint32_t id, HWND parent, FileDialogRequest req)
      : id_(id), parent_(parent), req_(std::move(req)) {}

  bool Show() override {
    bool open = req_.kind == LAUFEY_FILE_DIALOG_OPEN;
    HRESULT hr =
        CoCreateInstance(open ? CLSID_FileOpenDialog : CLSID_FileSaveDialog,
                         nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog_));
    if (FAILED(hr))
      return false;
    FILEOPENDIALOGOPTIONS opts = 0;
    dialog_->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (open) {
      opts |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
      if (req_.ChoosesDirectories())
        opts |= FOS_PICKFOLDERS;
      if (req_.Multiple())
        opts |= FOS_ALLOWMULTISELECT;
    } else {
      opts |= FOS_PATHMUSTEXIST;
      if (req_.flags & LAUFEY_FILE_DIALOG_NO_OVERWRITE_CONFIRM)
        opts &= ~FOS_OVERWRITEPROMPT;
      else
        opts |= FOS_OVERWRITEPROMPT;
    }
    if (req_.flags & LAUFEY_FILE_DIALOG_SHOW_HIDDEN)
      opts |= FOS_FORCESHOWHIDDEN;
    dialog_->SetOptions(opts);
    if (!req_.title.empty())
      dialog_->SetTitle(Utf8ToWide(req_.title).c_str());
    if (!req_.button_label.empty())
      dialog_->SetOkButtonLabel(Utf8ToWide(req_.button_label).c_str());
    // Filters (not for folder pickers, which ignore them).
    std::vector<std::wstring> names, specs;
    if (!(opts & FOS_PICKFOLDERS)) {
      for (const auto& f : req_.filters) {
        std::wstring spec;
        for (const auto& ext : f.extensions) {
          if (!spec.empty())
            spec += L";";
          spec += ext == "*" ? L"*.*" : L"*." + Utf8ToWide(ext);
        }
        names.push_back(Utf8ToWide(f.name));
        specs.push_back(spec);
      }
    }
    if (!names.empty()) {
      std::vector<COMDLG_FILTERSPEC> fs;
      for (size_t i = 0; i < names.size(); i++)
        fs.push_back({names[i].c_str(), specs[i].c_str()});
      dialog_->SetFileTypes(static_cast<UINT>(fs.size()), fs.data());
      dialog_->SetFileTypeIndex(1);
      if (!open && !req_.filters[0].extensions.empty() &&
          req_.filters[0].extensions[0] != "*")
        dialog_->SetDefaultExtension(
            Utf8ToWide(req_.filters[0].extensions[0]).c_str());
    }
    std::string dir, name;
    SplitDefaultPath(req_.default_path, &dir, &name);
    if (!dir.empty()) {
      ComPtr<IShellItem> folder;
      if (SUCCEEDED(SHCreateItemFromParsingName(
              Utf8ToWide(dir).c_str(), nullptr, IID_PPV_ARGS(&folder))))
        dialog_->SetFolder(folder.Get());
    }
    if (!name.empty() && !open)
      dialog_->SetFileName(Utf8ToWide(name).c_str());

    HWND owner = parent_ && IsWindow(parent_) ? parent_ : nullptr;
    // Show runs the dialog's own modal loop on this thread; the I/O window's
    // messages (cancel, the test hook, their retry timers) are dispatched
    // inside it.
    hr = dialog_->Show(owner);
    shown_ = true;
    std::vector<std::string> paths;
    int status = LAUFEY_FILE_DIALOG_CANCELLED;
    if (SUCCEEDED(hr)) {
      status = LAUFEY_FILE_DIALOG_ACCEPTED;
      if (open) {
        ComPtr<IFileOpenDialog> od;
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(dialog_.As(&od)) && SUCCEEDED(od->GetResults(&items))) {
          DWORD n = 0;
          items->GetCount(&n);
          for (DWORD i = 0; i < n; i++) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(items->GetItemAt(i, &item))) {
              std::string p = ItemPath(item.Get());
              if (!p.empty())
                paths.push_back(std::move(p));
            }
          }
        }
      } else {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(dialog_->GetResult(&item))) {
          std::string p = ItemPath(item.Get());
          if (!p.empty())
            paths.push_back(std::move(p));
        }
      }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      status = LAUFEY_FILE_DIALOG_FAILED;
    }
    FileDialogFinish(id_, status, paths);
    return true;
  }

  void Cancel() override {
    if (!dialog_ || shown_)
      return;
    // Close() needs the dialog's window, and a Close() made while Show() is
    // still setting the dialog up can be lost: retry until the window
    // exists, and repeat while this dialog stays open.
    if (DialogWindow())
      dialog_->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    RetryLater([this] { Cancel(); });
  }

  bool TestAccept(const std::string& path) override {
    if (!dialog_ || shown_)
      return false;
    ScheduleAccept(path, 0);
    return true;
  }

 private:
  // The accept the test hook asked for: once the dialog window exists, give
  // the dialog a moment to finish setting up, type the path and press OK;
  // if the same dialog is still open a while later (the press came too
  // early and was dropped), try again, a few times at most.
  void ScheduleAccept(const std::string& path, int attempt) {
    if (attempt >= 5)
      return;
    uint32_t id = id_;
    WinRunOnIoThreadAfter(attempt == 0 ? 400 : 1200, [this, id, path, attempt] {
      if (!FileDialogIsOpen(id) || shown_)
        return;
      if (DialogWindow())
        DoAccept(path);
      ScheduleAccept(path, attempt + 1);
    });
  }

  // The dialog's window, or null while Show() hasn't created it.
  HWND DialogWindow() {
    ComPtr<IOleWindow> ole;
    HWND hwnd = nullptr;
    if (FAILED(dialog_.As(&ole)) || FAILED(ole->GetWindow(&hwnd)) || !hwnd ||
        !IsWindow(hwnd))
      return nullptr;
    return hwnd;
  }

  // Runs `fn` on this thread in 700 ms if this dialog is still open (at
  // most ~10 s of retries, then the action is dropped).
  void RetryLater(std::function<void()> fn) {
    if (++retries_ > 15)
      return;
    uint32_t id = id_;
    WinRunOnIoThreadAfter(700, [this, id, fn = std::move(fn)] {
      if (FileDialogIsOpen(id) && !shown_)
        fn();
    });
  }

  bool DoAccept(const std::string& path) {
    HWND hwnd = DialogWindow();
    if (!hwnd)
      return false;
    if (req_.ChoosesDirectories()) {
      // A folder picker returns the folder it shows when the name box is
      // empty (a typed path would only navigate), so go into the folder and
      // press OK once the dialog has navigated there.
      if (!path.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                Utf8ToWide(path).c_str(), nullptr, IID_PPV_ARGS(&folder))))
          dialog_->SetFolder(folder.Get());
        dialog_->SetFileName(L"");
      }
      WinRunOnIoThreadAfter(400, [hwnd] {
        if (IsWindow(hwnd))
          PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      });
      return true;
    }
    if (!path.empty())
      dialog_->SetFileName(Utf8ToWide(path).c_str());
    // Press the default button: a full path typed into the name box opens /
    // saves exactly that path.
    PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    return true;
  }

  uint32_t id_;
  HWND parent_;
  FileDialogRequest req_;
  ComPtr<IFileDialog> dialog_;
  bool shown_ = false;  // Show() returned
  int retries_ = 0;
};

}  // namespace

uint32_t ShowFileDialogWin(ParentResolver parent,
                           const laufey_file_dialog_options_t* options,
                           laufey_file_dialog_result_fn callback,
                           void* user_data) {
  WinIoInit();
  if (!g_io_hwnd) {
    if (callback)
      callback(user_data, 0, LAUFEY_FILE_DIALOG_FAILED, nullptr, 0);
    return 0;
  }
  return ShowFileDialogCommon(
      options, callback, user_data, IoRunner(),
      [parent](uint32_t id, const FileDialogRequest& request) {
        HWND owner = parent ? static_cast<HWND>(parent()) : nullptr;
        auto* dialog = new WinFileDialog(id, owner, request);
        FileDialogAttach(id, dialog);
        if (!dialog->Show())
          FileDialogFinish(id, LAUFEY_FILE_DIALOG_FAILED, {});
      });
}

bool CancelFileDialogWin(uint32_t dialog_id) {
  return FileDialogCancel(dialog_id, IoRunner());
}

bool TestFileDialogRespondWin(int action, const char* path) {
  return FileDialogTestRespond(action, path, IoRunner());
}

}  // namespace laufey_common
