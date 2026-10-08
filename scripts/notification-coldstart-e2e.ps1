# The cold-start click on Windows: a click on one of the app's toasts while the
# app isn't running starts it (COM activates the LocalServer32 the app
# registered for its AppUserModelID) and the click reaches its notification
# response handler as the launch. See docs/notifications.md.
#
#   pwsh scripts/notification-coldstart-e2e.ps1 <webview|cef>
#
# Runs after `scripts/native-e2e-run.sh <backend> --menus-notifications`,
# which registered the AppUserModelID dev.laufey.e2e.notifications and its
# activator. Copies the backend (and the native_e2e runtime as its colocated
# runtime, with a laufey-launch.json naming the same app id) to a temporary
# folder, points the activator's LocalServer32 at the copy, then does what
# Windows does for a toast click: CoCreateInstance on the activator's CLSID
# (no process serves it, so COM starts the copy with -ToastActivated
# -Embedding) and INotificationActivationCallback::Activate with the toast's
# arguments. The runtime's cold-start mode writes what its response handler
# received to %TEMP%\laufey-coldstart-result.txt.
#
# Any process of the user can call Activate, so laufey accepts only
# arguments MAC'd with the install's key (laufey-notification-key in the app
# data directory). A forged Activate (well-formed arguments, no MAC) goes
# first and must not arrive, nor use up the launch; then the genuine click,
# signed here with the key as laufey signs what it posts.
param([Parameter(Mandatory = $true)][ValidateSet("webview", "cef")][string]$Backend)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$aumid = "dev.laufey.e2e.notifications"
$runtime = Join-Path $root "target\release\native_e2e_runtime.dll"
if ($Backend -eq "webview") {
  $src = Join-Path $root "webview\build"
  $exeName = "laufey_webview.exe"
} else {
  $src = Join-Path $root "cef\build\Release"
  $exeName = "laufey.exe"
}
$dir = Join-Path $env:TEMP "laufey-coldstart-$Backend"
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory $dir | Out-Null
if ($Backend -eq "webview") {
  Copy-Item (Join-Path $src $exeName) $dir
  $loader = Join-Path $src "WebView2Loader.dll"
  if (Test-Path $loader) { Copy-Item $loader $dir }
} else {
  Copy-Item "$src\*" $dir -Recurse
}
$exe = Join-Path $dir $exeName
# The CEF executable is CEF's bootstrap and <exe>.dll the host it loads, so
# the runtime is <exe>.runtime.dll there.
$rtExt = if ($Backend -eq "cef") { ".runtime.dll" } else { ".dll" }
Copy-Item $runtime ([IO.Path]::ChangeExtension($exe, $rtExt))
"{`"appId`": `"$aumid`"}" | Set-Content -Encoding ascii (Join-Path $dir "laufey-launch.json")

$clsid = (Get-ItemProperty "HKCU:\Software\Classes\AppUserModelId\$aumid").CustomActivator
if (-not $clsid) { throw "no activator registered for $aumid (run --menus-notifications first)" }
$server = "HKCU:\Software\Classes\CLSID\$clsid\LocalServer32"
$previous = (Get-ItemProperty $server)."(default)"
Set-ItemProperty $server -Name "(default)" -Value "`"$exe`" -ToastActivated"
$result = Join-Path $env:TEMP "laufey-coldstart-result.txt"
Remove-Item $result -ErrorAction SilentlyContinue

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("53E31837-6600-4A81-9395-75CFFE746F94"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface INotificationActivationCallback {
  void Activate([MarshalAs(UnmanagedType.LPWStr)] string appUserModelId,
                [MarshalAs(UnmanagedType.LPWStr)] string invokedArgs,
                IntPtr data, uint count);
}
public static class ToastClick {
  public static void Click(string clsid, string aumid, string args) {
    object server = Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid(clsid)));
    try {
      ((INotificationActivationCallback)server).Activate(aumid, args, IntPtr.Zero, 0);
    } finally {
      Marshal.ReleaseComObject(server);
    }
  }
}
"@

# The install's key (created here when no run made one yet; the app reads it).
$dataDir = Join-Path $env:LOCALAPPDATA $aumid
New-Item -ItemType Directory -Force $dataDir | Out-Null
$keyFile = Join-Path $dataDir "laufey-notification-key"
if (-not (Test-Path $keyFile)) {
  $bytes = New-Object byte[] 32
  [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
  (($bytes | ForEach-Object { $_.ToString("x2") }) -join "") + "`n" |
    Set-Content -NoNewline -Encoding ascii $keyFile
}
$hex = (Get-Content -Raw $keyFile).Trim()
$key = New-Object byte[] 32
for ($i = 0; $i -lt 32; $i++) { $key[$i] = [Convert]::ToByte($hex.Substring(2 * $i, 2), 16) }
function Sign([string]$plain) {
  $hmac = New-Object Security.Cryptography.HMACSHA256 (, $key)
  $mac = $hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("laufey-notification-click/1`n" + $plain))
  return $plain + "&mac=" + (($mac | ForEach-Object { $_.ToString("x2") }) -join "")
}

$failed = $false
try {
  # Forged: COM starts the app, which drops it; no result.
  [ToastClick]::Click($clsid, $aumid, "laufey=1&tag=forged-tag&data=%7B%22f%22%3A1%7D")
  Start-Sleep -Seconds 10
  if (Test-Path $result) {
    Write-Host "FAIL a forged Activate reached the response handler: $(Get-Content $result)"
    $failed = $true
    Remove-Item $result
  }
  # Genuine: the running copy (the COM server now) gets it, as the launch.
  [ToastClick]::Click($clsid, $aumid, (Sign "laufey=1&tag=cold-tag&action=cold-action&data=%7B%22c%22%3A1%7D"))
  $deadline = (Get-Date).AddSeconds(60)
  while (-not (Test-Path $result) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
  if (-not (Test-Path $result)) { throw "the started app wrote no result" }
  $lines = Get-Content $result
  Write-Host "[coldstart] $($lines -join ' | ')"
  if ($lines[0] -ne "cold-tag") { Write-Host "FAIL tag"; $failed = $true }
  if ($lines[1] -ne "cold-action") { Write-Host "FAIL action"; $failed = $true }
  if ($lines[2] -ne '{"c":1}') { Write-Host "FAIL data"; $failed = $true }
  if ($lines[3] -ne "true") { Write-Host "FAIL launch"; $failed = $true }
} catch {
  Write-Host "FAIL $_"
  $failed = $true
} finally {
  if ($previous) { Set-ItemProperty $server -Name "(default)" -Value $previous }
  Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($dir) } | ForEach-Object { try { $_.Kill() } catch {} }
}
if ($failed) { exit 1 }
Write-Host "[coldstart] PASS a forged Activate was dropped; a toast click started the app and reached its response handler as the launch"
