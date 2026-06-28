# RetroESP32 S-Video — ESP32 composite-video retro console

OTA firmware for an ESP32 (4MB flash, 8MB PSRAM) retro handheld that outputs **composite/S-video**
(NTSC) and reads ROMs from SD. The system is split into three self-contained ESP-IDF **v3.3.1**
(legacy GNU Make) apps, one per flash partition. The launcher (factory app) hands off to an OTA
slot via `odroid_system_application_set()` + reboot, chosen by the ROM's extension.

**Supported systems:** NES, Game Boy, Game Boy Color, Sega Master System, Sega Game Gear, Atari 800.
(Atari 2600 is integrated but currently disabled — see below.)

| App        | Folder      | Partition          | Systems / cores                                   |
|------------|-------------|--------------------|---------------------------------------------------|
| Launcher   | `launcher/` | factory @ 0x10000  | Carousel UI + NES (nofrendo) + SMS/GG (smsplus)   |
| Atari      | `atari/`    | ota_0 @ 0x1C0000   | Atari 800 (`atari800`). 2600/Stella code is present but **excluded** from the build (see below) |
| Game Boy   | `gb/`       | ota_1 @ 0x2E0000   | Game Boy **and Game Boy Color** (gnuboy)          |

Each app is fully self-contained — its own `components/` (incl. a per-app `odroid` HAL with
`video_out.h`), `Makefile`, `sdkconfig`, and `partitions.csv`. There are intentionally **no shared
components**: the `odroid`/`video_out.h` copies differ slightly per app (e.g. Atari uses 384-wide
composite geometry, GB/SMS use 256-wide).

## Build (Windows + msys64 MINGW32)

```bash
cd atari            # or launcher / gb
MSYSTEM=MINGW32 /c/msys64/usr/bin/bash -l build_idf.sh -j4 all
```

`build_idf.sh` is self-locating (works from any path) and sets `IDF_PATH` (override via the
environment). Output binary: `build/<app>.bin`.

## Flash (native Windows esptool, per partition offset)

```
python <idf>/components/esptool_py/esptool/esptool.py --chip esp32 --port //./COM6 \
  --baud 921600 --before default_reset --after hard_reset write_flash -z \
  --flash_mode dio --flash_freq 40m --flash_size detect \
  0x1C0000 atari/build/RetroESP32_Atari.bin     # ota_0; launcher=0x10000, gb=0x2E0000
```

## ⚠️ Build optimization — important

Use **RELEASE** globally (`CONFIG_OPTIMIZATION_LEVEL_RELEASE=y`, `-Os`) with **`-O2` forced** on the
timing-critical paths via `component.mk`: `main` (composite ISR + blit) and `odroid` (audio). The
6502 cores (`atari800`, `gnuboy`) also carry their own `-O2`. **Do NOT ship DEBUG (`-Og`)** — it
leaves the composite video/audio path unoptimized and the result is visibly choppy.

> **Lesson learned the hard way:** Game Boy Color was once *dropped* after a ~40-cycle investigation
> concluded its whole-screen shake was unfixable hardware memory-bus contention. It wasn't — it was
> the DEBUG build. At RELEASE/`-O2` the tighter code cuts core-0's SRAM traffic enough to stop
> starving the composite DMA, and CGB runs smooth and full-speed. Always confirm RELEASE before
> blaming the hardware.

## Atari 2600 (Stella) — deferred

The `atari/components/stella` core is integrated but `EXCLUDE_COMPONENTS := stella` in
`atari/Makefile` keeps it out of the build. Full-speed 2600 was not reachable on the 4MB ESP32
(the composite ISR claims the IRAM Stella's CPU needs). To be revisited on a 16MB ESP32 with more
IRAM headroom — though, like the GBC shake above, that "too slow" verdict was measured under DEBUG,
so it's worth a RELEASE re-test first.

## Future direction (ESP32-S3 "v2")

A design note for a more capable revision — composite via an external **R-2R DAC**, **native-USB**
gamepads (dropping the CH559), and ~4× the IRAM headroom for easy multi-emulator — is in
[`docs/FUTURE_ESP32-S3.md`](docs/FUTURE_ESP32-S3.md).

## License

This project is licensed under the **GNU General Public License v2** ([`LICENSE`](LICENSE)) — it
bundles GPLv2 emulator cores (smsplus, gnuboy, atari800, stella), LGPLv2 (nofrendo), and the ISC
composite generator (ESP_8_BIT). Per-component authors and licenses are listed in
[`CREDITS.md`](CREDITS.md). No ROMs or BIOS files are included.
