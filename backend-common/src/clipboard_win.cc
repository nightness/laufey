// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Win32 clipboard: CF_UNICODETEXT text, CF_HTML ("HTML Format") HTML, PNG
// images (the registered "PNG" format plus CF_DIBV5, which Windows also
// synthesizes as CF_DIB / CF_BITMAP for older apps), the formats present, and
// change events (AddClipboardFormatListener on the I/O window, io_win.cc).
// Text is stored UTF-8 in the laufey ABI and converted to/from UTF-16 here.
//
// The Win32 clipboard works from any thread, so these run on the caller's;
// OpenClipboard is retried for a moment while another app holds it. Image
// conversion uses WIC, initializing COM on the calling thread if needed.

#include "laufey_backend_common.h"
#include "laufey_io.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace laufey_common {

namespace {

UINT HtmlFormat() {
  static UINT f = RegisterClipboardFormatW(L"HTML Format");
  return f;
}
UINT PngFormat() {
  static UINT f = RegisterClipboardFormatW(L"PNG");
  return f;
}
UINT RtfFormat() {
  static UINT f = RegisterClipboardFormatW(L"Rich Text Format");
  return f;
}

// OpenClipboard fails while another window has it open; that is normally a
// few milliseconds, so retry for up to ~250 ms before giving up.
bool OpenClipboardRetry() {
  for (int i = 0; i < 25; i++) {
    if (OpenClipboard(nullptr))
      return true;
    Sleep(10);
  }
  return false;
}

struct ClipboardScope {
  bool open;
  ClipboardScope() : open(OpenClipboardRetry()) {}
  ~ClipboardScope() {
    if (open)
      CloseClipboard();
  }
};

// COM for WIC on whatever thread called us (the runtime's worker threads have
// none). An STA thread keeps its apartment.
struct ComScope {
  bool uninit = false;
  ComScope() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uninit = SUCCEEDED(hr);  // S_OK or S_FALSE; RPC_E_CHANGED_MODE is fine too
  }
  ~ComScope() {
    if (uninit)
      CoUninitialize();
  }
};

char* WideToUtf8Capped(const wchar_t* w, size_t wlen) {
  if (!w)
    return nullptr;
  if (wlen > LAUFEY_CLIPBOARD_MAX_READ_BYTES)
    return nullptr;
  int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(wlen), nullptr, 0,
                              nullptr, nullptr);
  if (n < 0 || static_cast<size_t>(n) > LAUFEY_CLIPBOARD_MAX_READ_BYTES)
    return nullptr;
  char* out = static_cast<char*>(malloc(static_cast<size_t>(n) + 1));
  if (!out)
    return nullptr;
  if (n > 0)
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(wlen), out, n, nullptr,
                        nullptr);
  out[n] = '\0';
  return out;
}

// Copies a clipboard global into a byte vector (capped). The clipboard must be
// open.
bool ReadGlobal(UINT format, std::vector<uint8_t>* out) {
  HANDLE handle = GetClipboardData(format);
  if (!handle)
    return false;
  SIZE_T size = GlobalSize(handle);
  if (size == 0 || size > LAUFEY_CLIPBOARD_MAX_READ_BYTES)
    return false;
  const void* p = GlobalLock(handle);
  if (!p)
    return false;
  out->assign(static_cast<const uint8_t*>(p),
              static_cast<const uint8_t*>(p) + size);
  GlobalUnlock(handle);
  return true;
}

// Puts `len` bytes on the open clipboard as `format`.
bool SetGlobal(UINT format, const void* data, size_t len) {
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len);
  if (!mem)
    return false;
  void* dst = GlobalLock(mem);
  if (!dst) {
    GlobalFree(mem);
    return false;
  }
  memcpy(dst, data, len);
  GlobalUnlock(mem);
  // Ownership of `mem` transfers to the system on success.
  if (!SetClipboardData(format, mem)) {
    GlobalFree(mem);
    return false;
  }
  return true;
}

bool SetUnicodeText(const std::string& text) {
  std::wstring wide = Utf8ToWide(text);
  return SetGlobal(CF_UNICODETEXT, wide.c_str(),
                   (wide.size() + 1) * sizeof(wchar_t));
}

// --- WIC
// ----------------------------------------------------------------------

ComPtr<IWICImagingFactory> WicFactory() {
  ComPtr<IWICImagingFactory> factory;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                   IID_PPV_ARGS(&factory));
  return factory;
}

// Decodes any WIC-readable image (`bytes`) to its first frame.
ComPtr<IWICBitmapSource> Decode(IWICImagingFactory* factory,
                                const uint8_t* bytes, size_t len) {
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes),
                                          static_cast<DWORD>(len))))
    return nullptr;
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(
          stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)))
    return nullptr;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame)))
    return nullptr;
  // Detach from the stream (which points at `bytes`) before returning.
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(factory->CreateBitmapFromSource(frame.Get(), WICBitmapCacheOnLoad,
                                             &bitmap)))
    return nullptr;
  return bitmap;
}

