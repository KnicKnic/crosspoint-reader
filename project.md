# X3 BLE Laptop Companion

## Goal

Build an XTEink X3 companion mode for a Windows work laptop over BLE 5.

The first milestone is a dedicated Microsoft Teams mute companion. The second
milestone is BLE-only sync for small notes, Markdown, text, and simple CSV chart
payloads.

This project has two code areas:

- `laptop_host/`: new .NET Windows host app.
- Existing CrossPoint-derived X3 firmware: add the companion BLE service,
  companion UI, button handling, reconnect behavior, and low-power behavior.

## Target Environment

- Host: Windows laptop.
- Host app stack: .NET 10 WPF tray app.
- Teams target: the current Microsoft Teams desktop client installed on the
  laptop.
- Transport: BLE only for both control/status and notes.
- Existing firmware base: CrossPoint X3 BLE page-turner fork.

## Milestone 1: Teams Mute Companion

### User Experience

The X3 has a Teams companion screen showing:

- BLE host connection state.
- Current microphone state: muted or unmuted.
- Current camera activity state: active or inactive when detectable.
- Last sync/error state from the host.

When the state changes, the X3 should update only the changed display region
using a partial e-paper refresh where practical.

### Button Behavior

Initial mapping while the Teams companion screen is active:

- `Confirm`: toggle Teams mute.
- `Back`: leave the companion screen.
- `Left` / `Right`: reserved for future panels such as notes or details.
- Side page buttons: reserved for future navigation.
- Power: keep existing system behavior.

The `Confirm` choice fits the current CrossPoint input model and is unlikely to
conflict with page navigation. A later settings item can make the mute action
remappable to a side/page button if real use shows that is more comfortable.

### Host Responsibilities

The .NET host app should:

- Run in the background, ideally as a tray app for v1.
- Auto-connect/reconnect to the paired X3.
- Detect Teams mute state when supported.
- Detect camera activity when supported.
- Send state updates to the X3 over BLE.
- Receive a mute-toggle command from the X3.
- Toggle mute using any supported Windows or Teams mechanism.
- Prefer supported APIs, but use automation or shortcut fallback if that is the
  only reliable option for the installed Teams client.

The practical fallback for mute toggle can be `Ctrl+Shift+M` or the configured
Teams mute shortcut if API-level control is not available. The host app should
report whether state detection/control is authoritative or best-effort.

### Firmware Responsibilities

The X3 firmware should:

- Add a laptop companion mode separate from the existing BLE page-turner remote
  support.
- Make the companion mode reachable from the normal Home UI.
- Advertise or expose a custom BLE GATT service for the Windows host.
- Render the Teams companion status screen.
- Send button commands to the host.
- Receive host status messages and refresh only changed regions when possible.
- Preserve low-power behavior and reconnect after sleep/wake.
- Avoid destabilizing the validated BLE page-turner reconnect path.

## BLE Protocol Sketch

Use a custom GATT service with small structured messages.

Suggested characteristics:

- Host status notify/write: host sends connection, mute, camera, and error
  state to X3.
- Device command notify/read: X3 sends user commands such as toggle mute.
- Device info read: firmware version, protocol version, battery/state summary.
- Notes transfer write/notify: reserved for milestone 2 chunked payloads.

Initial UUID assignments:

- Service: `7d2d5f00-778d-4df6-a6d5-7c4e7a000001`
- Host status: `7d2d5f00-778d-4df6-a6d5-7c4e7a000002`
- Device command: `7d2d5f00-778d-4df6-a6d5-7c4e7a000003`
- Device info: `7d2d5f00-778d-4df6-a6d5-7c4e7a000004`
- Notes transfer: `7d2d5f00-778d-4df6-a6d5-7c4e7a000005`

Initial message types:

- `host_status`: mute state, camera state, Teams detected, timestamp/sequence.
- `device_command`: toggle mute.
- `ack`: sequence acknowledgement and status.
- `error`: concise error code and optional short message.

Protocol constraints:

- Keep packets small enough for conservative BLE MTU behavior.
- Include sequence numbers for reconnect recovery.
- Treat host state as source of truth for Teams mute/camera display.
- After reconnect, host should resend the full current state.

## Milestone 2: Notes and Charts

Supported input assumptions:

- `.txt` notes.
- Markdown notes.
- CSV with a declared chart type.

BLE-only sync means payloads should be small and chunked. Large documents,
images, or full PDF sync are out of scope until BLE throughput and power impact
are measured.

Possible CSV chart metadata:

```text
chart_type=line
x=Date
y=Value
```

Rendering target:

- Text/Markdown: simple readable note viewer on X3.
- CSV: basic line/bar chart rendering from host-prepared or firmware-parsed
  data, depending on complexity.

## Power and Reconnect Requirements

- The Windows host should auto-reconnect after laptop sleep, app restart, and
  X3 sleep/wake.
- The X3 should remain low-power when idle.
- While the Teams companion screen is connected, the X3 should avoid deep sleep
  so BLE status and button control remain available, but it should still enter
  the existing low-power idle mode when nothing is changing.
- BLE work should avoid keeping the X3 awake indefinitely.
- Reconnect behavior should be bounded and observable through diagnostics.
- Existing page-turner BLE behavior must remain stable unless companion mode is
  explicitly enabled.
- Implementation note: current `Activity::preventAutoSleep()` resets the main
  loop inactivity timer and restores normal CPU speed. The companion mode should
  use or add a more specific deep-sleep suppression path so it can block deep
  sleep without preventing the 10 MHz idle CPU mode.

## Implementation Plan

1. Define custom BLE service IDs, message schema, sequence/ack behavior, and
   reconnect rules.
2. Scaffold `laptop_host/` as a .NET 10 WPF tray app.
3. Build a minimal host/X3 BLE handshake with connection state shown on the X3.
4. Add X3 Teams companion activity, Home UI entry point, and `Confirm` button
   command.
5. Implement host mute-toggle path for the installed Teams desktop client.
6. Add best-effort host mute/camera state detection and status updates.
7. Add partial refresh regions for mute/camera changes.
8. Add deep-sleep suppression for the active companion screen while preserving
   low-power idle CPU mode.
9. Validate reconnect, sleep/wake, and low-power behavior.
10. Start milestone 2 with small BLE chunk transfer and a text note viewer.

## Open Decisions

- Whether the X3 should act as BLE peripheral, central, or support both modes.
  Peripheral is preferred for the companion service if it can coexist cleanly
  with existing BLE code.
- Exact Teams mute/camera detection method for the installed Teams client.
- Whether notes rendering should be mostly host-prepared bitmaps/layout data or
  firmware-rendered text/CSV.

## Near-Term Validation

- Add `Laptop Companion` to the Home menu before `Settings`.
- Confirm the X3 can expose the custom service and connect from Windows.
- Confirm Teams mute toggle works reliably from the host app.
- Measure BLE reconnect behavior after Windows sleep and X3 sleep.
- Measure current draw or battery drain while the companion screen is connected
  but idle, confirming low-power CPU mode is still reached.
- Measure partial refresh readability and latency for mute/camera state changes.
- Confirm existing Free2/Free3 page-turner behavior still passes current audits.
