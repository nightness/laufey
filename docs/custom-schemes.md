# Custom URL schemes

A runtime can serve its web content in-process over a URL scheme of its own
instead of a loopback HTTP server. Every web-engine backend serves the built-in
`app` scheme, and a runtime may register more, such as `myapp`. Each registered
scheme is a real origin in the page: `location.origin` is `<scheme>://<host>`,
the page is a secure context (`crypto.subtle` and other secure-only APIs work),
`localStorage` and IndexedDB are scoped to that origin and, on the WebView
backends, persist across launches, and cross-origin requests carry
`Origin: <scheme>://<host>`. That gives an application a stable identity that
does not change with a port number, which storage, CORS allow-lists, and sign-in
providers that check the request origin (OAuth redirects, passkeys) depend on.

```rust
use laufey::Window;

fn main() {
  // Register before the first window: the engines read their scheme tables
  // when a web view is created.
  laufey::register_scheme_handler("myapp", |req| {
    let (status, body): (i32, &[u8]) = match req.url.as_str() {
      "myapp://app/" => (200, b"<!doctype html><h1>Hello</h1>"),
      _ => (404, b"not found"),
    };
    let headers = [("content-type".to_string(), "text/html".to_string())];
    req.exchange.begin(status, &headers);
    req.exchange.write(body);
    req.exchange.finish();
  });

  // location.origin in this window is "myapp://app".
  Window::new(800, 600).title("My App").load("myapp://app/");
}

laufey::main!(main);
```

One handler serves every registered scheme and every window: a later
`register_scheme_handler` call replaces the handler for all of them and adds the
new scheme, so dispatch on `req.url` (and `req.window_id` if windows serve
different content). The handler runs on a backend thread and must not block it;
move slow work onto your own thread or async runtime. Both the request body
(`req.exchange.read_body`) and the response are streamed, so the page's `fetch`
sees each `write` as it happens (see
[Streaming responses](#streaming-responses), which WebView2 supports only for
`fetch`, `EventSource` and XHR). Scheme names follow RFC 3986 (a letter, then
letters, digits, `+`, `-`, or `.`), are case-insensitive, and are stored in
lowercase; an invalid name is logged and ignored. Engine-less backends such as
Winit have no scheme support; `laufey::scheme_handlers_supported()` returns
`false` there, and the application should fall back to a loopback server.

Register every scheme before creating the first window. The system web views
read their scheme tables when a web view is created, so on most backends a
scheme registered afterwards is not served by a window that already exists, and
the backend logs a warning. Each backend installs the schemes in its own way:

- **WebView (macOS)** installs one `WKURLSchemeHandler` per scheme on the
  `WKWebViewConfiguration` of each new window. Schemes that WebKit handles
  itself (`http`, `https`, `file`, and so on) are skipped with a warning.
- **WebView (Linux)** registers each scheme on WebKitGTK's default web context
  and marks it secure and CORS-enabled in the context's security manager. The
  web context is shared, so WebKitGTK also applies a late registration to
  windows that already exist; the backend still warns, because the other
  backends do not.
- **WebView (Windows)** adds a `CoreWebView2CustomSchemeRegistration`
  (`TreatAsSecure`, `HasAuthorityComponent`) per scheme to the environment
  options. Every environment in a process shares one user data folder, and
  WebView2 requires identical scheme registrations across them, so the backend
  freezes the set when the first window is created; later registrations are
  logged and ignored for the rest of the process.
- **CEF** learns custom schemes in `CefApp::OnRegisterCustomSchemes`, which runs
  in every process during startup, before the runtime library is loaded. The
  embedder therefore also declares the schemes when it launches the host, with
  `--laufey-custom-schemes=myapp,other` or `LAUFEY_CUSTOM_SCHEMES=myapp,other`
  (comma-separated; `app` is always declared), or ships them as
  `"customSchemes"` in the app's [launch file](launch-config.md), which also
  works when nothing sets the environment (the app is started directly). The
  browser process forwards the list to its child processes. A declared scheme is
  served in every window from the moment it is registered. A scheme that is
  registered but was not declared is still served, but Chromium treats it as a
  non-standard scheme with an opaque origin and no secure context, and the
  backend logs a warning. Storage on a custom-scheme origin lives in the CEF
  profile: with an app data directory (`LAUFEY_APP_ID` / `LAUFEY_DATA_DIR`, or
  `"appId"` / `"dataDir"` in the [launch file](launch-config.md)) that profile
  is `<dir>/CEF` and persists across launches (see
  [App data & web storage](app-data.md)); without one the host uses a fresh
  temporary profile per process, so storage does not outlive it. See
  [Local Network Access](#local-network-access-cef) for how a declared scheme's
  pages reach loopback servers.

```sh
laufey --laufey-custom-schemes=myapp --runtime ./libmyapp.so
```

## Local Network Access (CEF)

Chromium's Local Network Access checks (formerly Private Network Access) hold a
page's request to a loopback or private-network address (`127.0.0.1`, `::1`,
`192.168.x.x`, ...) until the user grants the page a permission, unless the page
itself was loaded from such an address. A custom-scheme page counts as a public
origin, so without help an app page on `myapp://app` could not open its
runtime's loopback WebSocket or call a local dev server, and the CEF host shows
no prompt that would let anyone grant it: the request would simply hang.

The CEF backend answers those prompts itself (`CefPermissionHandler`):

- An origin on one of the embedder's declared schemes (`app` and every scheme in
  `--laufey-custom-schemes` / `LAUFEY_CUSTOM_SCHEMES` / the launch file's
  `"customSchemes"`) is granted local network access, for `fetch`, XHR and
  WebSockets alike.