bool EncodePng(IWICImagingFactory* factory, IWICBitmapSource* source,
               std::vector<uint8_t>* out) {
  ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
    return false;
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(
          factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
    return false;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
      FAILED(frame->Initialize(nullptr)))
    return false;
  UINT w = 0, h = 0;
  source->GetSize(&w, &h);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt)))
    return false;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(factory->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(source, GUID_WICPixelFormat32bppBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)))
    return false;
  if (FAILED(frame->WriteSource(conv.Get(), nullptr)) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    return false;
  HGLOBAL mem = nullptr;
  if (FAILED(GetHGlobalFromStream(stream.Get(), &mem)))
    return false;
  STATSTG stat = {};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)))
    return false;
  size_t size = static_cast<size_t>(stat.cbSize.QuadPart);
  if (size == 0 || size > LAUFEY_CLIPBOARD_MAX_READ_BYTES)
    return false;
  const void* p = GlobalLock(mem);
  if (!p)
    return false;
  out->assign(static_cast<const uint8_t*>(p),
              static_cast<const uint8_t*>(p) + size);
  GlobalUnlock(mem);
  return true;
}

// A CF_DIB / CF_DIBV5 payload as a BMP file, so WIC's BMP decoder handles
// every bit depth, palette and compression the clipboard may hold.
bool DibToBmpFile(const std::vector<uint8_t>& dib, std::vector<uint8_t>* bmp) {
  if (dib.size() < sizeof(BITMAPINFOHEADER))
    return false;
  BITMAPINFOHEADER hdr;
  memcpy(&hdr, dib.data(), sizeof(hdr));
  if (hdr.biSize < sizeof(BITMAPINFOHEADER) || hdr.biSize > dib.size())
    return false;
  size_t colors = hdr.biClrUsed;
  if (colors == 0 && hdr.biBitCount <= 8)
    colors = size_t{1} << hdr.biBitCount;
  size_t masks = (hdr.biCompression == BI_BITFIELDS &&
                  hdr.biSize == sizeof(BITMAPINFOHEADER))
                     ? 12
                     : 0;
  size_t offset = sizeof(BITMAPFILEHEADER) + hdr.biSize + masks + colors * 4;
  if (offset - sizeof(BITMAPFILEHEADER) > dib.size())
    return false;
  BITMAPFILEHEADER file = {};
  file.bfType = 0x4D42;  // "BM"
  file.bfSize = static_cast<DWORD>(sizeof(file) + dib.size());
  file.bfOffBits = static_cast<DWORD>(offset);
  bmp->resize(sizeof(file) + dib.size());
  memcpy(bmp->data(), &file, sizeof(file));
  memcpy(bmp->data() + sizeof(file), dib.data(), dib.size());
  return true;
}

// A CF_DIBV5 (top-down 32bpp BGRA with alpha) from a PNG.
bool PngToDibV5(IWICImagingFactory* factory, const uint8_t* png, size_t len,
                std::vector<uint8_t>* dib) {
  ComPtr<IWICBitmapSource> source = Decode(factory, png, len);
  if (!source)
    return false;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(factory->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)))
    return false;
  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  if (w == 0 || h == 0 || w > 32768 || h > 32768)
    return false;
  size_t stride = static_cast<size_t>(w) * 4;
  size_t pixels = stride * h;
  dib->assign(sizeof(BITMAPV5HEADER) + pixels, 0);
  BITMAPV5HEADER hdr = {};
  hdr.bV5Size = sizeof(hdr);
  hdr.bV5Width = static_cast<LONG>(w);
  hdr.bV5Height = -static_cast<LONG>(h);  // top-down
  hdr.bV5Planes = 1;
  hdr.bV5BitCount = 32;
  hdr.bV5Compression = BI_BITFIELDS;
  hdr.bV5SizeImage = static_cast<DWORD>(pixels);
  hdr.bV5RedMask = 0x00FF0000;
  hdr.bV5GreenMask = 0x0000FF00;
  hdr.bV5BlueMask = 0x000000FF;
  hdr.bV5AlphaMask = 0xFF000000;
  hdr.bV5CSType = LCS_sRGB;
  hdr.bV5Intent = LCS_GM_IMAGES;
  memcpy(dib->data(), &hdr, sizeof(hdr));
  return SUCCEEDED(conv->CopyPixels(nullptr, static_cast<UINT>(stride),
                                    static_cast<UINT>(pixels),
                                    dib->data() + sizeof(hdr)));
}

}  // namespace

