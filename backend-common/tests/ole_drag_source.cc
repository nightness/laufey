// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// An OLE drag source for the Windows native e2e (io_checks.rs): a small
// window whose whole area drags the files named on the command line as
// CF_HDROP with DoDragDrop, the way Explorer does. The battery starts it,
// drives a real drag out of it into a laufey window with SendInput, and
// checks that the drop reaches on_file_drop through the backend's own drop
// handling.
//
//   laufey_ole_drag_source <x> <y> <path>...
//
// Prints "ready <x> <y> <width> <height>" (the window's client area in
// physical screen pixels) once it is shown, then "pressed", "drag-begin"
// and "drag-end <dropped|cancelled|failed>" as the drag goes, and exits
// after a drag or after 60 seconds.

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::wstring> g_paths;

class DropSource : public IDropSource {
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
                                              DWORD keys) override {
    if (escape)
      return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON))
      return DRAGDROP_S_DROP;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }

 private:
  ULONG refs_ = 1;
};

// DROPFILES + the double-NUL-terminated wide paths.
HGLOBAL BuildHDrop() {
  std::wstring list;
  for (const auto& p : g_paths) {
    list += p;
    list.push_back(L'\0');
  }
  list.push_back(L'\0');
  size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!mem)
    return nullptr;
  auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  std::memcpy(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES), list.data(),
              list.size() * sizeof(wchar_t));
  GlobalUnlock(mem);
  return mem;
}

void Drag() {
  std::printf("drag-begin\n");
  std::fflush(stdout);
  IDataObject* data = nullptr;
  const char* result = "failed";
  if (SUCCEEDED(SHCreateDataObject(nullptr, 0, nullptr, nullptr,
                                   IID_PPV_ARGS(&data)))) {
    FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm = {};
    sm.tymed = TYMED_HGLOBAL;
    sm.hGlobal = BuildHDrop();
    if (sm.hGlobal && SUCCEEDED(data->SetData(&fe, &sm, TRUE))) {
      auto* source = new DropSource();
      DWORD effect = DROPEFFECT_NONE;
      HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
      source->Release();
      if (hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE)
        result = "dropped";
      else if (hr == DRAGDROP_S_DROP || hr == DRAGDROP_S_CANCEL)
        result = "cancelled";
    }
    data->Release();
  }
  std::printf("drag-end %s\n", result);
  std::fflush(stdout);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_LBUTTONDOWN:
      std::printf("pressed\n");
      std::fflush(stdout);
      Drag();
      PostQuitMessage(0);
      return 0;
    case WM_TIMER:
      PostQuitMessage(1);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: laufey_ole_drag_source <x> <y> <path>...\n");
    return 2;
  }
  // Physical pixels, like the battery's SendInput coordinates.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (FAILED(OleInitialize(nullptr)))
    return 1;
  int x = _wtoi(argv[1]), y = _wtoi(argv[2]);
  for (int i = 3; i < argc; i++)
    g_paths.emplace_back(argv[i]);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_HIGHLIGHT + 1);
  wc.lpszClassName = L"LaufeyOleDragSource";
  RegisterClassExW(&wc);
  HWND hwnd =
      CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
                      L"laufey-ole-drag-source", WS_POPUP, x, y, 160, 120,
                      nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd)
    return 1;
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  UpdateWindow(hwnd);
  SetTimer(hwnd, 1, 60000, nullptr);
  RECT rect;
  POINT origin = {0, 0};
  GetClientRect(hwnd, &rect);
  ClientToScreen(hwnd, &origin);
  std::printf("ready %ld %ld %ld %ld\n", origin.x, origin.y,
              rect.right - rect.left, rect.bottom - rect.top);
  std::fflush(stdout);

  MSG msg;
  int code = 0;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  code = static_cast<int>(msg.wParam);
  DestroyWindow(hwnd);
  OleUninitialize();
  return code;
}
