// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The file paths of an external drag over a CEF window on Windows, from the
// OLE drag itself. See LaufeyNativeDragFilePaths in app.h.
//
// Chromium registers its own IDropTarget on the window (RegisterDragDrop,
// ui::DropTargetWin). laufey takes that registration over with a target of
// its own that reads the drag's CF_HDROP and forwards every call to
// Chromium's, so the page sees exactly what it saw before.

#include "app.h"

#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "laufey_backend_common.h"

namespace {

std::mutex g_paths_mutex;
std::vector<std::string> g_paths;

// The files a drag's data object carries as CF_HDROP (empty for any other
// drag).
std::vector<std::string> HDropPaths(IDataObject* data) {
  std::vector<std::string> paths;
  if (!data)
    return paths;
  FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium = {};
  if (FAILED(data->GetData(&format, &medium)))
    return paths;
  if (auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count && paths.size() < LAUFEY_MAX_DROP_PATHS; i++) {
      UINT len = DragQueryFileW(drop, i, nullptr, 0);
      std::wstring path(len, L'\0');
      if (len && DragQueryFileW(drop, i, path.data(), len + 1) == len)
        paths.push_back(laufey_common::WideToUtf8(path));
    }
    GlobalUnlock(medium.hGlobal);
  }
  ReleaseStgMedium(&medium);
  return paths;
}

// Chromium's drop target, wrapped: records the files of each drag that
// enters (and of the drop), then hands every call to Chromium unchanged.
class PathRecordingDropTarget : public IDropTarget {
 public:
  explicit PathRecordingDropTarget(IDropTarget* inner) : inner_(inner) {
    inner_->AddRef();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *out = static_cast<IDropTarget*>(this);
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

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt,
                                      DWORD* effect) override {
    Record(data);
    return inner_->DragEnter(data, keys, pt, effect);
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt,
                                     DWORD* effect) override {
    return inner_->DragOver(keys, pt, effect);
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    return inner_->DragLeave();
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt,
                                 DWORD* effect) override {
    Record(data);
    return inner_->Drop(data, keys, pt, effect);
  }

 private:
  ~PathRecordingDropTarget() {
    inner_->Release();
  }

  // Kept after the drag ends: the page reports its phases afterwards, from
  // the renderer, and a drag the page reports always entered here first.
  static void Record(IDataObject* data) {
    std::vector<std::string> paths = HDropPaths(data);
    std::lock_guard<std::mutex> lock(g_paths_mutex);
    g_paths = std::move(paths);
  }

  IDropTarget* inner_;
  ULONG refs_ = 1;
};

}  // namespace

void LaufeyHookWindowDropTarget(HWND hwnd) {
  static std::set<HWND> hooked;
  if (!hwnd || hooked.count(hwnd))
    return;
  // RegisterDragDrop keeps the window's target in this window property;
  // there is no API that returns it.
  auto* inner =
      static_cast<IDropTarget*>(GetPropW(hwnd, L"OleDropTargetInterface"));
  if (!inner) {
    std::cerr << "laufey: no drop target on the window yet; external file "
                 "drops won't carry paths"
              << std::endl;
    return;
  }
  inner->AddRef();
  auto* wrapper = new PathRecordingDropTarget(inner);
  if (SUCCEEDED(RevokeDragDrop(hwnd))) {
    if (SUCCEEDED(RegisterDragDrop(hwnd, wrapper))) {
      hooked.insert(hwnd);
    } else {
      // Put Chromium's back.
      RegisterDragDrop(hwnd, inner);
    }
  }
  wrapper->Release();
  inner->Release();
}

std::vector<std::string> LaufeyNativeDragFilePaths() {
  std::lock_guard<std::mutex> lock(g_paths_mutex);
  return g_paths;
}
