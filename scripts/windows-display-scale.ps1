# Raises the primary display's scale (Settings > Display > Scale) for the
# native e2e's --hidpi step on a Windows CI runner, without signing out:
# per-monitor DPI aware processes started afterwards (the WebView2 and CEF
# backends are) run at the new scale.
#
#   scripts/windows-display-scale.ps1 [-Percent 200]
#
# The runner's display is small, and Windows caps the scale by resolution, so
# this first switches to the largest display mode the adapter offers. The
# scale is set the way the Settings app does it (SPI_SETLOGICALDPIOVERRIDE,
# relative to the recommended scale, per user). It prints the effective scale
# a new process sees and, under GitHub Actions, writes `scale=<factor>` to
# $GITHUB_OUTPUT (1 if nothing above 100% could be set). The runner is
# discarded after the job, so nothing is restored.
param([int]$Percent = 200)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LaufeyDisplay {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
    public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public short dmLogPixels;
    public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2;
    public int dmPanningWidth, dmPanningHeight;
  }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern bool EnumDisplaySettingsW(string device, int mode, ref DEVMODE dm);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int ChangeDisplaySettingsW(ref DEVMODE dm, int flags);
  [DllImport("user32.dll")]
  public static extern bool SystemParametersInfoW(int action, int param, IntPtr pv, int flags);
  [DllImport("user32.dll")]
  public static extern bool SystemParametersInfoW(int action, int param, ref int pv, int flags);
  [DllImport("user32.dll")]
  public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")]
  public static extern IntPtr MonitorFromPoint(long pt, int flags);
  [DllImport("shcore.dll")]
  public static extern int GetDpiForMonitor(IntPtr mon, int type, out uint x, out uint y);

  public static DEVMODE Mode(int i) {
    var dm = new DEVMODE();
    dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
    if (!EnumDisplaySettingsW(null, i, ref dm)) dm.dmPelsWidth = -1;
    return dm;
  }

  // The primary monitor's effective DPI as a per-monitor aware thread sees it.
  public static uint EffectiveDpi() {
    SetThreadDpiAwarenessContext(new IntPtr(-4)); // PER_MONITOR_AWARE_V2
    IntPtr mon = MonitorFromPoint(0, 1);          // MONITOR_DEFAULTTOPRIMARY
    uint x, y;
    return GetDpiForMonitor(mon, 0, out x, out y) == 0 ? x : 0;
  }
}
'@

$current = [LaufeyDisplay]::Mode(-1) # ENUM_CURRENT_SETTINGS
Write-Host "display: $($current.dmPelsWidth)x$($current.dmPelsHeight), effective DPI $([LaufeyDisplay]::EffectiveDpi())"

# The largest mode on offer (at the current color depth).
$best = $current
for ($i = 0; ; $i++) {
  $m = [LaufeyDisplay]::Mode($i)
  if ($m.dmPelsWidth -lt 0) { break }
  if ($m.dmBitsPerPel -eq $current.dmBitsPerPel -and
      ($m.dmPelsWidth * $m.dmPelsHeight) -gt ($best.dmPelsWidth * $best.dmPelsHeight)) {
    $best = $m
  }
}
if ($best.dmPelsWidth -ne $current.dmPelsWidth -or $best.dmPelsHeight -ne $current.dmPelsHeight) {
  $best.dmFields = 0x80000 -bor 0x100000 # DM_PELSWIDTH | DM_PELSHEIGHT
  $r = [LaufeyDisplay]::ChangeDisplaySettingsW([ref]$best, 0)
  Write-Host "switch to $($best.dmPelsWidth)x$($best.dmPelsHeight): result $r (0 = done)"
  Start-Sleep -Seconds 2
}

# SPI_GETLOGICALDPIOVERRIDE / SPI_SETLOGICALDPIOVERRIDE take a step relative
# to the recommended scale in this ladder; walk up until the effective DPI
# reaches the target or stops rising.
$target = [int][Math]::Round(96 * $Percent / 100)
$rel = 0
[void][LaufeyDisplay]::SystemParametersInfoW(0x009E, 0, [ref]$rel, 0)
$last = [LaufeyDisplay]::EffectiveDpi()
Write-Host "relative scale step $rel, effective DPI $last; target $target"
for ($step = 0; $step -lt 12 -and $last -lt $target; $step++) {
  $rel++
  [void][LaufeyDisplay]::SystemParametersInfoW(0x009F, $rel, [IntPtr]::Zero, 1) # SPIF_UPDATEINIFILE
  Start-Sleep -Milliseconds 1500
  $dpi = [int][LaufeyDisplay]::EffectiveDpi()
  Write-Host "relative step $rel -> effective DPI $dpi"
  if ($dpi -le $last) { break }
  $last = $dpi
}

$scale = [Math]::Round($last / 96.0, 2)
Write-Host "effective scale: $scale"
if ($env:GITHUB_OUTPUT) {
  "scale=$scale" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
