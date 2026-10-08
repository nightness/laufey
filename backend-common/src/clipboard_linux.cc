// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// GTK3 clipboard (the CLIPBOARD selection): text, HTML (text/html, with a
// plain-text alternative), PNG images (image/png verbatim plus every pixbuf
// format GTK offers), the targets present, and owner-change events. Both
// backends (CEF, webview) already link and initialize GTK on Linux, so we use
// the in-process clipboard rather than shelling out to xclip / wl-clipboard.
//
// GTK is not thread-safe: every call runs on the GLib default main context
// (GtkRunSync), whichever thread it came from. The gtk_clipboard_wait_*
// calls spin a nested main loop there until the owning app answers.
//
// Wayland: the core protocol lets only the client with keyboard focus set or
// read the selection, and laufey's GTK connection is never that client under
// CEF (Chromium's own connection owns the window), nor while the app is in
// the background. Where the compositor offers ext-data-control-v1 (KWin,
// wlroots) every operation goes through it instead, focused or not
// (clipboard_data_control_linux.h). GNOME's mutter has no data-control, but
// its Xwayland selection bridge copies the clipboard between X11 and Wayland
// both ways whatever has focus, so there laufey is an X11 client of
// Xwayland's CLIPBOARD selection over a private XCB connection
// (clipboard_x11_linux.h), GTK and the environment untouched.

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include "clipboard_data_control_linux.h"
#include "clipboard_x11_linux.h"
#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "session_lock_linux.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

GtkClipboard* Clipboard() {
  return gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
}

char* MallocString(const char* data, size_t len) {
  char* out = static_cast<char*>(malloc(len + 1));
  if (!out)
    return nullptr;
  memcpy(out, data, len);
  out[len] = '\0';
  return out;
}

// Lets a clipboard manager keep everything we offer after we exit.
void StoreAll(GtkClipboard* clipboard) {
  gtk_clipboard_set_can_store(clipboard, nullptr, 0);
  gtk_clipboard_store(clipboard);
}

// What we own while we hold the clipboard (set_with_data user data).
struct Offer {
  std::string html;
  bool has_text = false;
  std::string text;
  std::vector<uint8_t> png;
  GdkPixbuf* pixbuf = nullptr;
  ~Offer() {
    if (pixbuf)
      g_object_unref(pixbuf);
  }
};

enum OfferInfo : guint { kInfoHtml = 1, kInfoText, kInfoPng, kInfoImage };

void OfferGet(GtkClipboard*, GtkSelectionData* data, guint info,
              gpointer user_data) {
  auto* offer = static_cast<Offer*>(user_data);
  switch (info) {
    case kInfoHtml:
      gtk_selection_data_set(
          data, gtk_selection_data_get_target(data), 8,
          reinterpret_cast<const guchar*>(offer->html.data()),
          static_cast<gint>(offer->html.size()));
      break;
    case kInfoText:
      gtk_selection_data_set_text(data, offer->text.c_str(),
                                  static_cast<gint>(offer->text.size()));
      break;
    case kInfoPng:
      gtk_selection_data_set(data, gtk_selection_data_get_target(data), 8,
                             offer->png.data(),
                             static_cast<gint>(offer->png.size()));
      break;
    case kInfoImage:
      if (offer->pixbuf)
        gtk_selection_data_set_pixbuf(data, offer->pixbuf);
      break;
  }
}

void OfferClear(GtkClipboard*, gpointer user_data) {
  delete static_cast<Offer*>(user_data);
}

bool SetOffer(GtkTargetList* list, Offer* offer) {
  gint n = 0;
  GtkTargetEntry* entries = gtk_target_table_new_from_list(list, &n);
  GtkClipboard* clipboard = Clipboard();
  bool ok = clipboard && gtk_clipboard_set_with_data(
                             clipboard, entries, static_cast<guint>(n),
                             OfferGet, OfferClear, offer);
  gtk_target_table_free(entries, n);
  gtk_target_list_unref(list);
  if (!ok) {
    delete offer;
    return false;
  }
  StoreAll(clipboard);
  return true;
}

