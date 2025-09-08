# Apply best-effort real-time settings for CI environments on Windows runners.
Write-Host "[rt_harden] Configuring Windows settings"

# Helper class for MMCSS activation
Add-Type -Namespace mmcss -Name EnableMMCSS -MemberDefinition @'
  [DllImport("avrt.dll", CharSet = CharSet.Unicode)]
  public static extern IntPtr AvSetMmThreadCharacteristics(string TaskName, out uint TaskIndex);
  public static IntPtr EnableMMCSS(string profile) {
    uint idx; return AvSetMmThreadCharacteristics(profile, out idx);
  }
'@

# Set High Performance power plan and attempt to disable core parking
$scheme = powercfg -getactivescheme | Select-String -Pattern '(?<=\:)\s*(.*)' | ForEach-Object { $_.Matches[0].Value.Trim() }
if ($scheme) {
  powercfg -setacvalueindex $scheme SUB_PROCESSOR PROCTHROTTLEMIN 100 2>$null
  powercfg -setacvalueindex $scheme SUB_PROCESSOR PROCTHROTTLEMAX 100 2>$null
  powercfg -setacvalueindex $scheme SUB_PROCESSOR IDLEDISABLE 1 2>$null
  powercfg -setactive $scheme 2>$null
  Write-Host "[rt_harden] High performance power profile enforced"
}

# Request MMCSS priority for current PowerShell process
try {
  $null = mmcss::EnableMMCSS("Pro Audio")
  Write-Host "[rt_harden] MMCSS Pro Audio class enabled"
} catch {
  Write-Host "[rt_harden] MMCSS not available"
}

# Reduce system timer to 0.5 ms if possible
try {
  [void][System.Runtime.InteropServices.DllImport("winmm.dll")]public static extern uint timeBeginPeriod(uint uPeriod); $null = timeBeginPeriod 1
  Write-Host "[rt_harden] timeBeginPeriod(1) called"
} catch {
  Write-Host "[rt_harden] timeBeginPeriod not permitted"
}

Write-Host "[rt_harden] Windows real-time hardening complete"
