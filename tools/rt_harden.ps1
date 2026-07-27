# Apply best-effort real-time settings for CI environments on Windows runners.
Write-Host "[rt_harden] Configuring Windows settings"

# Native binding for best-effort MMCSS activation on this PowerShell thread.
$mmcssBindingAvailable = $false
try {
  Add-Type -Namespace Rtfw -Name MmcssNative -ErrorAction Stop -MemberDefinition @'
    [DllImport("avrt.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr AvSetMmThreadCharacteristics(string taskName, out uint taskIndex);
'@
  $mmcssBindingAvailable = $true
} catch {
  Write-Host "[rt_harden] MMCSS binding unavailable"
}

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
if ($mmcssBindingAvailable) {
  try {
    [uint32]$taskIndex = 0
    $mmcssHandle = [Rtfw.MmcssNative]::AvSetMmThreadCharacteristics(
      "Pro Audio",
      [ref]$taskIndex)
    if ($mmcssHandle -eq [IntPtr]::Zero) {
      throw "AvSetMmThreadCharacteristics failed"
    }
    Write-Host "[rt_harden] MMCSS Pro Audio class enabled"
  } catch {
    Write-Host "[rt_harden] MMCSS not available"
  }
}

# Reduce system timer to 0.5 ms if possible
try {
  Add-Type -Namespace winmm -Name Native -MemberDefinition @'
    [DllImport("winmm.dll")]
    public static extern uint timeBeginPeriod(uint uPeriod);
'@
  $null = [winmm.Native]::timeBeginPeriod(1)
  Write-Host "[rt_harden] timeBeginPeriod(1) called"
} catch {
  Write-Host "[rt_harden] timeBeginPeriod not permitted"
}

Write-Host "[rt_harden] Windows real-time hardening complete"
