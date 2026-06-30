"""Post-build: combineer bootloader + partitions + boot_app0 + app tot één
   `firmware-merged.bin` op flash-offset 0x0, geschikt voor eerste flash via
   ESP Web Tools (één part per chipFamily in manifest.json).

   ESP32 (V3):  bootloader op 0x1000, chip-arg "esp32",  flash_mode "dio"
   ESP32-C6 (V4): bootloader op 0x0,    chip-arg "esp32c6", flash_mode "dio"

   N.B. V4 stond eerst op flash_mode "qio" @ default-freq, maar dat gaf een
   boot-loop: de ROM laadde de 2nd-stage bootloader (`load:...0x40875730` /
   `ets_loader.c 67`) en resette dan direct via de RTC-watchdog
   (`LP_WDT_SYS` / `TG0_WDT_HPSYS`) zonder ook maar één bootloader-regel te
   printen. Veel C6-modules lezen hun flash betrouwbaarder in DIO; daarom
   forceren we DIO @ 40MHz voor C6.
"""
import os

Import("env")  # noqa: F821 — door PlatformIO geinjecteerd

def _merge(*_args, **_kwargs):
    platform = env.PioPlatform()  # noqa: F821
    fw_dir = platform.get_package_dir("framework-arduinoespressif32")
    esptool_dir = platform.get_package_dir("tool-esptoolpy")
    boot_app0 = os.path.join(fw_dir, "tools", "partitions", "boot_app0.bin")
    build_dir = env.subst("$BUILD_DIR")  # noqa: F821
    out = os.path.join(build_dir, "firmware-merged.bin")

    board_mcu = env.get("BOARD_MCU", "esp32")
    is_c6 = "c6" in board_mcu.lower()
    chip_arg     = "esp32c6" if is_c6 else "esp32"
    flash_mode   = "dio"  # zowel V3 als V4 op DIO (V4 was qio → boot-loop)
    boot_offset  = "0x0"    if is_c6 else "0x1000"

    cmd = [
        env.subst("$PYTHONEXE"),  # noqa: F821
        os.path.join(esptool_dir, "esptool.py"),
        "--chip", chip_arg,
        "merge_bin",
        "-o", out,
        "--flash_mode", flash_mode,
    ]
    # Forceer conservatieve flash-freq op C6 om de boot-loop uit te sluiten;
    # V3 ongewijzigd (neemt de freq uit de bootloader-header over).
    if is_c6:
        cmd += ["--flash_freq", "40m"]
    cmd += [
        "--flash_size", "4MB",
        boot_offset, os.path.join(build_dir, "bootloader.bin"),
        "0x8000",    os.path.join(build_dir, "partitions.bin"),
        "0xe000",    boot_app0,
        "0x10000",   os.path.join(build_dir, "firmware.bin"),
    ]
    env.Execute(" ".join(cmd))  # noqa: F821

env.AddPostAction("$BUILD_DIR/firmware.bin", _merge)  # noqa: F821
