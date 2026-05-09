"""Post-build: combineer bootloader + partitions + boot_app0 + app tot één
   `firmware-merged.bin` op flash-offset 0x0, geschikt voor eerste flash via
   ESP Web Tools (één part per chipFamily in manifest.json)."""
import os

Import("env")  # noqa: F821 — door PlatformIO geinjecteerd

def _merge(*_args, **_kwargs):
    platform = env.PioPlatform()  # noqa: F821
    fw_dir = platform.get_package_dir("framework-arduinoespressif32")
    esptool_dir = platform.get_package_dir("tool-esptoolpy")
    boot_app0 = os.path.join(fw_dir, "tools", "partitions", "boot_app0.bin")
    build_dir = env.subst("$BUILD_DIR")  # noqa: F821
    out = os.path.join(build_dir, "firmware-merged.bin")
    env.Execute(  # noqa: F821
        " ".join([
            env.subst("$PYTHONEXE"),  # noqa: F821
            os.path.join(esptool_dir, "esptool.py"),
            "--chip", "esp32",
            "merge_bin",
            "-o", out,
            "--flash_mode", "dio",
            "--flash_size", "4MB",
            "0x1000",  os.path.join(build_dir, "bootloader.bin"),
            "0x8000",  os.path.join(build_dir, "partitions.bin"),
            "0xe000",  boot_app0,
            "0x10000", os.path.join(build_dir, "firmware.bin"),
        ])
    )

env.AddPostAction("$BUILD_DIR/firmware.bin", _merge)  # noqa: F821
