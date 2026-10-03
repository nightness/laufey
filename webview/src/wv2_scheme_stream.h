// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

// Custom-scheme responses on WebView2, buffered or streamed.
//
// WebView2 reads a WebResourceRequested response's IStream to its end before
// it hands any of it to the page (the documented contract: "Stream must have
// all the content data available by the time the WebResourceRequested event
// deferral of this response is completed"; MicrosoftEdge/WebView2Feedback
// #3519). A response that never ends, such as Server-Sent Events, therefore
// never reaches the page, and blocking the stream's Read stalls the one
// thread WebView2 reads every response stream on.
//
// So bodies are buffered, as before, unless the request came from the page
// shim (wv2_scheme_stream_shim.js: fetch, EventSource and asynchronous XHR to
// a same-origin URL on a registered scheme), which tags it with
// `x-laufey-stream: <id>`. A tagged response that is not finished
// kPushAfterMs after its head is answered with the head alone (marked
// `x-laufey-stream: push`); its body then goes to the document over
// PostWebMessageAsJson as the runtime writes it, under a credit window the
// shim refills as the page reads. See docs/custom-schemes.md.

#ifndef LAUFEY_WV2_SCHEME_STREAM_H_
#define LAUFEY_WV2_SCHEME_STREAM_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "WebView2.h"

namespace laufey_wv2 {

struct StreamHooks {
  // Runs `task` on the UI thread (inline when already on it).
  std::function<void(std::function<void()>)> run_on_ui;
  // UI thread: PostWebMessageAsJson(json) to window `window_id`'s top-level
  // document. Returns false if the window or its web view is gone.
  std::function<bool(uint32_t window_id, const std::wstring& json)> post_json;
};

// Once, on the UI thread, before the first window.
void InitSchemeStreams(StreamHooks hooks);

// The document-start script that installs the shim for `schemes`.
std::string BuildSchemeStreamShimScript(
    const std::vector<std::string>& schemes);

// The WebResourceRequested handler for registered schemes (UI thread).
HRESULT HandleSchemeRequest(ICoreWebView2Environment* env,
                            ICoreWebView2WebResourceRequestedEventArgs* args,
                            uint32_t window_id);

// A web message from window `window_id`'s top-level document whose URI is
// `source`. Returns true if it was a stream control message (consumed).
bool HandleStreamMessage(uint32_t window_id, const wchar_t* message,
                         const wchar_t* source);

// Main-frame navigation `navigation_id` started / committed: streams of the
// document it replaces are cancelled at commit.
void OnNavigationStarting(uint32_t window_id, uint64_t navigation_id);
void OnNavigationCommitted(uint32_t window_id, uint64_t navigation_id);

// The window (or its renderer) is gone: cancel all of its streams.
void CancelStreamsForWindow(uint32_t window_id);

}  // namespace laufey_wv2

#endif  // LAUFEY_WV2_SCHEME_STREAM_H_
