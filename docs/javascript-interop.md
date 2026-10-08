# JavaScript interop

JavaScript interop lets the page and your Rust code call each other. You expose
native functions under a namespace object in the page; when the page calls one,
it receives a promise, and your Rust handler resolves or rejects it. Your code
can also evaluate a script in a window and read back its result.

```rust
use laufey::{Value, Window};

let win = Window::new(800, 600)
  .bind("greet", |call| {
    let name = call.args.first().and_then(|v| v.as_string()).unwrap_or("World");
    call.resolve(Value::String(format!("Hello, {name}!")));
  })
  .bind_async("fetchUser", |call| async move {
    let user = load_user().await;
    call.resolve(user);
  })
  .load("index.html");

// Evaluate a script in the page and read the result.
win.execute_js("document.title", Some(|result, _error| println!("{result:?}")));
```

```js
// In the page:
const message = await Laufey.greet("Ada"); // "Hello, Ada!"
```

Arguments and results cross the boundary as a `Value`, which models the JSON
types — null, boolean, integer, double, string, list, and dictionary — along
with binary blobs. When the page passes a JavaScript function as an argument, it
arrives as a callback value that you can invoke later and must release when you
are finished with it. The namespace object is named `Laufey` by default; call
`laufey::set_js_namespace` before creating any windows to change it. All
handlers run on the user-interface thread. None of this is available on the
Winit backend, which has no JavaScript engine.

Strings arrive as UTF-8. A JavaScript string can hold an unpaired surrogate,
which has no UTF-8 form; it arrives as U+FFFD, and a surrogate pair as its one
code point (on WebView2, `execute_js` results used to be narrowed code unit by
code unit, which garbled every non-ASCII character).

## Which documents can call

Only the window's top-level document gets the namespace, and only its calls
reach your handlers. Sub-frames, cross-origin iframes included, can't call: CEF
binds the namespace in the main frame only and drops a call from any other
frame; WKWebView accepts script messages from the main frame only; WebView2
takes the top document's messages; and on WebKitGTK, whose script message
handler is visible to every frame and names no frame, the top frame's bridge
script sends a per-window random token that the backend checks.

`call.call_id` is issued by the backend, not taken from the page: two windows'
pages never collide, and an answer goes back to exactly the call that asked.

A window's document can navigate anywhere, so every call carries the origin of
the document that made it (API 44): `call.origin` is `"myapp://app"`,
`"http://127.0.0.1:5173"`, or `"null"` for an opaque origin. Check it in a
binding that the page's origin should be trusted with. A packaged app can also
pin its bridge to its own origins in its launch file (`"bridgeOrigins"`, by
default its `"customSchemes"`; see [Launch configuration](launch-config.md)):
documents on other origins then get no namespace, and their calls never reach
your handlers.
