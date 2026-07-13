from pathlib import Path

Import("env")


MISSING_TASK_SECTIONS = (
    ".text.prvGetExpectedIdleTime",
    ".text.vTaskStepTick",
)


def patch_sections_ld() -> None:
    platform = env.PioPlatform()
    package_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
    if not package_dir:
        raise RuntimeError("framework-arduinoespressif32-libs package not found")

    mcu = env.BoardConfig().get("build.mcu", "")
    sections_path = Path(package_dir) / mcu / "ld" / "sections.ld"
    if not sections_path.is_file():
        raise RuntimeError(f"FreeRTOS linker script not found: {sections_path}")

    text = sections_path.read_text()
    if all(section in text for section in MISSING_TASK_SECTIONS):
        return

    old = ".text.prvAddNewTaskToReadyList .text.prvDeleteTCB"
    new = (
        ".text.prvAddNewTaskToReadyList "
        ".text.prvGetExpectedIdleTime "
        ".text.prvDeleteTCB"
    )
    text = text.replace(old, new, 1)

    old = ".text.vTaskMissedYield .text.vTaskPlaceOnEventList"
    new = (
        ".text.vTaskMissedYield "
        ".text.vTaskStepTick "
        ".text.vTaskPlaceOnEventList"
    )
    text = text.replace(old, new, 1)

    if not all(section in text for section in MISSING_TASK_SECTIONS):
        raise RuntimeError(
            "Could not patch FreeRTOS tickless idle sections into sections.ld"
        )

    sections_path.write_text(text)
    print(f"Patched FreeRTOS tickless linker sections: {sections_path}")


patch_sections_ld()