// text/html as the source wrote it: UTF-8, or UTF-16 with a BOM (Firefox
// historically), trailing NULs dropped.
bool DecodeHtml(const guchar* data, gint len, std::string* out) {
  if (!data || len <= 0 ||
      static_cast<size_t>(len) > LAUFEY_CLIPBOARD_MAX_READ_BYTES)
    return false;
  if (len >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) ||
                   (data[0] == 0xFE && data[1] == 0xFF))) {
    gsize written = 0;
    gchar* utf8 = g_convert(reinterpret_cast<const gchar*>(data), len, "UTF-8",
                            "UTF-16", nullptr, &written, nullptr);
    if (!utf8)
      return false;
    out->assign(utf8, written);
    g_free(utf8);
  } else {
    out->assign(reinterpret_cast<const char*>(data), static_cast<size_t>(len));
  }
  while (!out->empty() && out->back() == '\0')
    out->pop_back();
  // The BOM, if g_convert kept it.
  if (out->compare(0, 3, "\xEF\xBB\xBF") == 0)
    out->erase(0, 3);
  return g_utf8_validate(out->data(), static_cast<gssize>(out->size()),
                         nullptr) != FALSE;
}

std::vector<uint8_t> PixbufToPng(GdkPixbuf* pixbuf) {
  std::vector<uint8_t> out;
  gchar* buffer = nullptr;
  gsize size = 0;
  if (pixbuf && gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &size, "png",
                                          nullptr, nullptr)) {
    if (size <= LAUFEY_CLIPBOARD_MAX_READ_BYTES)
      out.assign(reinterpret_cast<uint8_t*>(buffer),
                 reinterpret_cast<uint8_t*>(buffer) + size);
    g_free(buffer);
  }
  return out;
}

gulong g_owner_change_id = 0;

void OnOwnerChange(GtkClipboard*, GdkEvent*, gpointer) {
  FireClipboardChange();
}

// --- The clipboard without focus (Wayland) ---------------------------------
//
// ext-data-control where the compositor offers it, else (mutter) the X11
// CLIPBOARD of Xwayland. Each answers false when it isn't in use here, and
// the caller then uses GTK's clipboard.
//
// Privacy: these paths read the clipboard whatever has focus, so an app in
// the background reads it while the session is unlocked (as on macOS and
// Windows, and as Electron does). While the session is locked (logind's
// LockedHint) every read through them is refused: it answers "nothing
// there", and the reason is logged once per lock.

bool FreeAvailable() {
  return data_control::Available() || x11_clipboard::Available();
}

// True (and the reason logged, once per lock) when a read must be refused.
bool ReadRefused() {
  static std::atomic<bool> logged{false};
  if (!SessionLockedLinux()) {
    logged = false;
    return false;
  }
  if (!logged.exchange(true)) {
    g_warning(
        "laufey: clipboard read refused: the session is locked (reads "
        "without focus are refused while the screen is locked)");
  }
  return true;
}

bool FreeTypes(std::vector<std::string>* out) {
  if (FreeAvailable() && ReadRefused()) {
    out->clear();
    return true;
  }
  return data_control::Types(out) || x11_clipboard::Types(out);
}

// *type_out: what the data is (the MIME type read, or the X11 property
// type the owner answered with).
bool FreeRead(const std::vector<std::string>& mimes, std::string* out,
              std::string* mime_out, bool* found,
              std::string* type_out = nullptr) {
  if (FreeAvailable() && ReadRefused()) {
    *found = false;
    return true;
  }
  std::string mime;
  if (data_control::Read(mimes, LAUFEY_CLIPBOARD_MAX_READ_BYTES, out, &mime,
                         found)) {
    if (type_out)
      *type_out = mime;
  } else {
    std::string type;
    if (!x11_clipboard::Read(mimes, LAUFEY_CLIPBOARD_MAX_READ_BYTES, out, &mime,
                             found, &type))
      return false;
    if (type_out)
      *type_out = type;
  }
  if (mime_out)
    *mime_out = mime;
  return true;
}

bool FreeWrite(const data_control::Entries& entries) {
  return data_control::Write(entries) || x11_clipboard::Write(entries);
}

// The types text is read as, in order of preference. (TEXT is left out: its
// owner may answer in COMPOUND_TEXT.)
const std::vector<std::string>& TextTypes() {
  static const std::vector<std::string> types = {
      "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING"};
  return types;
}

std::shared_ptr<const std::string> Shared(std::string s) {
  return std::make_shared<const std::string>(std::move(s));
}

