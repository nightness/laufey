// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The CEF backend runs web content in Chromium's sandbox
//! (docs/backends.md, "The Chromium sandbox"). Part of the CEF battery and
//! `LAUFEY_E2E_ONLY=sandbox` (native-e2e-run.sh --sandbox).
//!
//! The checks look at the backend's renderer (and GPU) processes from outside,
//! through the OS:
//!
//! - Linux: `/proc/<pid>/status` shows a seccomp-bpf filter (`Seccomp: 2`),
//!   `NoNewPrivs: 1`, and for the renderer a PID namespace of its own (a
//!   nested `NSpid`), which both layer-1 sandboxes (user namespaces, the
//!   setuid helper) create.
//! - macOS: `sandbox_check(pid, NULL, 0)` reports the helper as sandboxed
//!   (Seatbelt).
//! - Windows: the renderer and GPU processes (told apart by their command
//!   line's `--type=`) run below Medium integrity (the renderer's token is
//!   lowered to Untrusted, the GPU process's to Low) or in an AppContainer,
//!   while the browser process itself does not.
//!
//! `LAUFEY_E2E_EXPECT_SANDBOX=0` inverts the checks, for a run where the host
//! must have turned the sandbox off (a Linux machine with neither user
//! namespaces nor the setuid helper, a Windows build with -DUSE_SANDBOX=OFF):
//! web content still renders, unsandboxed.
//! On Linux, `LAUFEY_E2E_EXPECT_SANDBOX_MODE=namespace|setuid|off` also names
//! the layer-1 sandbox the renderers must be in (a user namespace of their
//! own, or only the helper's PID namespace); native-e2e-run.sh checks the
//! host's `laufey: sandbox: <mode>` line against it, and these checks the
//! `sandbox` platform fact.

use std::time::Duration;

use laufey::{Value, Window};

use super::{check, na};

/// What the run expects: LAUFEY_E2E_EXPECT_SANDBOX when set, else sandboxed.
/// LAUFEY_E2E_EXPECT_SANDBOX_MODE=off expects it off too.
fn expect_sandboxed() -> bool {
  if expect_mode().as_deref() == Some("off") {
    return false;
  }
  !matches!(
    std::env::var("LAUFEY_E2E_EXPECT_SANDBOX").as_deref(),
    Ok("0")
  )
}

/// The Linux layer-1 sandbox the run expects (LAUFEY_E2E_EXPECT_SANDBOX_MODE:
/// `namespace`, `setuid` or `off`, the mode the host logs as
/// `laufey: sandbox: <mode>`), when it names one.
fn expect_mode() -> Option<String> {
  std::env::var("LAUFEY_E2E_EXPECT_SANDBOX_MODE")
    .ok()
    .filter(|m| !m.is_empty())
}

/// A child process of the backend, as the OS reports it.
#[derive(Debug, Clone)]
#[allow(dead_code)]
struct Child {
  pid: u32,
  /// "renderer", "gpu-process", ... ("" when the OS doesn't say).
  kind: String,
}

pub async fn run() {
  if std::env::var("LAUFEY_E2E_BACKEND").as_deref() != Ok("cef") {
    na("sandbox: web content runs in the engine's own sandbox (not CEF)");
    return;
  }
  // A page, so a renderer exists, and proof it renders under the sandbox.
  let win = Window::new(320, 240)
    .title("native-e2e-sandbox")
    .load("laufey-e2e://app/");
  win.show();
  let answered = page_answers(&win).await;
  check(
    &format!(
      "sandbox: a page renders and runs script (sandbox expected: {})",
      expect_sandboxed()
    ),
    answered,
  );
  os_checks().await;
  #[cfg(target_os = "linux")]
  platform_fact_checks();
  let _ = &win;
}

