// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The page-side file-drag observer the Chromium engines (CEF, WebView2) use
// for drag and drop (API 39; see docs/drag-and-drop.md). Header-only, so the
// CEF renderer helper can inject it without linking backend-common.
//
// It listens in the capture phase on the window for trusted drag events that
// carry files, keeps the window accepting file drops (so `drop` reaches the
// page and this observer), and calls `send(phase, x, y, count, files)` for
// ENTER (0), OVER (1: at most every 50 ms, and on movement), LEAVE (2) and
// DROP (3); `files` is the DataTransfer's FileList for DROP and null
// otherwise. The script is an expression evaluating to a function of `send`;
// the host invokes it with a function no page script can reach, before any
// page script runs, and everything it uses is captured then.

#ifndef LAUFEY_FILE_DROP_OBSERVER_H_
#define LAUFEY_FILE_DROP_OBSERVER_H_

#include <string>

namespace laufey_common {

inline std::string BuildDomFileDropObserverScript() {
  // ES5-only, no page-reachable state: everything it uses is captured when it
  // runs, before any page script, and `send` is only ever in this closure.
  return R"JS((function (send) {
  'use strict';
  var w = window;
  if (w.top !== w) return;
  // Bound now, so calling them later looks nothing up: a page that replaces
  // Function.prototype.call (or Date.now) afterwards is never called here.
  var call = Function.prototype.call;
  var addTo = call.bind(EventTarget.prototype.addEventListener);
  var nowOf = call.bind(Date.now);
  var depth = 0, active = false, lastAt = 0, lastX = -1, lastY = -1;
  function files(e) {
    var dt = e.dataTransfer;
    if (!dt) return false;
    var t = dt.types;
    for (var i = 0; t && i < t.length; i++) if (t[i] === 'Files') return true;
    return false;
  }
  function count(e) {
    var items = e.dataTransfer.items, n = 0;
    for (var i = 0; items && i < items.length; i++) {
      if (items[i].kind === 'file') n++;
    }
    return n;
  }
  function enter(e) {
    active = true;
    lastX = e.clientX; lastY = e.clientY; lastAt = nowOf(Date);
    send(0, e.clientX, e.clientY, count(e), null);
  }
  function on(type, fn) { addTo(w, type, fn, true); }
  on('dragenter', function (e) {
    if (!e.isTrusted || !files(e)) return;
    depth++;
    if (!active) enter(e);
  });
  on('dragover', function (e) {
    if (!e.isTrusted || !files(e)) return;
    // Accept file drags everywhere, so the drop reaches the page and us
    // (Chromium fires `drop` only after a cancelled `dragover`). The page's
    // own handlers still decide what the drop does there.
    e.preventDefault();
    if (!active) { depth = 1; enter(e); return; }
    var t = nowOf(Date);
    if ((e.clientX !== lastX || e.clientY !== lastY) && t - lastAt >= 50) {
      lastX = e.clientX; lastY = e.clientY; lastAt = t;
      send(1, e.clientX, e.clientY, count(e), null);
    }
  });
  on('dragleave', function (e) {
    if (!e.isTrusted || !active) return;
    depth--;
    // Chromium reports (0, 0) for the leave out of the window itself.
    var out = (e.clientX <= 0 && e.clientY <= 0) || e.clientX >= w.innerWidth ||
      e.clientY >= w.innerHeight;
    if (depth <= 0 || out) {
      depth = 0; active = false;
      send(2, e.clientX, e.clientY, 0, null);
    }
  });
  on('drop', function (e) {
    if (!e.isTrusted || !files(e)) return;
    depth = 0; active = false;
    send(3, e.clientX, e.clientY, count(e), e.dataTransfer.files);
  });
}))JS";
}

// The WebView2 backend's whole drop observer: BuildDomFileDropObserverScript
// with a `send` that posts `{"__laufeyFileDrop":"<token>",...}` (and the
// dropped files) to the host through chrome.webview. The per-process `token`
// ([0-9a-f], no quoting needed) is what tells the host a message came from
// here, so it must never reach page script: everything `send` calls is
// captured and bound when the script runs, at document creation, before any
// page script. A page that later replaces Function.prototype.call / apply /
// bind, chrome.webview.postMessage or String.prototype methods is never
// called with the message.
inline std::string BuildWebView2FileDropScript(const std::string& token) {
  return "(function () {\n"
         "  var wv = window.chrome && window.chrome.webview;\n"
         "  if (!wv || window.top !== window) return;\n"
         "  var call = Function.prototype.call;\n"
         "  var post = call.bind(wv.postMessage);\n"
         "  var postX = wv.postMessageWithAdditionalObjects ?\n"
         "      call.bind(wv.postMessageWithAdditionalObjects) : null;\n"
         "  var token = '" +
         token +
         "';\n"
         "  (" +
         BuildDomFileDropObserverScript() +
         ")(function (p, x, y, n, files) {\n"
         "    var m = '{\"__laufeyFileDrop\":\"' + token + "
         "'\",\"p\":' + (p | 0) + ',\"x\":' + (+x || 0) + "
         "',\"y\":' + (+y || 0) + ',\"n\":' + (n | 0) + '}';\n"
         "    if (files && postX) postX(wv, m, files);\n"
         "    else post(wv, m);\n"
         "  });\n"
         "})();\n";
}

}  // namespace laufey_common

#endif  // LAUFEY_FILE_DROP_OBSERVER_H_
