// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// gtk_message_dialog-based dialogs. Both backends (CEF, webview) link
// GTK3 on Linux, so we can use the in-process dialog rather than
// shelling out to zenity.

#include <gtk/gtk.h>

#include "laufey_backend_common.h"
#include "laufey_menu.h"
#include "laufey_window.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

// The dialogs RunGtkDialog is running, innermost last. UI thread only.
std::vector<GtkDialog*>& RunningDialogs() {
  static auto* dialogs = new std::vector<GtkDialog*>;
  return *dialogs;
}

gboolean CancelRunningDialogs(gpointer) {
  // gtk_dialog_response only asks each nested loop to end (they return once
  // this source is done), so every dialog stays alive and listed meanwhile.
  for (GtkDialog* dialog : std::vector<GtkDialog*>(RunningDialogs()))
    gtk_dialog_response(dialog, GTK_RESPONSE_CANCEL);
  return G_SOURCE_REMOVE;
}

}  // namespace

int RunGtkDialog(void* dialog) {
  GtkDialog* dlg = GTK_DIALOG(dialog);
  // Quitting already: CancelGtkDialogsForQuit has run (or is queued behind
  // this) and wouldn't see a dialog that starts now.
  if (IsQuitting())
    return GTK_RESPONSE_CANCEL;
  std::vector<GtkDialog*>& running = RunningDialogs();
  running.push_back(dlg);
  gint result = gtk_dialog_run(dlg);
  running.erase(std::find(running.begin(), running.end(), dlg));
  return result;
}

void CancelGtkDialogsForQuit() {
  // An idle source, not g_main_context_invoke: that runs the function right
  // here when no thread owns the context (before the loop starts), which
  // could be a thread other than the UI thread.
  g_idle_add_full(G_PRIORITY_HIGH, CancelRunningDialogs, nullptr, nullptr);
}

int ShowDialogLinux(int dialog_type, const std::string& title,
                    const std::string& message,
                    const std::string& default_value, char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;

  // gtk_dialog_run is a nested main loop on this (the UI) thread.
  ScopedNativeModalLoop modal_loop;
  GtkWindow* parent = nullptr;

  if (dialog_type == LAUFEY_DIALOG_ALERT) {
    GtkWidget* dlg = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
        message.c_str());
    if (!title.empty())
      gtk_window_set_title(GTK_WINDOW(dlg), title.c_str());
    RunGtkDialog(dlg);
    gtk_widget_destroy(dlg);
    return 1;
  }
  if (dialog_type == LAUFEY_DIALOG_CONFIRM) {
    GtkWidget* dlg = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "%s", message.c_str());
    if (!title.empty())
      gtk_window_set_title(GTK_WINDOW(dlg), title.c_str());
    gint result = RunGtkDialog(dlg);
    gtk_widget_destroy(dlg);
    return (result == GTK_RESPONSE_OK) ? 1 : 0;
  }
  if (dialog_type == LAUFEY_DIALOG_PROMPT) {
    GtkWidget* dlg = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "%s", message.c_str());
    if (!title.empty())
      gtk_window_set_title(GTK_WINDOW(dlg), title.c_str());
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), default_value.c_str());
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show(entry);

    gint result = RunGtkDialog(dlg);
    int rc = 0;
    if (result == GTK_RESPONSE_OK) {
      const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
      if (out_input_value && text)
        *out_input_value = strdup(text);
      rc = 1;
    }
    gtk_widget_destroy(dlg);
    return rc;
  }
  return 0;
}

}  // namespace laufey_common
