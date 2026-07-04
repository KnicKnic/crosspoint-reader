param(
    [ValidateSet("default", "gh_release", "gh_release_rc", "slim")]
    [string]$Environment = "default",
    [string]$Firmware = "",
    [string]$Port = "",
    [int]$Baud = 921600,
    [Alias("StartSerial", "Serial")]
    [switch]$Monitor,
    [int]$MonitorBaud = 115200,
    [switch]$Build,
    [switch]$SkipVerify,
    [switch]$SkipFlash
)

$ErrorActionPreference = "Stop"

Set-StrictMode -Version Latest
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root

$app0Offset = "0x10000"
$app1Offset = "0x650000"
$appPartitionSize = 0x640000

function Find-Esptool {
    $candidates = @(
        (Join-Path $env:USERPROFILE ".platformio\penv\Scripts\esptool.exe"),
        (Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $fromPath = Get-Command esptool.exe -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    throw "esptool not found. Build once with PlatformIO or install PlatformIO first."
}

function Test-PythonCandidate {
    param(
        [string]$Exe,
        [string[]]$Args = @()
    )

    try {
        & $Exe @Args -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 8) else 1)" *> $null
        return $LASTEXITCODE -eq 0
    } catch {
        return $false
    }
}

function Find-Python {
    $pyLauncher = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($pyLauncher -and (Test-PythonCandidate $pyLauncher.Source @("-3"))) {
        return [pscustomobject]@{
            Exe = $pyLauncher.Source
            Args = @("-3")
        }
    }

    $commands = @("python3.exe", "python.exe")
    foreach ($command in $commands) {
        $fromPath = Get-Command $command -All -ErrorAction SilentlyContinue
        foreach ($candidate in $fromPath) {
            if (Test-PythonCandidate $candidate.Source) {
                return [pscustomobject]@{
                    Exe = $candidate.Source
                    Args = @()
                }
            }
        }
    }

    $penvPython = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
    if ((Test-Path $penvPython) -and (Test-PythonCandidate $penvPython)) {
        return [pscustomobject]@{
            Exe = $penvPython
            Args = @()
        }
    }

    throw "Python 3.8 or newer not found. Install Python 3 or fix PATH so the debugging monitor can run."
}

function Find-X3Port {
    $ports = Get-CimInstance Win32_SerialPort |
        Where-Object {
            $_.PNPDeviceID -match "VID_303A" -or
            $_.Name -match "ESP32|USB Serial|USB JTAG" -or
            $_.Description -match "ESP32|USB Serial|USB JTAG"
        } |
        Sort-Object DeviceID

    if (-not $ports) {
        throw "No likely ESP32-C3/X3 serial port found. Put the X3 in download mode and retry."
    }

    if ($ports.Count -gt 1) {
        Write-Host "Multiple candidate ports found:"
        $ports | ForEach-Object { Write-Host "  $($_.DeviceID)  $($_.Name)" }
        throw "Pass -Port COMx to choose the X3."
    }

    return $ports[0].DeviceID
}

if ($Build) {
    & (Join-Path $PSScriptRoot "build_x3_windows.ps1") -Environment $Environment
}

if (($Monitor -or -not $SkipFlash) -and [string]::IsNullOrWhiteSpace($Port)) {
    $Port = Find-X3Port
}

if ($SkipFlash) {
    Write-Host ""
    Write-Host "Skipping flash step."
} else {
    if ([string]::IsNullOrWhiteSpace($Firmware)) {
        $Firmware = ".pio\build\$Environment\firmware.bin"
    }

    $firmwarePath = Resolve-Path $Firmware
    $firmwareInfo = Get-Item $firmwarePath
    if ($firmwareInfo.Length -gt $appPartitionSize) {
        throw ("Firmware is too large: 0x{0:x} > 0x{1:x}" -f $firmwareInfo.Length, $appPartitionSize)
    }

    $esptool = Find-Esptool
    $sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $firmwarePath).Hash.ToLowerInvariant()

    Write-Host "Firmware: $firmwarePath"
    Write-Host ("Size:     0x{0:x} bytes" -f $firmwareInfo.Length)
    Write-Host "SHA-256:  $sha256"
    Write-Host "Port:     $Port"
    Write-Host "Baud:     $Baud"
    Write-Host "esptool:  $esptool"
    Write-Host ""

    & $esptool --chip esp32c3 -p $Port chip-id

    Write-Host ""
    Write-Host "Writing both OTA app slots..."
    & $esptool --chip esp32c3 -p $Port -b $Baud write-flash `
        $app0Offset $firmwarePath `
        $app1Offset $firmwarePath

    if (-not $SkipVerify) {
        Write-Host ""
        Write-Host "Verifying both OTA app slots..."
        & $esptool --chip esp32c3 -p $Port verify-flash $app0Offset $firmwarePath
        & $esptool --chip esp32c3 -p $Port verify-flash $app1Offset $firmwarePath
    }

    clear-Host
    Write-Host ""
    Write-Host "Flashed X3 firmware to app0 and app1 successfully."
    [console]::Beep()
}

if ($Monitor) {
    $python = Find-Python
    $monitorScript = Join-Path $PSScriptRoot "debugging_monitor.py"
    $pythonArgs = @()
    if ($python.Args) {
        $pythonArgs += $python.Args
    }
    $monitorArgs = @($monitorScript, $Port, "--baud", $MonitorBaud)

    Write-Host ""
    Write-Host "Opening debugging monitor on $Port at $MonitorBaud baud with $($python.Exe)..."
    Write-Host "Press Ctrl+C or close the graph window to close the monitor."
    & $python.Exe @pythonArgs @monitorArgs
}
