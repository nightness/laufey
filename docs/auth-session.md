# Auth session

A desktop app that signs its user in with OAuth / OpenID Connect should not show
the identity provider's page in its own web view: the user can't see the address
bar, the app could read the password, and many providers refuse embedded web
views. [RFC 8252](https://www.rfc-editor.org/rfc/rfc8252) ("OAuth 2.0 for Native
Apps") says to run the sign-in in the user's browser and receive the redirect
back in the app. laufey's auth session does that through the OS where the OS has
an API for it:

- **macOS** (10.15+, WKWebView and CEF): `ASWebAuthenticationSession`. The
  sign-in page opens in a sheet on the app's window, backed by Safari (or the
  user's default browser when it supports it), and the session ends when the
  browser reaches the callback URL. The user can close the sheet, which ends the
  session with a real `cancelled`.
- **Windows, Linux**: there is no OS equivalent. RFC 8252 says to open the
  system browser and receive the redirect through a loopback listener
  (`http://127.0.0.1:<port>/…`, §7.3) or a claimed URL scheme (§7.1), which the
  embedder does itself (see [deep-links.md](deep-links.md) for receiving a
  custom-scheme URL). The capabilities are 0 and every request answers
  `not_supported`, so a caller can fall back without guessing the OS. The
  browser gives no signal when the user closes its tab; the embedder needs a
  timeout and an in-app cancel button there.
- **Winit**: `NULL` (no web content).

```rust
let caps = laufey::auth_session_capabilities();
if caps.supported {
  // The app's own window (0: the key window), PKCE + state are yours.
  match laufey::auth_session_start(window.id(), &authorize_url, "myapp", true).await {
    Ok(callback_url) => { /* "myapp://callback?code=...&state=..." */ }
    Err(e) if e.kind == laufey::AuthSessionErrorKind::Cancelled => { /* the user closed it */ }
    Err(e) => { /* e.message */ }
  }
} else {
  // RFC 8252: open the system browser, listen on a loopback port.
}
```

## API

C ABI (API ≥ 42, `capi/include/laufey.h`):

```c
uint32_t (*auth_session_capabilities)(void* backend_data);
void (*auth_session_start)(void* backend_data, uint32_t window_id,
                           const char* url, const char* callback,
                           uint32_t flags,
                           laufey_auth_session_result_fn on_result,
                           void* user_data);
bool (*test_cancel_auth_session)(void* backend_data);
bool (*auth_session_cancel)(void* backend_data);  // API >= 43

typedef void (*laufey_auth_session_result_fn)(void* user_data, int32_t status,
                                              const char* value);
```

- `auth_session_capabilities` returns `LAUFEY_AUTH_SESSION_CAP_SUPPORTED`,
  `_EPHEMERAL` and (macOS 14.4+) `_HTTPS_CALLBACK`. Any thread.
- `auth_session_start` opens the sign-in at `url` (http or https, at most
  `LAUFEY_AUTH_SESSION_MAX_URL_BYTES`, printable ASCII: percent-encode the
  rest). `callback` is where it ends:
  - a **custom scheme** (`"myapp"`): the first navigation to `myapp:…` completes
    the session with that URL. Not `http`, `https`, `file`, `about`, `data`,
    `javascript`, `blob`, `ws` or `wss`;
  - an **https URL** (`"https://example.com/auth/done"`, macOS 14.4+): a
    navigation to that host and path completes it. The app needs the host as an
    associated domain (`webcredentials:`), as for [passkeys](passkeys.md#macos).

  `window_id` anchors the sheet: `0` means the key window, else the app's first
  visible window. A menu-bar app with no window still gets the sheet (on a
  window of the OS's own). `flags`: `LAUFEY_AUTH_SESSION_EPHEMERAL` asks for a
  private browser session, sharing no cookies with the browser, and skips the
  "“App” Wants to Use “example.com” to Sign In" prompt macOS shows otherwise.
- The result arrives through `on_result` **exactly once**, on **any** thread:
  - `LAUFEY_AUTH_SESSION_OK`: `value` is the full callback URL. The code,
    `state` and any error parameters are in it; validating `state` and redeeming
    the code with PKCE (RFC 7636) are the caller's job.
  - `CANCELLED`: the user closed the sheet or declined the prompt, the anchor
    window closed, the app's event loop ended, the app called
    `auth_session_cancel`, or `test_cancel_auth_session`.
  - `NOT_SUPPORTED`: Windows, Linux, an https callback before macOS 14.4.
  - `INVALID`: a bad url, callback, flags or window id.
  - `BUSY`: another session is in progress. One runs at a time per app.
  - `FAILED`: the OS refused or failed (`value` says why).

  Refusals (`NOT_SUPPORTED`, `INVALID`, `BUSY`) are answered synchronously,
  before `auth_session_start` returns. Embedders must not block in the callback.
- `auth_session_cancel` (API 43) is how the app gives up on the running session:
  the page cancelled the sign-in, or the app's own timeout fired. The sheet
  closes, the session's `on_result` gets `CANCELLED` (still exactly once), and
  the next `auth_session_start` is not `BUSY`. It returns `false`, and does
  nothing, when no session is running, which is always the case where sessions
  are not supported (Windows, Linux, Winit). Any thread.
- `test_cancel_auth_session` (test-only) ends the running session as the user
  closing its sheet would.

Rust (`laufey` crate): `auth_session_capabilities()`,
`auth_session_start(window_id, url, callback, ephemeral) -> impl Future<Output = Result<String, AuthSessionError>>`
(`AuthSessionError { kind: AuthSessionErrorKind, message }`, the kind's `code()`
is `cancelled` / `not_supported` / `invalid` / `busy` / `failed`), and
`auth_session_cancel() -> bool` (API 43) and `test_cancel_auth_session()`. The
session starts when the function is called; dropping the future does not end it:
call `auth_session_cancel()`.

## Behavior notes

- **Cancellation is always answered.** macOS reports `canceledLogin` when the
  user closes the sheet, but a session cancelled while the consent prompt of a
  non-ephemeral session is still up never hears back from the OS. laufey answers
  `cancelled` itself when it cancels (window closed, test hook, quit) and drops
  whatever the OS reports later.
- **The anchor window closing** cancels the session anchored to it.
- **Quitting** (the event loop ending) cancels the running session before the
  runtime is shut down.
- **Nothing is logged**: sign-in and callback URLs carry codes and state, and
  error messages never echo them.
- **Default browser**: macOS uses Safari, or the default browser when it
  implements the web-authentication extension point; an ephemeral session always
  starts from an empty cookie jar.

## Testing

`backend-common/tests/auth_main_thread_test.cc` covers argument validation, the
one-session slot, exactly-once results and cancellation without the OS. The
native e2e (`scripts/native-e2e-run.sh <backend> --auth-thread`) checks the
capabilities on every OS, `not_supported` off macOS, and on macOS a real
`ASWebAuthenticationSession` round trip through a loopback server to a
custom-scheme callback (ephemeral, so no prompt), `busy`, the test hook, the
anchor window closing and, in CI, a cancel during the consent prompt. See
[e2e-testing.md](e2e-testing.md).
