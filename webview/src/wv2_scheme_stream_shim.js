// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// WebView2 streaming shim for registered custom schemes (see
// docs/custom-schemes.md, "Streaming responses on WebView2").
//
// WebView2 reads a WebResourceRequested response stream to its end before it
// hands any byte to the page (MicrosoftEdge/WebView2Feedback#3519), so a
// response that never ends (Server-Sent Events) never arrives and a slow one
// arrives all at once. For fetch(), EventSource and asynchronous
// XMLHttpRequest to a same-origin URL on a registered scheme this shim tags
// the request with `x-laufey-stream: <random id>`. When the response is not
// complete shortly after its head (see wv2_scheme_stream.cc), the host
// answers with the head only, marked `x-laufey-stream: push`, and posts the
// body to this document over chrome.webview messages; the shim turns them
// back into the Response body. Everything else (navigations, subresources,
// other origins, synchronous XHR) is untouched.
//
// Built into a document-start script by BuildSchemeStreamShimScript, which
// prepends `const __laufeySchemes = [...]`. Runs in every frame; a frame that
// is not the top-level one routes through the top-level frame's registry when
// it is same-origin with it, and otherwise leaves its requests alone.
(function () {
  "use strict";
  const G = window;
  const SCHEMES = __laufeySchemes;
  const KEY = "__laufeyStream";
  const REGISTRY = Symbol.for("laufey.schemeStream");
  const TAG = "x-laufey-stream";

  // Captured before any page script runs, so a page that replaces fetch,
  // Request, ... later neither breaks nor bypasses the shim.
  const origFetch = G.fetch;
  const NRequest = G.Request;
  const NResponse = G.Response;
  const NHeaders = G.Headers;
  const NReadableStream = G.ReadableStream;
  const NXHR = G.XMLHttpRequest;
  const NEventSource = G.EventSource;
  const NAbortController = G.AbortController;
  const NTextDecoder = G.TextDecoder;
  const NDecompressionStream = G.DecompressionStream;
  const NURL = G.URL;
  const NMessageEvent = G.MessageEvent;
  const NProgressEvent = G.ProgressEvent;
  const NEvent = G.Event;
  const NDOMException = G.DOMException;
  const NBlob = G.Blob;
  const NDOMParser = G.DOMParser;
  const getRandomValues = G.crypto.getRandomValues.bind(G.crypto);
  const apply = Reflect.apply;
  const setTimeout_ = G.setTimeout.bind(G);
  const clearTimeout_ = G.clearTimeout.bind(G);
  if (!origFetch || !NRequest || !NReadableStream) return;

  function randomId() {
    const b = new Uint8Array(16);
    getRandomValues(b);
    let s = "";
    for (let i = 0; i < b.length; i++) {
      s += (b[i] < 16 ? "0" : "") + b[i].toString(16);
    }
    return s;
  }

  // Origin of a URL on a registered scheme. URL.origin is "null" for
  // non-special schemes, so build "<scheme>://<host>" the way the engine
  // reports location.origin for a registered (standard) scheme.
  function schemeOrigin(href) {
    let u;
    try {
      u = new NURL(href, G.location.href);
    } catch (_) {
      return null;
    }
    const scheme = u.protocol.slice(0, -1).toLowerCase();
    if (SCHEMES.indexOf(scheme) < 0) return null;
    return scheme + "://" + u.host;
  }
  function eligible(href) {
    const o = schemeOrigin(href);
    return o !== null && o === G.location.origin;
  }

  function decodeBase64(s) {
    if (typeof Uint8Array.fromBase64 === "function") {
      return Uint8Array.fromBase64(s);
    }
    const bin = G.atob(s);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  // --- The registry: one per top-level document, the only one that talks to
  // the host. Sub-frames that are same-origin with it share it.
  function installRegistry() {
    const wv = G.chrome && G.chrome.webview;
    if (!wv || typeof wv.postMessage !== "function") return null;
    const postMessage = wv.postMessage;
    const sinks = new Map();
    const send = (t, id, n) =>
      apply(postMessage, wv, [
        '{"' + KEY + '":{"t":"' + t + '","id":"' + id + '","n":' + (n | 0) +
        "}}",
      ]);
    apply(wv.addEventListener, wv, [
      "message",
      (e) => {
        const d = e.data;
        const m = d && typeof d === "object" ? d[KEY] : undefined;
        if (!m || typeof m !== "object") return;
        // Ours: keep it from the page's own message listeners.
        e.stopImmediatePropagation();
        const sink = sinks.get(m.id);
        if (!sink) return; // not a request this document made
        if (m.t === "d" && typeof m.b === "string") {
          sink.data(decodeBase64(m.b));
        } else if (m.t === "e") {
          sinks.delete(m.id);
          sink.end();
        } else if (m.t === "x") {
          sinks.delete(m.id);
          sink.error(String(m.m || "stream failed"));
        }
      },
    ]);
    const registry = Object.freeze({
      open(id, sink) {
        sinks.set(id, sink);
      },
      // `cancel` also tells the host to stop the response.
      close(id, cancel) {
        const had = sinks.delete(id);
        if (cancel && had) send("cancel", id, 0);
      },
      attach(id) {
        send("attach", id, 0);
      },
      ack(id, n) {
        send("ack", id, n);
      },
    });
    Object.defineProperty(G, REGISTRY, { value: registry });
    return registry;
  }
  const ownRegistry = G.top === G ? installRegistry() : null;
  function registry() {
    if (ownRegistry) return ownRegistry;
    try {
      // Throws for a cross-origin top-level document.
      return G.top[REGISTRY] || null;
    } catch (_) {
      return null;
    }
  }

  // Requests this document has open, cancelled when it goes away (the host
  // also cancels them when the top-level document is replaced).
  const live = new Set();
  G.addEventListener("pagehide", () => {
    for (const cancel of Array.from(live)) cancel();
  });

  const NULL_BODY = [101, 103, 204, 205, 304];

  // fetch() of an eligible Request through `reg`. Resolves like fetch().
  async function streamFetch(req, reg) {
    const id = randomId();
    const headers = new NHeaders(req.headers);
    headers.set(TAG, id);
    const tagged = new NRequest(req, { headers, duplex: "half" });

    const st = { q: [], done: false, err: null, wake: null, cancelled: false };
    const wakeup = () => {
      const w = st.wake;
      st.wake = null;
      if (w) w();
    };
    const cancel = () => {
      if (st.cancelled) return;
      st.cancelled = true;
      live.delete(cancel);
      st.q.length = 0;
      reg.close(id, true);
      wakeup();
    };
    reg.open(id, {
      data(u8) {
        if (st.cancelled) return;
        // Chunks from the top-level frame's realm are copied into ours.
        st.q.push(u8 instanceof Uint8Array ? u8 : new Uint8Array(u8));
        wakeup();
      },
      end() {
        st.done = true;
        wakeup();
      },
      error(msg) {
        st.err = new TypeError(msg);
        wakeup();
      },
    });
    live.add(cancel);
    const signal = req.signal;
    const onAbort = () => {
      st.err = signal.reason !== undefined
        ? signal.reason
        : new NDOMException("The operation was aborted.", "AbortError");
      cancel();
    };
    if (signal) {
      if (signal.aborted) onAbort();
      else signal.addEventListener("abort", onAbort, { once: true });
    }

    let resp;
    try {
      resp = await apply(origFetch, G, [tagged]);
    } catch (e) {
      cancel();
      throw e;
    }
    if (resp.headers.get(TAG) !== "push") {
      // Complete before the host's deadline: an ordinary response.
      live.delete(cancel);
      reg.close(id, false);
      if (signal) signal.removeEventListener("abort", onAbort);
      return resp;
    }

    const outHeaders = new NHeaders(resp.headers);
    outHeaders.delete(TAG);
    let body = null;
    if (NULL_BODY.indexOf(resp.status) < 0) {
      body = new NReadableStream(
        {
          pull(c) {
            return new Promise((resolve) => {
              const step = () => {
                if (st.q.length) {
                  const chunk = st.q.shift();
                  c.enqueue(chunk);
                  reg.ack(id, chunk.byteLength);
                  resolve();
                } else if (st.err) {
                  c.error(st.err);
                  resolve();
                } else if (st.done) {
                  live.delete(cancel);
                  c.close();
                  resolve();
                } else if (st.cancelled) {
                  resolve();
                } else {
                  st.wake = step;
                }
              };
              step();
            });
          },
          cancel() {
            cancel();
          },
        },
        { highWaterMark: 0 },
      );
      // The host passes the body through as the runtime wrote it; decode a
      // content-coding the way the network stack would have.
      const enc = (resp.headers.get("content-encoding") || "").trim()
        .toLowerCase();
      if (enc && enc !== "identity") {
        const fmt = enc === "x-gzip" ? "gzip" : enc;
        try {
          body = body.pipeThrough(new NDecompressionStream(fmt));
        } catch (_) {
          cancel();
          throw new TypeError("unsupported content-encoding: " + enc);
        }
      }
    } else {
      cancel();
    }
    const out = new NResponse(body, {
      status: resp.status,
      statusText: resp.statusText,
      headers: outHeaders,
    });
    const props = {
      url: { value: resp.url },
      redirected: { value: resp.redirected },
      type: { value: resp.type },
    };
    Object.defineProperties(out, props);
    const nativeClone = out.clone;
    Object.defineProperty(out, "clone", {
      value: function clone() {
        const c = apply(nativeClone, this, []);
        Object.defineProperties(c, props);
        return c;
      },
    });
    if (body) reg.attach(id);
    return out;
  }

  // Returns a Promise<Response>, or null when the shim does not apply.
  function maybeStream(req) {
    if (req.mode === "navigate" || !eligible(req.url)) return null;
    const reg = registry();
    return reg ? streamFetch(req, reg) : null;
  }

  G.fetch = function fetch(input, init) {
    let req;
    try {
      req = new NRequest(input, init);
    } catch (_) {
      return apply(origFetch, G, [input, init]); // let fetch report it
    }
    return maybeStream(req) || apply(origFetch, G, [req]);
  };

  // --- EventSource over the shimmed fetch (WHATWG HTML 9.2).
  if (NEventSource) {
    const CONNECTING = 0, OPEN = 1, CLOSED = 2;
    class LaufeyEventSource extends EventTarget {
      static get CONNECTING() {
        return CONNECTING;
      }
      static get OPEN() {
        return OPEN;
      }
      static get CLOSED() {
        return CLOSED;
      }
      static [Symbol.hasInstance](v) {
        return Function.prototype[Symbol.hasInstance].call(
          LaufeyEventSource,
          v,
        ) ||
          v instanceof NEventSource;
      }
      #url;
      #creds;
      #state = CONNECTING;
      #lastId = "";
      #retry = 3000;
      #ctrl = null;
      #timer = 0;
      constructor(url, init) {
        let abs;
        try {
          abs = new NURL(url, G.location.href).href;
        } catch (_) {
          return new NEventSource(url, init);
        }
        if (!eligible(abs) || !registry()) return new NEventSource(url, init);
        super();
        this.#url = abs;
        this.#creds = !!(init && init.withCredentials);
        this.onopen = null;
        this.onmessage = null;
        this.onerror = null;
        this.#connect();
      }
      get url() {
        return this.#url;
      }
      get withCredentials() {
        return this.#creds;
      }
      get readyState() {
        return this.#state;
      }
      close() {
        this.#state = CLOSED;
        clearTimeout_(this.#timer);
        if (this.#ctrl) this.#ctrl.abort();
        this.#ctrl = null;
      }
      #fire(ev) {
        const h = this["on" + ev.type];
        if (typeof h === "function") {
          try {
            h.call(this, ev);
          } catch (e) {
            setTimeout_(() => {
              throw e;
            });
          }
        }
        this.dispatchEvent(ev);
      }
      #fail(reconnect) {
        if (this.#state === CLOSED) return;
        if (this.#ctrl) this.#ctrl.abort();
        this.#ctrl = null;
        this.#state = reconnect ? CONNECTING : CLOSED;
        this.#fire(new NEvent("error"));
        if (reconnect && this.#state === CONNECTING) {
          this.#timer = setTimeout_(() => this.#connect(), this.#retry);
        }
      }
      async #connect() {
        if (this.#state === CLOSED) return;
        const ctrl = new NAbortController();
        this.#ctrl = ctrl;
        const headers = new NHeaders({ accept: "text/event-stream" });
        if (this.#lastId) headers.set("last-event-id", this.#lastId);
        let resp;
        try {
          const req = new NRequest(this.#url, {
            headers,
            cache: "no-store",
            credentials: this.#creds ? "include" : "same-origin",
            signal: ctrl.signal,
          });
          resp = await (maybeStream(req) || apply(origFetch, G, [req]));
        } catch (_) {
          if (this.#ctrl === ctrl) this.#fail(true);
          return;
        }
        if (this.#ctrl !== ctrl) return;
        const type = (resp.headers.get("content-type") || "").split(";")[0]
          .trim()
          .toLowerCase();
        if (resp.status !== 200 || type !== "text/event-stream" || !resp.body) {
          this.#fail(false);
          return;
        }
        this.#state = OPEN;
        this.#fire(new NEvent("open"));
        const reader = resp.body.getReader();
        const dec = new NTextDecoder();
        let buf = "", data = "", event = "", id = null, first = true;
        const line = (l) => {
          if (l === "") {
            if (id !== null) this.#lastId = id;
            id = null;
            if (data === "") {
              event = "";
              return;
            }
            if (data.endsWith("\n")) data = data.slice(0, -1);
            const ev = new NMessageEvent(event || "message", {
              data,
              origin: G.location.origin,
              lastEventId: this.#lastId,
            });
            data = "";
            event = "";
            if (this.#state !== CLOSED) this.#fire(ev);
            return;
          }
          if (l[0] === ":") return;
          const c = l.indexOf(":");
          let field = l, value = "";
          if (c >= 0) {
            field = l.slice(0, c);
            value = l.slice(c + 1);
            if (value[0] === " ") value = value.slice(1);
          }
          if (field === "data") data += value + "\n";
          else if (field === "event") event = value;
          else if (field === "id") {
            if (value.indexOf("\0") < 0) id = value;
          } else if (field === "retry") {
            if (/^[0-9]+$/.test(value)) this.#retry = parseInt(value, 10);
          }
        };
        try {
          for (;;) {
            const { value, done } = await reader.read();
            if (this.#ctrl !== ctrl) return;
            if (done) break;
            buf += dec.decode(value, { stream: true });
            if (first && buf.length) {
              if (buf.charCodeAt(0) === 0xfeff) buf = buf.slice(1);
              first = false;
            }
            // A CR at the end may be the first half of a CRLF; keep it.
            let start = 0;
            for (let i = 0; i < buf.length; i++) {
              const ch = buf[i];
              if (ch === "\n" || ch === "\r") {
                if (ch === "\r" && i === buf.length - 1) break;
                line(buf.slice(start, i));
                if (ch === "\r" && buf[i + 1] === "\n") i++;
                start = i + 1;
              }
            }
            buf = buf.slice(start);
          }
        } catch (_) {
          // Network error mid-stream: reconnect, below.
        }
        if (this.#ctrl === ctrl) this.#fail(true);
      }
    }
    for (
      const [k, v] of [["CONNECTING", CONNECTING], ["OPEN", OPEN], [
        "CLOSED",
        CLOSED,
      ]]
    ) {
      Object.defineProperty(LaufeyEventSource.prototype, k, { value: v });
    }
    Object.defineProperty(G, "EventSource", {
      value: LaufeyEventSource,
      writable: true,
      configurable: true,
    });
  }

  // --- Asynchronous XMLHttpRequest over the shimmed fetch. A subclass, so
  // instanceof and every native code path (synchronous requests, other
  // origins and schemes) keep working; `upload` progress events are not
  // emitted for a shimmed request.
  if (NXHR) {
    const UNSENT = 0, OPENED = 1, HEADERS_RECEIVED = 2, LOADING = 3, DONE = 4;
    const NativeProto = NXHR.prototype;
    const nget = (name) =>
      Object.getOwnPropertyDescriptor(NativeProto, name).get;
    const nset = (name) =>
      Object.getOwnPropertyDescriptor(NativeProto, name).set;
    class LaufeyXMLHttpRequest extends NXHR {
      #s = null; // shim state; null while native
      #responseType = "";
      #timeout = 0;
      #creds = false;
      open(method, url, async, _user, _password) {
        let abs = null;
        try {
          abs = new NURL(url, G.location.href).href;
        } catch (_) {
          // Let the native open throw the right error.
        }
        const isAsync = arguments.length < 3 || !!async;
        if (abs && isAsync && eligible(abs) && registry()) {
          if (this.#s && this.#s.abort) this.#s.abort(true);
          this.#s = {
            method: String(method).toUpperCase(),
            url: abs,
            headers: new NHeaders(),
            state: OPENED,
            status: 0,
            statusText: "",
            responseURL: "",
            respHeaders: null,
            chunks: [],
            loaded: 0,
            text: "",
            decoder: null,
            mime: null,
            sent: false,
            abort: null,
            final: undefined,
          };
          this.#emit("readystatechange");
          return;
        }
        this.#s = null;
        return apply(NativeProto.open, this, arguments);
      }
      setRequestHeader(name, value) {
        const s = this.#s;
        if (!s) return apply(NativeProto.setRequestHeader, this, arguments);
        if (s.state !== OPENED || s.sent) {
          throw new NDOMException(
            "The object's state must be OPENED.",
            "InvalidStateError",
          );
        }
        s.headers.append(name, value);
      }
      overrideMimeType(mime) {
        if (this.#s) this.#s.mime = String(mime);
        return apply(NativeProto.overrideMimeType, this, arguments);
      }
      get readyState() {
        return this.#s ? this.#s.state : apply(nget("readyState"), this, []);
      }
      get status() {
        return this.#s ? this.#s.status : apply(nget("status"), this, []);
      }
      get statusText() {
        return this.#s
          ? this.#s.statusText
          : apply(nget("statusText"), this, []);
      }
      get responseURL() {
        return this.#s
          ? this.#s.responseURL
          : apply(nget("responseURL"), this, []);
      }
      get responseType() {
        return this.#s
          ? this.#responseType
          : apply(nget("responseType"), this, []);
      }
      set responseType(v) {
        if (this.#s) {
          if (this.#s.state === LOADING || this.#s.state === DONE) {
            throw new NDOMException(
              "Cannot set responseType now.",
              "InvalidStateError",
            );
          }
        } else {
          apply(nset("responseType"), this, [v]);
        }
        const ok = ["", "text", "json", "arraybuffer", "blob", "document"];
        if (ok.indexOf(v) >= 0) this.#responseType = v;
      }
      get timeout() {
        return this.#s ? this.#timeout : apply(nget("timeout"), this, []);
      }
      set timeout(v) {
        this.#timeout = Math.max(0, Number(v) || 0);
        if (!this.#s) apply(nset("timeout"), this, [v]);
      }
      get withCredentials() {
        return this.#s ? this.#creds : apply(nget("withCredentials"), this, []);
      }
      set withCredentials(v) {
        this.#creds = !!v;
        if (!this.#s) apply(nset("withCredentials"), this, [v]);
      }
      get responseText() {
        const s = this.#s;
        if (!s) return apply(nget("responseText"), this, []);
        if (this.#responseType !== "" && this.#responseType !== "text") {
          throw new NDOMException(
            "responseText is only available if responseType is '' or 'text'.",
            "InvalidStateError",
          );
        }
        return s.state === LOADING || s.state === DONE ? s.text : "";
      }
      get response() {
        const s = this.#s;
        if (!s) return apply(nget("response"), this, []);
        const rt = this.#responseType;
        if (rt === "" || rt === "text") return this.responseText;
        if (s.state !== DONE) return null;
        if (s.final !== undefined) return s.final;
        const bytes = this.#bytes();
        const mime = this.#mimeType();
        if (rt === "arraybuffer") s.final = bytes.buffer;
        else if (rt === "blob") s.final = new NBlob([bytes], { type: mime });
        else if (rt === "json") {
          try {
            s.final = JSON.parse(new NTextDecoder().decode(bytes));
          } catch (_) {
            s.final = null;
          }
        } else if (rt === "document") {
          const type = mime.split(";")[0].trim().toLowerCase();
          const xml = type === "text/xml" || type === "application/xml" ||
            type.endsWith("+xml");
          s.final = type === "text/html" || xml
            ? new NDOMParser().parseFromString(
              new NTextDecoder(this.#charset()).decode(bytes),
              xml ? "application/xml" : "text/html",
            )
            : null;
        }
        return s.final;
      }
      get responseXML() {
        const s = this.#s;
        if (!s) return apply(nget("responseXML"), this, []);
        const rt = this.#responseType;
        if (rt !== "" && rt !== "document") {
          throw new NDOMException(
            "responseXML is unavailable for this responseType.",
            "InvalidStateError",
          );
        }
        if (s.state !== DONE) return null;
        const saved = this.#responseType;
        this.#responseType = "document";
        try {
          return this.response;
        } finally {
          this.#responseType = saved;
        }
      }
      getResponseHeader(name) {
        const s = this.#s;
        if (!s) return apply(NativeProto.getResponseHeader, this, arguments);
        return s.respHeaders ? s.respHeaders.get(name) : null;
      }
      getAllResponseHeaders() {
        const s = this.#s;
        if (!s) {
          return apply(NativeProto.getAllResponseHeaders, this, arguments);
        }
        if (!s.respHeaders) return "";
        let out = "";
        for (const [k, v] of s.respHeaders) out += k + ": " + v + "\r\n";
        return out;
      }
      abort() {
        const s = this.#s;
        if (!s) return apply(NativeProto.abort, this, arguments);
        if (s.abort) s.abort(false);
        else if (s.state === DONE || (s.state === OPENED && !s.sent)) {
          s.state = UNSENT;
        }
      }
      send(body) {
        const s = this.#s;
        if (!s) return apply(NativeProto.send, this, arguments);
        if (s.state !== OPENED || s.sent) {
          throw new NDOMException(
            "The object's state must be OPENED.",
            "InvalidStateError",
          );
        }
        s.sent = true;
        const ctrl = new NAbortController();
        let timer = 0;
        let finished = false;
        const finish = (kind) => {
          if (finished) return;
          finished = true;
          clearTimeout_(timer);
          s.abort = null;
          ctrl.abort();
          if (kind !== "load") {
            s.status = 0;
            s.statusText = "";
            s.respHeaders = null;
            s.chunks = [];
            s.text = "";
          }
          s.state = DONE;
          this.#emit("readystatechange");
          if (kind === "load") this.#progress("progress");
          this.#progress(kind);
          this.#progress("loadend");
        };
        s.abort = (silent) => {
          if (silent) {
            finished = true;
            ctrl.abort();
            clearTimeout_(timer);
            s.abort = null;
            return;
          }
          finish("abort");
          s.state = UNSENT;
        };
        if (this.#timeout > 0) {
          timer = setTimeout_(() => finish("timeout"), this.#timeout);
        }
        const hasBody = s.method !== "GET" && s.method !== "HEAD" &&
          body != null;
        let req;
        try {
          req = new NRequest(s.url, {
            method: s.method,
            headers: s.headers,
            body: hasBody ? body : undefined,
            credentials: this.#creds ? "include" : "same-origin",
            signal: ctrl.signal,
          });
        } catch (_) {
          setTimeout_(() => finish("error"));
          return;
        }
        this.#progress("loadstart");
        (async () => {
          let resp;
          try {
            resp = await (maybeStream(req) || apply(origFetch, G, [req]));
          } catch (_) {
            finish("error");
            return;
          }
          if (finished) return;
          s.status = resp.status;
          s.statusText = resp.statusText;
          s.responseURL = resp.url;
          s.respHeaders = resp.headers;
          s.total = Number(resp.headers.get("content-length")) || 0;
          s.state = HEADERS_RECEIVED;
          this.#emit("readystatechange");
          if (finished || !resp.body) {
            if (!finished) finish("load");
            return;
          }
          const reader = resp.body.getReader();
          s.decoder = new NTextDecoder(this.#charset());
          try {
            for (;;) {
              const { value, done } = await reader.read();
              if (finished) return;
              if (done) break;
              s.chunks.push(value);
              s.loaded += value.byteLength;
              if (this.#responseType === "" || this.#responseType === "text") {
                s.text += s.decoder.decode(value, { stream: true });
              }
              s.state = LOADING;
              this.#emit("readystatechange");
              this.#progress("progress");
            }
          } catch (_) {
            finish("error");
            return;
          }
          if (this.#responseType === "" || this.#responseType === "text") {
            s.text += s.decoder.decode();
          }
          finish("load");
        })();
      }
      #bytes() {
        const s = this.#s;
        const out = new Uint8Array(s.loaded);
        let o = 0;
        for (const c of s.chunks) {
          out.set(c, o);
          o += c.byteLength;
        }
        return out;
      }
      #mimeType() {
        const s = this.#s;
        return s.mime || (s.respHeaders && s.respHeaders.get("content-type")) ||
          "";
      }
      #charset() {
        const m = /;\s*charset\s*=\s*"?([^";\s]+)/i.exec(this.#mimeType());
        try {
          if (m) {
            new NTextDecoder(m[1]);
            return m[1];
          }
        } catch (_) {
          // Unknown label: fall back to UTF-8, as XHR does.
        }
        return "utf-8";
      }
      #emit(type) {
        this.dispatchEvent(new NEvent(type));
      }
      #progress(type) {
        const s = this.#s;
        this.dispatchEvent(
          new NProgressEvent(type, {
            lengthComputable: !!s.total,
            loaded: s.loaded,
            total: s.total || 0,
          }),
        );
      }
    }
    Object.defineProperty(G, "XMLHttpRequest", {
      value: LaufeyXMLHttpRequest,
      writable: true,
      configurable: true,
    });
  }
})();