char* ClipboardReadTextWin() {
  ClipboardScope clip;
  if (!clip.open)
    return nullptr;
  char* result = nullptr;
  HANDLE handle = GetClipboardData(CF_UNICODETEXT);
  if (handle) {
    SIZE_T bytes = GlobalSize(handle);
    const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(handle));
    if (w) {
      size_t max_chars = bytes / sizeof(wchar_t);
      result = WideToUtf8Capped(w, wcsnlen(w, max_chars));
      GlobalUnlock(handle);
    }
  }
  return result;
}

void ClipboardWriteTextWin(const std::string& text) {
  ClipboardScope clip;
  if (!clip.open)
    return;
  EmptyClipboard();
  SetUnicodeText(text);
}

uint32_t ClipboardCapabilitiesWin() {
  return LAUFEY_CLIPBOARD_CAP_TEXT | LAUFEY_CLIPBOARD_CAP_HTML |
         LAUFEY_CLIPBOARD_CAP_IMAGE | LAUFEY_CLIPBOARD_CAP_FORMATS |
         LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS;
}

char* ClipboardReadHtmlWin() {
  std::vector<uint8_t> raw;
  {
    ClipboardScope clip;
    if (!clip.open || !ReadGlobal(HtmlFormat(), &raw))
      return nullptr;
  }
  std::string html;
  if (!ExtractCfHtmlFragment(std::string(raw.begin(), raw.end()), &html))
    return nullptr;
  char* out = static_cast<char*>(malloc(html.size() + 1));
  if (!out)
    return nullptr;
  memcpy(out, html.c_str(), html.size() + 1);
  return out;
}

bool ClipboardWriteHtmlWin(const std::string& html, const char* text_or_null) {
  std::string cf = BuildCfHtml(html);
  ClipboardScope clip;
  if (!clip.open)
    return false;
  EmptyClipboard();
  // CF_HTML is NUL-terminated UTF-8.
  bool ok = SetGlobal(HtmlFormat(), cf.c_str(), cf.size() + 1);
  if (ok && text_or_null)
    ok = SetUnicodeText(text_or_null);
  return ok;
}

uint8_t* ClipboardReadImageWin(size_t* len_out) {
  if (len_out)
    *len_out = 0;
  std::vector<uint8_t> png, dib;
  {
    ClipboardScope clip;
    if (!clip.open)
      return nullptr;
    if (!ReadGlobal(PngFormat(), &png) ||
        !LooksLikePng(png.data(), png.size())) {
      png.clear();
      if (!ReadGlobal(CF_DIBV5, &dib))
        ReadGlobal(CF_DIB, &dib);
    }
  }
  if (png.empty()) {
    if (dib.empty())
      return nullptr;
    std::vector<uint8_t> bmp;
    if (!DibToBmpFile(dib, &bmp))
      return nullptr;
    ComScope com;
    ComPtr<IWICImagingFactory> factory = WicFactory();
    if (!factory)
      return nullptr;
    ComPtr<IWICBitmapSource> source =
        Decode(factory.Get(), bmp.data(), bmp.size());
    if (!source || !EncodePng(factory.Get(), source.Get(), &png))
      return nullptr;
  }
  uint8_t* out = MallocCopy(png.data(), png.size());
  if (out && len_out)
    *len_out = png.size();
  return out;
}

bool ClipboardWriteImageWin(const uint8_t* png, size_t len) {
  if (!LooksLikePng(png, len))
    return false;
  std::vector<uint8_t> dib;
  {
    ComScope com;
    ComPtr<IWICImagingFactory> factory = WicFactory();
    if (!factory || !PngToDibV5(factory.Get(), png, len, &dib))
      return false;
  }
  ClipboardScope clip;
  if (!clip.open)
    return false;
  EmptyClipboard();
  bool ok = SetGlobal(PngFormat(), png, len);
  if (ok)
    ok = SetGlobal(CF_DIBV5, dib.data(), dib.size());
  return ok;
}

char* ClipboardReadFormatsWin() {
  std::vector<std::string> formats;
  ClipboardScope clip;
  if (!clip.open)
    return nullptr;
  UINT f = 0;
  while ((f = EnumClipboardFormats(f)) != 0) {
    if (f == CF_UNICODETEXT || f == CF_TEXT || f == CF_OEMTEXT) {
      formats.push_back("text/plain");
    } else if (f == HtmlFormat()) {
      formats.push_back("text/html");
    } else if (f == PngFormat() || f == CF_DIB || f == CF_DIBV5 ||
               f == CF_BITMAP) {
      formats.push_back("image/png");
    } else if (f == CF_HDROP) {
      formats.push_back("text/uri-list");
    } else if (f == RtfFormat()) {
      formats.push_back("text/rtf");
    }
  }
  return JoinClipboardFormats(formats);
}

}  // namespace laufey_common
