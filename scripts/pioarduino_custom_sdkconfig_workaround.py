from pathlib import Path

Import("env")


EMBEDDED_TEXT_STEMS = (
    "https_server.crt",
    "https_server.key",
    "rmaker_mqtt_server.crt",
    "rmaker_claim_service_server.crt",
    "rmaker_ota_server.crt",
)


def write_embedded_text_stub(build_dir, stem):
    symbol = stem.replace(".", "_")
    asm_path = build_dir / f"{stem}.S"

    if asm_path.exists():
        return

    asm_path.write_text(
        "\n".join(
            [
                ".section .rodata.embedded",
                f".global _binary_{symbol}_start",
                f"_binary_{symbol}_start:",
                ".byte 0",
                f".global _binary_{symbol}_end",
                f"_binary_{symbol}_end:",
                f".global _binary_{symbol}_size",
                f".set _binary_{symbol}_size, _binary_{symbol}_end - _binary_{symbol}_start",
                "",
            ]
        ),
        encoding="utf-8",
    )


if env.GetProjectOption("custom_sdkconfig", ""):
    build_dir = Path(env.subst("$BUILD_DIR"))
    build_dir.mkdir(parents=True, exist_ok=True)

    for stem in EMBEDDED_TEXT_STEMS:
        write_embedded_text_stub(build_dir, stem)
