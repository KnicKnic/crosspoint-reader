"""
PlatformIO pre-build script: keep FreeRTOS tickless sections in flash.text.

The Arduino-IDF prebuilt ESP32-C3 linker script excludes libfreertos.a from the
catch-all flash text rule and whitelists individual FreeRTOS task functions.
When tickless idle is enabled, two task sections can otherwise become orphan
output sections after .flash.text, which can make esptool reject firmware.bin.
"""

from pathlib import Path

Import("env")  # noqa: F821 (SCons-injected global)


PATCH_LINES = [
    "    *libfreertos.a:tasks.*(.literal.prvGetExpectedIdleTime .text.prvGetExpectedIdleTime)",
    "    *libfreertos.a:tasks.*(.literal.vTaskStepTick .text.vTaskStepTick)",
]


def patch_freertos_tickless_linker():
    package_dir = env.PioPlatform().get_package_dir(  # noqa: F821
        "framework-arduinoespressif32-libs"
    )
    if not package_dir:
        return

    sections_path = Path(package_dir) / "esp32c3" / "ld" / "sections.ld"
    if not sections_path.exists():
        return

    text = sections_path.read_text(encoding="utf-8")
    if ".text.prvGetExpectedIdleTime" in text and ".text.vTaskStepTick" in text:
        return

    marker = "    *libfreertos.a:tasks.*(.literal.xTaskGetNext .text.xTaskGetNext)\n"
    if marker not in text:
        raise RuntimeError(
            "ESP32-C3 sections.ld did not contain expected FreeRTOS tasks marker"
        )

    replacement = marker + "\n".join(PATCH_LINES) + "\n"
    sections_path.write_text(text.replace(marker, replacement, 1), encoding="utf-8")
    print("Patched ESP32-C3 FreeRTOS tickless linker sections")


patch_freertos_tickless_linker()