// Text as UTF-8 under its UTF-8 types, and as STRING only when it converts
// to Latin-1 exactly (ICCCM: STRING is ISO 8859-1; text with a character
// outside it isn't offered as STRING at all).
void AddTextEntries(data_control::Entries* entries, const std::string& text) {
  auto data = Shared(text);
  for (const char* t :
       {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"})
    entries->push_back({t, data});
  gsize written = 0;
  gchar* latin1 = g_convert(text.data(), static_cast<gssize>(text.size()),
                            "ISO-8859-1", "UTF-8", nullptr, &written, nullptr);
  if (latin1) {
    entries->push_back({"STRING", Shared(std::string(latin1, written))});
    g_free(latin1);
  }
}

// The selection as UTF-8 text without focus; false when that isn't in use.
bool FreeText(char** result) {
  std::string data, type;
  bool found = false;
  if (!FreeRead(TextTypes(), &data, nullptr, &found, &type))
    return false;
  *result = nullptr;
  if (!found)
    return true;
  while (!data.empty() && data.back() == '\0')
    data.pop_back();
  if (type == "STRING" ||
      !g_utf8_validate(data.data(), static_cast<gssize>(data.size()),
                       nullptr)) {
    // STRING is Latin-1 (ICCCM); anything else invalid is dropped.
    if (type != "STRING")
      return true;
    gsize written = 0;
    gchar* utf8 = g_convert(data.data(), static_cast<gssize>(data.size()),
                            "UTF-8", "ISO-8859-1", nullptr, &written, nullptr);
    if (!utf8)
      return true;
    data.assign(utf8, written);
    g_free(utf8);
  }
  *result = MallocString(data.data(), data.size());
  return true;
}

}  // namespace

char* ClipboardReadTextLinux() {
  char* result = nullptr;
  if (FreeText(&result))
    return result;
  GtkRunSync([&] {
    GtkClipboard* clipboard = Clipboard();
    if (!clipboard)
      return;
    gchar* text = gtk_clipboard_wait_for_text(clipboard);
    if (!text)
      return;
    // Re-own through malloc so the caller frees with free() (matching the
    // other ClipboardReadText* implementations) rather than g_free.
    size_t len = strlen(text);
    if (len <= LAUFEY_CLIPBOARD_MAX_READ_BYTES)
      result = MallocString(text, len);
    g_free(text);
  });
  return result;
}

void ClipboardWriteTextLinux(const std::string& text) {
  data_control::Entries entries;
  AddTextEntries(&entries, text);
  if (FreeWrite(entries))
    return;
  GtkRunSync([&] {
    GtkClipboard* clipboard = Clipboard();
    if (!clipboard)
      return;
    gtk_clipboard_set_text(clipboard, text.c_str(),
                           static_cast<gint>(text.size()));
    // Persist the contents so they survive this process exiting, if a
    // clipboard manager is running to take ownership.
    gtk_clipboard_store(clipboard);
  });
}

uint32_t ClipboardCapabilitiesLinux() {
  uint32_t caps = LAUFEY_CLIPBOARD_CAP_TEXT | LAUFEY_CLIPBOARD_CAP_HTML |
                  LAUFEY_CLIPBOARD_CAP_IMAGE | LAUFEY_CLIPBOARD_CAP_FORMATS;
  GtkRunSync([&] {
    GdkDisplay* display = gdk_display_get_default();
    if (!display) {
      caps = 0;
      return;
    }
    // X11 reports selection changes through XFixes; GTK's Wayland backend
    // reports them from the data device (focused app only), ext-data-control
    // for every selection, and on mutter XFixes on Xwayland.
    bool wayland = strstr(G_OBJECT_TYPE_NAME(display), "Wayland") != nullptr;
    if (wayland || gdk_display_supports_selection_notification(display))
      caps |= LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS;
  });
  return caps;
}

char* ClipboardReadHtmlLinux() {
  char* result = nullptr;
  {
    std::string data;
    bool found = false;
    if (FreeRead({"text/html"}, &data, nullptr, &found)) {
      std::string html;
      if (found &&
          DecodeHtml(reinterpret_cast<const guchar*>(data.data()),
                     static_cast<gint>(data.size()), &html))
        result = MallocString(html.data(), html.size());
      return result;
    }
  }
  GtkRunSync([&] {
    GtkClipboard* clipboard = Clipboard();
    if (!clipboard)
      return;
    GtkSelectionData* data = gtk_clipboard_wait_for_contents(
        clipboard, gdk_atom_intern_static_string("text/html"));
    if (!data)
      return;
    std::string html;
    if (DecodeHtml(gtk_selection_data_get_data(data),
                   gtk_selection_data_get_length(data), &html))
      result = MallocString(html.data(), html.size());
    gtk_selection_data_free(data);
  });
  return result;
}

bool ClipboardWriteHtmlLinux(const std::string& html,
                             const char* text_or_null) {
  {
    data_control::Entries entries;
    entries.push_back({"text/html", Shared(html)});
    if (text_or_null)
      AddTextEntries(&entries, text_or_null);
    if (FreeWrite(entries))
      return true;
  }
  bool ok = false;
  GtkRunSync([&] {
    auto* offer = new Offer();
    offer->html = html;
    offer->has_text = text_or_null != nullptr;
    if (text_or_null)
      offer->text = text_or_null;
    GtkTargetList* list = gtk_target_list_new(nullptr, 0);
    gtk_target_list_add(list, gdk_atom_intern_static_string("text/html"), 0,
                        kInfoHtml);
    if (offer->has_text)
      gtk_target_list_add_text_targets(list, kInfoText);
    ok = SetOffer(list, offer);
  });
  return ok;
}

namespace {

// The image types read, in order of preference: PNG (verbatim), then the
// formats re-encoded as PNG. Nothing else is handed to an image decoder.
const std::vector<std::string>& ImageTypes() {
  static const std::vector<std::string> types = {"image/png", "image/jpeg",
                                                 "image/bmp", "image/gif"};
  return types;
}

// `mime` (one of ImageTypes()) decoded and re-encoded as PNG; GTK thread.
std::vector<uint8_t> DecodeImage(const std::string& mime, const guchar* bytes,
                                 size_t len) {
  std::vector<uint8_t> png;
  GdkPixbufLoader* loader =
      gdk_pixbuf_loader_new_with_mime_type(mime.c_str(), nullptr);
  if (!loader)
    return png;
  if (gdk_pixbuf_loader_write(loader, bytes, len, nullptr) &&
      gdk_pixbuf_loader_close(loader, nullptr)) {
    png = PixbufToPng(gdk_pixbuf_loader_get_pixbuf(loader));
  } else {
    gdk_pixbuf_loader_close(loader, nullptr);
  }
  g_object_unref(loader);
  return png;
}

}  // namespace

uint8_t* ClipboardReadImageLinux(size_t* len_out) {
  if (len_out)
    *len_out = 0;
  std::vector<uint8_t> png;
  if (FreeAvailable()) {
    // The PNG verbatim when the owner offers it, else JPEG, BMP or GIF,
    // re-encoded.
    std::string data, mime;
    bool found = false;
    FreeRead(ImageTypes(), &data, &mime, &found);
    if (found && mime == "image/png") {
      const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
      if (LooksLikePng(bytes, data.size()))
        png.assign(bytes, bytes + data.size());
    } else if (found) {
      GtkRunSync([&] {
        png = DecodeImage(mime, reinterpret_cast<const guchar*>(data.data()),
                          data.size());
      });
    }
  } else {
    GtkRunSync([&] {
      GtkClipboard* clipboard = Clipboard();
      if (!clipboard)
        return;
      // The PNG verbatim when the owner offers it.
      GtkSelectionData* data = gtk_clipboard_wait_for_contents(
          clipboard, gdk_atom_intern_static_string("image/png"));
      if (data) {
        const guchar* bytes = gtk_selection_data_get_data(data);
        gint len = gtk_selection_data_get_length(data);
        if (len > 0 &&
            static_cast<size_t>(len) <= LAUFEY_CLIPBOARD_MAX_READ_BYTES &&
            LooksLikePng(bytes, static_cast<size_t>(len)))
          png.assign(bytes, bytes + len);
        gtk_selection_data_free(data);
        if (!png.empty())
          return;
      }
      // Otherwise JPEG, BMP or GIF, re-encoded.
      for (size_t i = 1; i < ImageTypes().size() && png.empty(); i++) {
        const std::string& mime = ImageTypes()[i];
        data = gtk_clipboard_wait_for_contents(
            clipboard, gdk_atom_intern(mime.c_str(), FALSE));
        if (!data)
          continue;
        gint len = gtk_selection_data_get_length(data);
        if (len > 0 &&
            static_cast<size_t>(len) <= LAUFEY_CLIPBOARD_MAX_READ_BYTES)
          png = DecodeImage(mime, gtk_selection_data_get_data(data),
                            static_cast<size_t>(len));
        gtk_selection_data_free(data);
      }
    });
  }
  if (png.empty())
    return nullptr;
  uint8_t* out = MallocCopy(png.data(), png.size());
  if (out && len_out)
    *len_out = png.size();
  return out;
}

bool ClipboardWriteImageLinux(const uint8_t* png, size_t len) {
  if (!LooksLikePng(png, len))
    return false;
  {
    data_control::Entries entries;
    entries.push_back(
        {"image/png", Shared(std::string(reinterpret_cast<const char*>(png),
                                         len))});
    if (FreeWrite(entries))
      return true;
  }
  bool ok = false;
  GtkRunSync([&] {
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
    if (!loader)
      return;
    GdkPixbuf* pixbuf = nullptr;
    if (gdk_pixbuf_loader_write(loader, png, len, nullptr) &&
        gdk_pixbuf_loader_close(loader, nullptr)) {
      pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
      if (pixbuf)
        g_object_ref(pixbuf);
    } else {
      gdk_pixbuf_loader_close(loader, nullptr);
    }
    g_object_unref(loader);
    if (!pixbuf)
      return;
    auto* offer = new Offer();
    offer->png.assign(png, png + len);
    offer->pixbuf = pixbuf;
    GtkTargetList* list = gtk_target_list_new(nullptr, 0);
    // image/png first, verbatim; then every format gdk-pixbuf can write.
    gtk_target_list_add(list, gdk_atom_intern_static_string("image/png"), 0,
                        kInfoPng);
    gtk_target_list_add_image_targets(list, kInfoImage, TRUE);
    ok = SetOffer(list, offer);
  });
  return ok;
}

namespace {

// A target / MIME type as a clipboard format name, or "".
std::string FormatOf(const std::string& t) {
  if (t == "UTF8_STRING" || t == "STRING" || t == "TEXT" ||
      t == "COMPOUND_TEXT" || t.compare(0, 10, "text/plain") == 0)
    return "text/plain";
  if (t == "text/html")
    return "text/html";
  if (t == "image/png" || t == "image/jpeg" || t == "image/bmp" ||
      t == "image/gif")
    return "image/png";  // what read_clipboard_image can return
  if (t == "text/uri-list" || t == "x-special/gnome-copied-files")
    return "text/uri-list";
  if (t == "text/rtf" || t == "application/rtf")
    return "text/rtf";
  return "";
}

}  // namespace

char* ClipboardReadFormatsLinux() {
  std::vector<std::string> formats;
  {
    std::vector<std::string> types;
    if (FreeTypes(&types)) {
      for (const auto& t : types) {
        std::string f = FormatOf(t);
        if (!f.empty())
          formats.push_back(f);
      }
      return JoinClipboardFormats(formats);
    }
  }
  bool ok = true;
  GtkRunSync([&] {
    GtkClipboard* clipboard = Clipboard();
    if (!clipboard) {
      ok = false;
      return;
    }
    GdkAtom* targets = nullptr;
    gint n = 0;
    // FALSE for an empty clipboard (no owner) as well as on failure; both
    // read as "nothing there".
    if (!gtk_clipboard_wait_for_targets(clipboard, &targets, &n))
      return;
    for (gint i = 0; i < n; i++) {
      gchar* name = gdk_atom_name(targets[i]);
      if (!name)
        continue;
      std::string f = FormatOf(name);
      g_free(name);
      if (!f.empty())
        formats.push_back(f);
    }
    g_free(targets);
  });
  if (!ok)
    return nullptr;
  return JoinClipboardFormats(formats);
}

void ClipboardWatchLinux(bool on) {
  if (data_control::Available()) {
    data_control::Watch(on);
    return;
  }
  if (x11_clipboard::Available()) {
    x11_clipboard::Watch(on);
    return;
  }
  GtkRunAsync([on] {
    GtkClipboard* clipboard = Clipboard();
    if (!clipboard)
      return;
    if (on && !g_owner_change_id) {
      g_owner_change_id = g_signal_connect(clipboard, "owner-change",
                                           G_CALLBACK(OnOwnerChange), nullptr);
    } else if (!on && g_owner_change_id) {
      g_signal_handler_disconnect(clipboard, g_owner_change_id);
      g_owner_change_id = 0;
    }
  });
}

}  // namespace laufey_common
