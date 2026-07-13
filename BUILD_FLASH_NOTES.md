# Build and Flash Notes

These commands assume PowerShell from the repository root:

```powershell
cd C:\code\crosspoint-freeink
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONUTF8 = '1'
```

Use PlatformIO's virtualenv executable on this machine. The global `pio` command
may use a different Python version.

## Build

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e default
```

The first build after changing `custom_sdkconfig` can take a long time because
pioarduino rebuilds Arduino/ESP-IDF libraries with the project sdkconfig.
It writes local cache markers such as `sdkconfig.defaults`, `sdkconfig.default`,
`dependencies.lock`, `CMakeLists.txt`, `.dummy/`, and a package-level
`framework-arduinoespressif32-libs\sdkconfig`. Do not delete these unless you
want to force that slow framework rebuild.

The default environment enables PM, DFS, tickless idle, PM profiling, ESP timer
profiling, and FreeRTOS run-time stats. CrossPoint requests a 160 MHz max /
10 MHz min DFS policy for ESP32-C3.

System settings include:

- `Auto Light Sleep`: off by default. Turning it on flushes and stops USB serial
  logging before enabling automatic light sleep. Turning it off restarts serial.
- `Power Stats`: displays current CPU/DFS state, top PM locks, top tasks, and
  timer details. The same profiler dump is also emitted over serial every 60
  seconds while serial logging is active.

If a warm `platformio` command is unexpectedly slow, check whether the log says
`*** Compile Arduino IDF libs for default ***`. If it does, one of those cache
markers was removed or the `custom_sdkconfig` hash changed. Also check for
overlapping PlatformIO/Python processes from the IDE and use PlatformIO's
virtualenv command shown above. The global `platformio` on this machine can pick
up Python 3.13 and fail with a `littlefs` import error.

## Flash

Connect the board on `COM3`, then run:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e default -t upload --upload-port COM3
```

PlatformIO rebuilds anything stale before uploading. The firmware artifact is:

```text
.pio\build\default\firmware.bin
```

Even when nothing is stale, the PlatformIO upload target still scans the build
graph before flashing. Use the fast esptool command below when you only need to
flash the already-built image.

## Fast Reflash Without Rebuild

If the firmware has already been built and you only want to reflash the same
artifacts, skip PlatformIO's build graph and call esptool directly:

```powershell
$python = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$bootapp = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"

& $python $esptool --chip esp32c3 --port COM3 --baud 921600 `
  --before default-reset --after hard-reset write-flash -z `
  --flash-mode dio --flash-freq 80m --flash-size 16MB `
  0x0 .pio\build\default\bootloader.bin `
  0x8000 .pio\build\default\partitions.bin `
  0xe000 $bootapp `
  0x10000 .pio\build\default\firmware.bin
```

Run a normal PlatformIO build again before using this command after any source,
library, partition, or sdkconfig change.

## Useful Checks

Check the active serial port:

```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Name,Description
```

Clean only the default build output if a rebuild gets wedged:

```powershell
Remove-Item .pio\build\default -Recurse -Force
```

If Windows reports path-length issues, enable Win32 long paths and restart:

```powershell
New-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' `
  -Name 'LongPathsEnabled' -Value 1 -PropertyType DWORD -Force
```
