# X3 Laptop Companion Host

.NET 10 WPF tray app for the X3 BLE Laptop Companion.

## Requirements

- Windows 10 19041 or newer.
- .NET 10 SDK.
- Bluetooth LE adapter.

## Current Status

- Scans for the X3 companion GATT service.
- Subscribes to device-command notifications.
- Sends best-effort Teams presence status to the X3.
- Handles `ToggleMute` by focusing Teams when possible and sending `Ctrl+Shift+M`.
- Includes a test mode that keeps BLE active and sends simulated Teams,
  microphone, camera, and status-message values to the X3.
- Includes a Teams dry-run mode that keeps BLE active and acknowledges X3 mute
  commands without focusing or controlling Teams.
- Writes a diagnostic log to `%LOCALAPPDATA%\X3LaptopCompanion\host.log`.
  Use the `Open Log` button or tray menu item to jump to it.

Microphone and camera state detection are still placeholders and currently report
`Unknown`.

## Build

```powershell
dotnet build .\X3LaptopCompanion.csproj
```