- Every other origin is denied: `http(s)` pages (remote content, or a site the
  app navigated to), `file:`, `data:`, opaque origins, and schemes that were
  registered without being declared. Their requests fail at once with a network
  error rather than waiting for a prompt, so remote content keeps Chromium's
  protection against reaching into the user's network.
- A prompt that asks for local network access together with any other permission
  is denied as a whole, even for a declared origin, so the grant never extends
  to another permission. Prompts that don't involve local network access get the
  engine's default handling.

The security model rests on who can put content on an origin. A page on a
declared custom scheme can only come from the embedder's own scheme handler,
in-process: no network server can produce that origin. The grant therefore
extends to whatever the handler serves, so a handler that proxies remote content
onto its scheme hands that content local network access too, and should not.
Navigating a window to a remote site doesn't widen anything: that site's origin
is not declared, so it is denied. LNA is never switched off for the process (no
`--disable-features=LocalNetworkAccessChecks`).

The WebView backends have no such checks: WKWebView, WebView2 and WebKitGTK let
a page reach loopback servers already.

## Streaming responses

A response reaches the page as the handler writes it, and a response may never
end: a page can read a Server-Sent Events stream with `EventSource`, or a
`fetch` body with `response.body.getReader()`, while the handler keeps writing.
When the page stops reading (the reader is cancelled, the request aborted, the
`EventSource` closed, the document replaced or the window closed), the next
`write` returns a negative value; stop writing then and call `finish`. A handler
that has nothing to send for a long time learns of the cancellation only at its
next write, so a heartbeat (an SSE comment line such as `:\n\n`) bounds how long
a dead stream lingers.

The backend also tells the handler directly: the `on_cancel` callback passed to
`register_scheme_handler` (in the `laufey` crate,
`SchemeExchange::is_cancelled()` turns true) fires at most once per exchange,
never after `finish` has returned, when the engine gives up on the request: CEF
cancels the request, WKWebView stops the scheme task, WebKitGTK lets the body
stream go before it ended, WebView2 cancels a streamed response (the reader
cancelled, the document gone). WebKitGTK says nothing about a request cancelled
before its head was sent, and WebView2 nothing about a response it takes in one
piece; there, as everywhere, the next `write` failing is the signal.

A handler's `write` **never blocks**: the runtime writes from its event loop
thread, so a write that waited for a slow page would stall the whole app. Each
backend takes the bytes and hands them on as the engine reads:

