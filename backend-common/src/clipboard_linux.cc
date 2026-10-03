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

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include "laufey_backend_common.h"
#include "laufey_io.h"

#include <cstdlib>
#include <cstring>
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

}  // namespace

char* ClipboardReadTextLinux() {
  char* result = nullptr;
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
    // reports them from the data device (focused app only).
    bool wayland = strstr(G_OBJECT_TYPE_NAME(display), "Wayland") != nullptr;
    if (wayland || gdk_display_supports_selection_notification(display))
      caps |= LAUFEY_CLIPBOARD_CAP_CHANGE_EVENTS;
  });
  return caps;
}

char* ClipboardReadHtmlLinux() {
  char* result = nullptr;
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

uint8_t* ClipboardReadImageLinux(size_t* len_out) {
  if (len_out)
    *len_out = 0;
  std::vector<uint8_t> png;
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
    // Otherwise any image format gdk-pixbuf can decode, re-encoded.
    GdkPixbuf* pixbuf = gtk_clipboard_wait_for_image(clipboard);
    if (pixbuf) {
      png = PixbufToPng(pixbuf);
      g_object_unref(pixbuf);
    }
  });
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

char* ClipboardReadFormatsLinux() {
  std::vector<std::string> formats;
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
      std::string t = name;
      g_free(name);
      if (t == "UTF8_STRING" || t == "STRING" || t == "TEXT" ||
          t == "COMPOUND_TEXT" || t.compare(0, 10, "text/plain") == 0) {
        formats.push_back("text/plain");
      } else if (t == "text/html") {
        formats.push_back("text/html");
      } else if (t.compare(0, 6, "image/") == 0) {
        formats.push_back("image/png");
      } else if (t == "text/uri-list" || t == "x-special/gnome-copied-files") {
        formats.push_back("text/uri-list");
      } else if (t == "text/rtf" || t == "application/rtf") {
        formats.push_back("text/rtf");
      }
    }
    g_free(targets);
  });
  if (!ok)
    return nullptr;
  return JoinClipboardFormats(formats);
}

void ClipboardWatchLinux(bool on) {
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
