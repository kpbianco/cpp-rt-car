param(
    [Parameter(Mandatory = $true)][string]$EngineRoot,
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [ValidateSet("Development", "Shipping")][string]$Configuration = "Development",
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"
if (-not $IsLinux) {
    throw "M19-02 is approved only for the Debian 12 x86_64 Unreal 5.8.1 tuple; use verify-unreal.sh on that runner."
}

$Arguments = @(
    "scripts/verify-unreal.sh",
    "--engine-root", $EngineRoot,
    "--sdk-root", $SdkRoot,
    "--configuration", $Configuration
)
if ($BuildOnly) {
    $Arguments += "--build-only"
}
& bash @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "verify-unreal.sh failed with exit code $LASTEXITCODE"
}
