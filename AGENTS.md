# Agent Instructions

- On Windows, build firmware with `.\scripts\build_x3_windows.ps1 -Environment default`.
- Use the build script instead of invoking bare `pio`; it sets UTF-8 output and prefers PlatformIO's own virtualenv executable.
- To build and flash on Windows, use `.\scripts\flash_x3_windows.ps1 -Build -Environment default -Port COMx`.