- **WKWebView** passes each write to WebKit (`didReceiveData:`) on the main
  thread; WebKit takes all of it.
- **WebKitGTK** queues each write in memory for the response's `GInputStream`,
  which WebKit reads from the GTK main loop (a pollable stream: no thread waits
  on it). Before API 42 the body went through a pipe whose `write` blocked once
  64 KiB were unread, which stalled the runtime whenever the GTK thread was
  busy.
- **CEF** queues it for Chromium's next `Read` (Chromium stops reading while the
  page doesn't consume).
- **WebView2** streams through the page (below).

The queue for a page that isn't reading is capped at **64 MiB** on WebKitGTK,
CEF and WebView2 (for a response WebView2 streams through the page, below): past
that the response fails (the page's `fetch` or read rejects) and the handler's
next `write` returns a negative value, instead of the body growing without
bound. WKWebView hands everything to WebKit, so it has no cap of its own. A
response WebView2 answers in one piece is held whole until the handler finishes
it, whether or not the page reads, so it is capped separately at **512 MiB**:
past that the request fails.

A handler that calls `finish` without ever calling `begin` gives the page no
response: its request fails, as a network error does (the `fetch` rejects), on
every backend.

WKWebView, WebKitGTK and CEF hand each write to the page as it arrives. WebView2
cannot: it reads a `WebResourceRequested` response stream to its end before the
page sees any of it
([WebView2Feedback#3519](https://github.com/MicrosoftEdge/WebView2Feedback/issues/3519)),
so a stream that never ends would never arrive. The WebView2 backend therefore
streams through the page instead:

- A script installed at document start wraps `fetch`, `EventSource` and
  `XMLHttpRequest` (asynchronous requests). Requests they make to a URL on a
  registered scheme **with the document's own origin** carry an
  `x-laufey-stream` header with a random id (the backend strips it before the
  handler sees the request).
- A tagged response that is still open 50 ms after its head is answered with the
  head alone, and its body is posted to the document as it is written
  (`PostWebMessageAsJson`, base64). The wrapper builds the `Response` from those
  chunks, so the page reads it incrementally; status, status text, headers,
  binary bodies and `Content-Encoding` (decoded with `DecompressionStream`)
  behave as before. A tagged response that finishes within 50 ms, and every
  other request, is answered in one piece as before: the body is held until the
  handler finishes (up to 512 MiB) and handed to WebView2 without another copy.
- The page acknowledges what it reads: at most 4 MiB is in flight to a page that
  is not reading. Beyond that the backend holds up to 64 MiB, and a response
  that outgrows it fails (the page's read rejects and the handler's write
  returns a negative value); the handler's `write` itself never blocks.
- Only a same-origin document may receive a body: the backend checks the origin
  of the document that asks for it against the request URL.

The limits: navigations, subresources (`<img>`, `<script>`, `<video>`, ...),
synchronous XHR, requests to another origin, and requests made from a document
that is cross-origin with the top-level document (whose web messages WebView2
does not deliver) are still answered in one piece, so on WebView2 they cannot
stream; a response to them that never ends is cancelled when the document goes
away. A streamed `Response`'s `clone()`, `url`, `redirected` and `type` behave
as usual, but upload progress events are not fired for a wrapped
`XMLHttpRequest`, and the page's own `chrome.webview` message listeners do not
see the stream messages.

Treat everything in a request as untrusted input, as you would for an HTTP
server. A page on another origin, including a remote page the window navigated
to, can send requests to your scheme; engines allow it to read the response only
when the response carries CORS headers, so do not answer with
`Access-Control-Allow-Origin: *` on responses that carry private data, and check
the `origin` request header before acting on state-changing requests. Because
one handler serves every scheme, check the scheme and host of `req.url` rather
than assuming which one a request came in on.

The C-level contract is described in
[C ABI → Custom URL scheme handler](c-abi.md#custom-url-scheme-handler-api--26),
and the end-to-end battery that checks it on each backend in
[End-to-end testing](e2e-testing.md#c-custom-scheme--ipc).
