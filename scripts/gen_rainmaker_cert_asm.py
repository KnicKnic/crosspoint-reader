from pathlib import Path
import subprocess

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
BUILD_DIR = Path(env.subst("$BUILD_DIR"))

CERTS = (
    "rmaker_mqtt_server.crt",
    "rmaker_claim_service_server.crt",
    "rmaker_ota_server.crt",
)

PLACEHOLDER_CERT = (
    "CrossPoint does not use ESP RainMaker; this placeholder only satisfies "
    "unused framework build symbols.\n"
)


def package_path(package: str) -> Path:
    path = env.PioPlatform().get_package_dir(package)
    if not path:
        raise RuntimeError(f"PlatformIO package is not installed: {package}")
    return Path(path)


def cmake_executable() -> Path | str:
    exe = "cmake.exe" if env["PLATFORM"] == "win32" else "cmake"
    bundled = package_path("tool-cmake") / "bin" / exe
    if bundled.exists():
        return bundled
    return env.WhereIs("cmake") or "cmake"


def cert_source(filename: str) -> Path:
    root_managed = (
        PROJECT_DIR
        / "managed_components"
        / "espressif__esp_rainmaker"
        / "server_certs"
        / filename
    )
    if root_managed.exists():
        return root_managed

    build_managed = (
        BUILD_DIR
        / "managed_components"
        / "espressif__esp_rainmaker"
        / "server_certs"
        / filename
    )
    if build_managed.exists():
        return build_managed

    placeholder = BUILD_DIR / f"{filename}.placeholder"
    placeholder.parent.mkdir(parents=True, exist_ok=True)
    write_text_if_changed(placeholder, PLACEHOLDER_CERT)
    return placeholder


def write_text_if_changed(path: Path, content: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def replace_if_changed(source: Path, target: Path) -> bool:
    source_bytes = source.read_bytes()
    if target.exists() and target.read_bytes() == source_bytes:
        source.unlink()
        return False
    source.replace(target)
    return True


embed_script = (
    package_path("framework-espidf")
    / "tools"
    / "cmake"
    / "scripts"
    / "data_file_embed_asm.cmake"
)
cmake = cmake_executable()

for cert in CERTS:
    source = cert_source(cert)
    generated = BUILD_DIR / f"{cert}.S"
    generated_tmp = BUILD_DIR / f"{cert}.S.tmp"
    generated.parent.mkdir(parents=True, exist_ok=True)

    subprocess.run(
        [
            str(cmake),
            f"-DDATA_FILE={source}",
            f"-DSOURCE_FILE={generated_tmp}",
            f"-DVARIABLE_BASENAME={cert}",
            "-DFILE_TYPE=TEXT",
            "-P",
            str(embed_script),
        ],
        check=True,
    )
    changed = replace_if_changed(generated_tmp, generated)
    print(f"{'Generated' if changed else 'Unchanged'} RainMaker cert assembly: {generated}")