/// The host reports the sandbox it chose as a platform fact (`sandbox`,
/// `sandboxReason`): the mode the run expects, with a reason.
#[cfg(target_os = "linux")]
fn platform_fact_checks() {
  let json = laufey::platform_features().unwrap_or_default();
  let mode = json
    .split("\"sandbox\":\"")
    .nth(1)
    .and_then(|rest| rest.split('"').next())
    .map(str::to_owned);
  let want = expect_mode();
  check(
    &format!(
      "sandbox: platform_features reports the sandbox mode ({mode:?}, \
       expected {})",
      want.as_deref().unwrap_or("any")
    ),
    match (&mode, &want) {
      (Some(m), Some(w)) => m == w,
      (Some(m), None) => {
        ["namespace", "setuid", "chromium", "off"].contains(&m.as_str())
      }
      _ => false,
    },
  );
  check(
    "sandbox: platform_features says why (sandboxReason)",
    json.contains("\"sandboxReason\":\""),
  );
}

async fn page_answers(win: &Window) -> bool {
  for _ in 0..100 {
    let (tx, rx) = tokio::sync::oneshot::channel();
    win.execute_js(
      "1 + 1",
      Some(move |r: Result<Value, Value>| {
        let _ = tx.send(r);
      }),
    );
    if let Ok(Ok(Ok(Value::Int(2)))) =
      tokio::time::timeout(Duration::from_secs(1), rx).await
    {
      return true;
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
  }
  false
}

/// Waits for at least one child of `kind` (the renderer starts with the page,
/// the GPU process may come a moment later).
async fn wait_children(kind: &str) -> Vec<Child> {
  for _ in 0..50 {
    let found: Vec<Child> =
      children().into_iter().filter(|c| c.kind == kind).collect();
    if !found.is_empty() {
      return found;
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
  }
  Vec::new()
}

// ---- Linux ---------------------------------------------------------------
//
// The renderers can't be told apart by their command lines: laufey hands CEF
// a copy of argv, so the title Chromium sets for a process forked from the
// zygote never reaches /proc/<pid>/cmdline, which keeps reading
// "--type=zygote". They are told apart by where they sit instead: a renderer
// is a leaf forked from the sandboxed zygote (the one started without
// --no-zygote-sandbox), and the GPU process is the one running a GPU
// watchdog thread.

#[cfg(target_os = "linux")]
struct Proc {
  pid: u32,
  ppid: u32,
  args: Vec<String>,
}

#[cfg(target_os = "linux")]
impl Proc {
  fn kind(&self) -> &str {
    self
      .args
      .iter()
      .find_map(|a| a.strip_prefix("--type="))
      .unwrap_or("")
  }
}

#[cfg(target_os = "linux")]
fn parent_of(pid: u32) -> Option<u32> {
  let stat = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
  // "pid (comm) state ppid ...": comm may hold spaces and parentheses.
  let rest = &stat[stat.rfind(')')? + 1..];
  rest.split_whitespace().nth(1)?.parse().ok()
}

/// Every process below this one.
#[cfg(target_os = "linux")]
fn descendants() -> Vec<Proc> {
  let me = std::process::id();
  let mut all = Vec::new();
  if let Ok(dir) = std::fs::read_dir("/proc") {
    for entry in dir.flatten() {
      let Some(pid) = entry.file_name().to_str().and_then(|s| s.parse().ok())
      else {
        continue;
      };
      let Some(ppid) = parent_of(pid) else { continue };
      let args = std::fs::read(format!("/proc/{pid}/cmdline"))
        .unwrap_or_default()
        .split(|b| *b == 0)
        .filter_map(|a| std::str::from_utf8(a).ok().map(str::to_string))
        .collect();
      all.push(Proc { pid, ppid, args });
    }
  }
  let parent: std::collections::HashMap<u32, u32> =
    all.iter().map(|p| (p.pid, p.ppid)).collect();
  let below_me = |mut pid: u32| {
    for _ in 0..32 {
      match parent.get(&pid) {
        Some(&p) if p == me => return true,
        Some(&p) if p > 1 => pid = p,
        _ => return false,
      }
    }
    false
  };
  all
    .into_iter()
    .filter(|p| p.pid != me && below_me(p.pid))
    .collect()
}

#[cfg(target_os = "linux")]
fn children() -> Vec<Child> {
  let procs = descendants();
  let by_pid: std::collections::HashMap<u32, &Proc> =
    procs.iter().map(|p| (p.pid, p)).collect();
  let has_children = |pid: u32| procs.iter().any(|p| p.ppid == pid);
  let mut out = Vec::new();
  for p in &procs {
    let kind = if p.kind() == "renderer" {
      "renderer"
    } else if thread_names(p.pid)
      .iter()
      .any(|t| t == "GpuWatchdog" || t == "VizCompositorTh")
    {
      "gpu-process"
    } else if p.kind() == "zygote" && !has_children(p.pid) {
      // A leaf forked from a zygote: a renderer when that zygote is the
      // sandboxed one.
      match by_pid.get(&p.ppid) {
        Some(z)
          if z.kind() == "zygote"
            && !z.args.iter().any(|a| a == "--no-zygote-sandbox") =>
        {
          "renderer"
        }
        _ => continue,
      }
    } else {
      continue;
    };
    out.push(Child {
      pid: p.pid,
      kind: kind.to_string(),
    });
  }
  out
}

#[cfg(target_os = "linux")]
fn thread_names(pid: u32) -> Vec<String> {
  let Ok(tasks) = std::fs::read_dir(format!("/proc/{pid}/task")) else {
    return Vec::new();
  };
  tasks
    .flatten()
    .filter_map(|t| std::fs::read_to_string(t.path().join("comm")).ok())
    .map(|n| n.trim().to_string())
    .collect()
}

#[cfg(target_os = "linux")]
fn status_field(pid: u32, field: &str) -> Option<String> {
  let status = std::fs::read_to_string(format!("/proc/{pid}/status")).ok()?;
  status.lines().find_map(|l| {
    l.strip_prefix(field)
      .and_then(|r| r.strip_prefix(':'))
      .map(|v| v.trim().to_string())
  })
}

#[cfg(target_os = "linux")]
async fn os_checks() {
  let want = expect_sandboxed();
  let renderers = wait_children("renderer").await;
  check(
    &format!("sandbox: the backend has renderer processes ({renderers:?})"),
    !renderers.is_empty(),
  );
  for r in &renderers {
    let seccomp = status_field(r.pid, "Seccomp");
    let nnp = status_field(r.pid, "NoNewPrivs");
    let nspid = status_field(r.pid, "NSpid");
    let nested = nspid
      .as_deref()
      .map(|v| v.split_whitespace().count() > 1)
      .unwrap_or(false);
    let sandboxed =
      seccomp.as_deref() == Some("2") && nnp.as_deref() == Some("1") && nested;
    check(
      &format!(
        "sandbox: renderer {} {} (Seccomp {seccomp:?}, NoNewPrivs {nnp:?}, \
         NSpid {nspid:?})",
        r.pid,
        if want {
          "is sandboxed"
        } else {
          "runs unsandboxed"
        }
      ),
      sandboxed == want,
    );
    // Which layer 1: the namespace sandbox puts the renderer in a user
    // namespace of its own (another uid_map than the browser's); the setuid
    // helper only creates PID and network namespaces.
    if let Some(mode) = expect_mode().filter(|m| m != "off") {
      let own = std::fs::read_to_string("/proc/self/uid_map").ok();
      let theirs =
        std::fs::read_to_string(format!("/proc/{}/uid_map", r.pid)).ok();
      let layer = match (&own, &theirs) {
        (Some(a), Some(b)) if a != b => "namespace",
        (Some(_), Some(_)) => "setuid",
        _ => "unknown",
      };
      check(
        &format!(
          "sandbox: renderer {} uses the {mode} sandbox (found {layer}; \
           uid_map {:?})",
          r.pid,
          theirs.as_deref().map(str::trim)
        ),
        layer == mode,
      );
    }
  }
  let gpu = wait_children("gpu-process").await;
  if gpu.is_empty() {
    na("sandbox: no GPU process to check");
  }
  for g in &gpu {
    let seccomp = status_field(g.pid, "Seccomp");
    let filtered = seccomp.as_deref() == Some("2");
    // Chromium starts the GPU sandbox only in a single-threaded GPU process
    // (sandbox_linux.cc, "InitializeSandbox() called with multiple threads
    // in process gpu-process"); Mesa's llvmpipe, the software GL under Xvfb,
    // starts its threads first, so there the GPU process stays unsandboxed
    // by Chromium's own choice. A hardware GL driver doesn't.
    let llvmpipe = thread_names(g.pid)
      .iter()
      .any(|t| t.starts_with("llvmpipe"));
    if want && !filtered && llvmpipe {
      na(&format!(
        "sandbox: GPU process {} runs Mesa llvmpipe, which Chromium leaves \
         unsandboxed (Seccomp {seccomp:?})",
        g.pid
      ));
      continue;
    }
    check(
      &format!(
        "sandbox: GPU process {} {} (Seccomp {seccomp:?})",
        g.pid,
        if want {
          "runs under seccomp-bpf"
        } else {
          "runs unsandboxed"
        }
      ),
      filtered == want,
    );
  }
}

// ---- macOS ---------------------------------------------------------------

#[cfg(target_os = "macos")]
fn children() -> Vec<Child> {
  // Every process with its parent and command line; the helpers are direct
  // children of the browser process.
  let me = std::process::id();
  let Ok(out) = std::process::Command::new("/bin/ps")
    .args(["-A", "-o", "pid=,ppid=,command="])
    .output()
  else {
    return Vec::new();
  };
  String::from_utf8_lossy(&out.stdout)
    .lines()
    .filter_map(|line| {
      let mut it = line.split_whitespace();
      let pid: u32 = it.next()?.parse().ok()?;
      let ppid: u32 = it.next()?.parse().ok()?;
      if ppid != me {
        return None;
      }
      let kind = it
        .find_map(|a| a.strip_prefix("--type="))
        .unwrap_or("")
        .to_string();
      Some(Child { pid, kind })
    })
    .collect()
}

#[cfg(target_os = "macos")]
extern "C" {
  // libsystem_sandbox (part of libSystem): 1 if `pid` is sandboxed.
  fn sandbox_check(
    pid: i32,
    operation: *const std::ffi::c_char,
    filter_type: i32,
    ...
  ) -> i32;
}

#[cfg(target_os = "macos")]
async fn os_checks() {
  let want = expect_sandboxed();
  // Not every helper is sandboxed by every policy; the renderer and the
  // GPU process are.
  for kind in ["renderer", "gpu-process"] {
    let found = wait_children(kind).await;
    check(
      &format!("sandbox: the backend has {kind} processes ({found:?})"),
      !found.is_empty(),
    );
    for c in &found {
      let sandboxed =
        unsafe { sandbox_check(c.pid as i32, std::ptr::null(), 0) } == 1;
      check(
        &format!(
          "sandbox: {kind} {} {} (sandbox_check {})",
          c.pid,
          if want {
            "is sandboxed"
          } else {
            "runs unsandboxed"
          },
          sandboxed
        ),
        sandboxed == want,
      );
    }
  }
}

// ---- Windows -------------------------------------------------------------

#[cfg(target_os = "windows")]
mod win {
  use std::ffi::c_void;

  pub type Handle = *mut c_void;
  pub const TH32CS_SNAPPROCESS: u32 = 0x2;
  pub const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
  pub const TOKEN_QUERY: u32 = 0x8;
  pub const TOKEN_INTEGRITY_LEVEL: i32 = 25;
  pub const TOKEN_IS_APP_CONTAINER: i32 = 29;
  /// NtQueryInformationProcess: the command line as a UNICODE_STRING
  /// (Windows 8.1+; PROCESS_QUERY_LIMITED_INFORMATION is enough).
  pub const PROCESS_COMMAND_LINE_INFORMATION: u32 = 60;
  pub const INVALID_HANDLE_VALUE: Handle = -1isize as Handle;

  #[repr(C)]
  pub struct ProcessEntry32W {
    pub dw_size: u32,
    pub cnt_usage: u32,
    pub th32_process_id: u32,
    pub th32_default_heap_id: usize,
    pub th32_module_id: u32,
    pub cnt_threads: u32,
    pub th32_parent_process_id: u32,
    pub pc_pri_class_base: i32,
    pub dw_flags: u32,
    pub sz_exe_file: [u16; 260],
  }

  #[repr(C)]
  pub struct UnicodeString {
    pub length: u16,
    pub maximum_length: u16,
    pub buffer: *const u16,
  }

  #[link(name = "kernel32")]
  extern "system" {
    pub fn CreateToolhelp32Snapshot(flags: u32, pid: u32) -> Handle;
    pub fn Process32FirstW(snap: Handle, entry: *mut ProcessEntry32W) -> i32;
    pub fn Process32NextW(snap: Handle, entry: *mut ProcessEntry32W) -> i32;
    pub fn OpenProcess(access: u32, inherit: i32, pid: u32) -> Handle;
    pub fn CloseHandle(h: Handle) -> i32;
  }
  #[link(name = "advapi32")]
  extern "system" {
    pub fn OpenProcessToken(p: Handle, access: u32, token: *mut Handle) -> i32;
    pub fn GetTokenInformation(
      token: Handle,
      class: i32,
      info: *mut c_void,
      len: u32,
      ret_len: *mut u32,
    ) -> i32;
    pub fn GetSidSubAuthorityCount(sid: *mut c_void) -> *mut u8;
    pub fn GetSidSubAuthority(sid: *mut c_void, index: u32) -> *mut u32;
  }
  #[link(name = "ntdll")]
  extern "system" {
    pub fn NtQueryInformationProcess(
      process: Handle,
      class: u32,
      info: *mut c_void,
      len: u32,
      ret_len: *mut u32,
    ) -> i32;
  }
}

/// The `--type=` of a child's command line ("" for none or unreadable).
#[cfg(target_os = "windows")]
fn process_kind(pid: u32) -> String {
  use win::*;
  unsafe {
    let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
    if process.is_null() {
      return String::new();
    }
    // A UNICODE_STRING header followed by its buffer.
    let mut buf = vec![0u64; 8192];
    let mut len = 0u32;
    let status = NtQueryInformationProcess(
      process,
      PROCESS_COMMAND_LINE_INFORMATION,
      buf.as_mut_ptr() as *mut _,
      (buf.len() * 8) as u32,
      &mut len,
    );
    CloseHandle(process);
    if status < 0 {
      return String::new();
    }
    let us = &*(buf.as_ptr() as *const UnicodeString);
    if us.buffer.is_null() {
      return String::new();
    }
    let wide = std::slice::from_raw_parts(us.buffer, (us.length / 2) as usize);
    String::from_utf16_lossy(wide)
      .split_whitespace()
      .find_map(|a| a.trim_matches('"').strip_prefix("--type="))
      .unwrap_or("")
      .to_string()
  }
}

#[cfg(target_os = "windows")]
fn children() -> Vec<Child> {
  use win::*;
  let me = std::process::id();
  let mut all: Vec<(u32, u32)> = Vec::new();
  unsafe {
    let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if snap == INVALID_HANDLE_VALUE {
      return Vec::new();
    }
    let mut e: ProcessEntry32W = std::mem::zeroed();
    e.dw_size = std::mem::size_of::<ProcessEntry32W>() as u32;
    let mut ok = Process32FirstW(snap, &mut e);
    while ok != 0 {
      all.push((e.th32_process_id, e.th32_parent_process_id));
      ok = Process32NextW(snap, &mut e);
    }
    CloseHandle(snap);
  }
  // Direct children only: CEF's subprocesses are started by the browser.
  all
    .into_iter()
    .filter(|(pid, ppid)| *ppid == me && *pid != me)
    .map(|(pid, _)| Child {
      pid,
      kind: process_kind(pid),
    })
    .collect()
}

/// A process token's sandbox-relevant facts.
#[cfg(target_os = "windows")]
#[derive(Debug, Clone, Copy)]
struct TokenFacts {
  /// The mandatory integrity level RID (0x0000 untrusted, 0x1000 low,
  /// 0x2000 medium, ...).
  integrity: u32,
  /// An AppContainer (or less privileged AppContainer) token.
  app_container: bool,
}

#[cfg(target_os = "windows")]
fn token_facts(pid: u32) -> Option<TokenFacts> {
  use win::*;
  unsafe {
    let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
    if process.is_null() {
      return None;
    }
    let mut token: Handle = std::ptr::null_mut();
    let opened = OpenProcessToken(process, TOKEN_QUERY, &mut token);
    CloseHandle(process);
    if opened == 0 {
      return None;
    }
    let mut buf = vec![0u8; 256];
    let mut len = 0u32;
    let got = GetTokenInformation(
      token,
      TOKEN_INTEGRITY_LEVEL,
      buf.as_mut_ptr() as *mut _,
      buf.len() as u32,
      &mut len,
    );
    let mut is_ac = 0u32;
    let got_ac = GetTokenInformation(
      token,
      TOKEN_IS_APP_CONTAINER,
      &mut is_ac as *mut u32 as *mut _,
      4,
      &mut len,
    );
    CloseHandle(token);
    if got == 0 {
      return None;
    }
    // TOKEN_MANDATORY_LABEL { SID_AND_ATTRIBUTES { PSID Sid; DWORD Attributes } }
    let sid = *(buf.as_ptr() as *const *mut std::ffi::c_void);
    let count = *GetSidSubAuthorityCount(sid);
    if count == 0 {
      return None;
    }
    Some(TokenFacts {
      integrity: *GetSidSubAuthority(sid, (count - 1) as u32),
      app_container: got_ac != 0 && is_ac != 0,
    })
  }
}

/// Sandboxed as Chromium sandboxes a renderer or the GPU process on Windows:
/// a token lowered below Medium integrity (Low, Untrusted) or an
/// AppContainer.
#[cfg(target_os = "windows")]
fn is_sandboxed(t: &TokenFacts) -> bool {
  const MEDIUM: u32 = 0x2000;
  t.integrity < MEDIUM || t.app_container
}

#[cfg(target_os = "windows")]
async fn os_checks() {
  let want = expect_sandboxed();
  let me = token_facts(std::process::id());
  check(
    &format!("sandbox: the browser process runs unsandboxed ({me:x?})"),
    me.is_some_and(|t| !is_sandboxed(&t)),
  );
  // Not every helper is sandboxed by every policy; the renderer and the
  // GPU process are. A renderer lowers its token a moment after it starts,
  // so wait for the expected state before judging it, for 10 s at most per
  // kind: each round can itself wait 5 s for a first child (wait_children),
  // so a bound on the rounds alone would let a missing kind take minutes.
  for kind in ["renderer", "gpu-process"] {
    let mut found: Vec<(u32, Option<TokenFacts>)> = Vec::new();
    let deadline = std::time::Instant::now() + Duration::from_secs(10);
    loop {
      found = wait_children(kind)
        .await
        .into_iter()
        .map(|c| (c.pid, token_facts(c.pid)))
        .collect();
      let settled = !found.is_empty()
        && found
          .iter()
          .all(|(_, t)| t.is_some_and(|t| is_sandboxed(&t) == want));
      if settled || std::time::Instant::now() >= deadline {
        break;
      }
      tokio::time::sleep(Duration::from_millis(100)).await;
    }
    check(
      &format!("sandbox: the backend has {kind} processes ({found:x?})"),
      !found.is_empty(),
    );
    for (pid, facts) in &found {
      check(
        &format!(
          "sandbox: {kind} {pid} {} ({facts:x?})",
          if want {
            "runs below Medium integrity or in an AppContainer"
          } else {
            "runs at the browser's integrity"
          }
        ),
        facts.is_some_and(|t| is_sandboxed(&t) == want),
      );
    }
  }
}

#[cfg(not(any(
  target_os = "linux",
  target_os = "macos",
  target_os = "windows"
)))]
fn children() -> Vec<Child> {
  Vec::new()
}

#[cfg(not(any(
  target_os = "linux",
  target_os = "macos",
  target_os = "windows"
)))]
async fn os_checks() {
  na("sandbox: no OS check on this platform");
}
