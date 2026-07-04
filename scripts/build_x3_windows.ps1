param(
    [ValidateSet("default", "gh_release", "gh_release_rc", "slim")]
    [string]$Environment = "default",
    [int]$Jobs = 4
)

$ErrorActionPreference = "Stop"

Set-StrictMode -Version Latest
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root

function Find-PlatformIO {
    $penvPio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
    if (Test-Path $penvPio) {
        return $penvPio
    }

    $fromPath = Get-Command pio -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    throw "pio not found. Install PlatformIO first."
}

$pio = Find-PlatformIO
Write-Host "Building $Environment firmware with $pio..."
& $pio run -e $Environment -j $Jobs
