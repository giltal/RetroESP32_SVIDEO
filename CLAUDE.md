# RetroESP32 S-Video — Project Context

## What This Is

OTA firmware for an **ESP32** (4MB flash, 8MB PSRAM) retro-gaming handheld that outputs **composite /
S-video (NTSC)** instead of an LCD, and reads ROMs from SD. It is a composite-video port/fork of the
ODROID-GO–style **RetroESP32** (upstream reference lives at `../RetroESP32-master/`).

The system is **three separate ESP-IDF v3.3.1 (legacy GNU Make) apps**, one per flash partition. The
launcher is the factory app; it hands off to an OTA slot via `odroid_system_application_set()` +
reboot, selected by the carousel system / ROM extension.

## Repository Structure

```
RetroESP32_SVIDEO/            <- git repo root
├─ launcher/   factory @0x10000  — carousel UI + NES (nofrendo) + SMS/GG (smsplus), in-process
├─ atari/      ota_0   @0x1C0000 — Atari 800 (atari800). 2600/Stella present but EXCLUDED
├─ gb/         ota_1   @0x2E0000 — Game Boy / Color (gnuboy)
├─ README.md
├─ .gitignore  (build/, *.old, mon*.py, __pycache__)
└─ CLAUDE.md
```

Each app is **fully self-contained**: its own `main/`, `components/`, `Makefile`, `sdkconfig`,
`partitions.csv`. There are **no shared components** on purpose — each app has its own `odroid` HAL
(incl. `video_out.h`), and those copies differ (Atari uses 384-wide composite geometry; GB/SMS use
256-wide). Don't try to dedupe them.

| App | components/ |
|-----|-------------|
| `launcher` | nofrendo, smsplus, odroid |
| `atari` | atari800, odroid, stella *(excluded)* |
| `gb` | gnuboy, odroid |

## Partition Layout (4MB, all three `partitions.csv` must match)

```
nvs,       data, nvs,     0x9000,   0x6000     # settings + composite calibration (preserve!)
phy_init,  data, phy,     0xF000,   0x1000
factory,   app,  factory, 0x10000,  0x1A0000   # launcher + NES/SMS/GG
otadata,   data, ota,     0x1B0000, 0x2000
ota_0,     app,  ota_0,   0x1C0000, 0x120000   # atari  (1.125 MB)
ota_1,     app,  ota_1,   0x2E0000, 0x120000   # gb     (1.125 MB)
```

## Development Environment

- **Host**: Windows. Build runs in **msys64 MINGW32** (`MSYSTEM=MINGW32`).
- **ESP-IDF**: v3.3.1 at `/c/Users/97254/esp/v3.3.1/esp-idf` (override via `$IDF_PATH`).
- **Toolchain**: `xtensa-esp32-elf-` at `/c/Users/97254/esp/toolchains/xtensa-esp32-elf/bin`.
- **Flash**: native Windows `esptool.py` over USB serial (typically **COM6**).

## Build & Flash

`build_idf.sh` (one per app) is **self-locating** — pass it by absolute path; `bash -l` changes cwd
so a relative `./build_idf.sh` will NOT be found.

```bash
# Build (from anywhere)
MSYSTEM=MINGW32 /c/msys64/usr/bin/bash -l /c/ESPIDFprojects/RetroESP32_SVIDEO/atari/build_idf.sh -j4 all
#   -> build/RetroESP32_Atari.bin   (launcher/gb produce their own <name>.bin)
```

```powershell
# Flash (PowerShell, native esptool) — offset per partition above
python C:\Users\97254\esp\v3.3.1\esp-idf\components\esptool_py\esptool\esptool.py `
  --chip esp32 --port //./COM6 --baud 921600 --before default_reset --after hard_reset `
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect `
  0x1C0000 C:\ESPIDFprojects\RetroESP32_SVIDEO\atari\build\RetroESP32_Atari.bin
#   launcher -> 0x10000,  gb -> 0x2E0000
```

## ⚠️ Build Optimization — READ THIS

**Always build RELEASE, never DEBUG.** The single biggest perf trap in this project:
`CONFIG_OPTIMIZATION_LEVEL_DEBUG=y` (`-Og`) leaves the timing-critical composite path unoptimized
and the result is **visibly choppy video + crackly audio**. Symptoms were misdiagnosed for a long
time as emulator/IRAM problems before the real cause (DEBUG) was found.

Correct config per app:
- `sdkconfig`: `CONFIG_OPTIMIZATION_LEVEL_RELEASE=y` (`-Os`).
- `main/component.mk`: `CFLAGS += -O2` and `CXXFLAGS += -O2` — `main` owns `video_out.h` (the
  composite ISR + blit), so it must be `-O2`, not the global `-Os`.
- `odroid/component.mk`: `CFLAGS += -O2` / `CXXFLAGS += -O2` — audio HAL, also timing critical.
- The 6502 cores (`atari800`, `gnuboy`) carry their own `-O2` in their `component.mk`.

**Status** (all RELEASE + verified on hardware):
- `atari` ✅ RELEASE + `-O2` on `main` + `odroid` (atari800 core already `-O2`).
- `launcher` ✅ RELEASE; `main`/`odroid`/`nofrendo`/`smsplus` already carry `-Ofast`.
- `gb` ✅ RELEASE + `-O2` on `main` + `odroid` + `gnuboy` (these were the components left on `-Og`).
- `../RetroESP32_P4` (separate ESP32-P4/HDMI variant, not in this repo) already RELEASE.

## Composite Video Architecture (`components/odroid/video_out.h`)

Derived from rossumur's ESP_8_BIT composite generator.

- `video_init(samples_per_cc, machine, palette, ntsc)` — sets up the I2S DMA + ISR. We run **NTSC**
  (`ntsc=1`). `machine` selects active-line geometry: **`EMU_ATARI` = 384 wide** (Atari 800),
  **`EMU_SMS` = 256 wide** (GB/SMS/GG/2600). `_lines[]` points at the 8-bit framebuffer rows;
  `palette` is 256 packed 4-phase composite words.
- The **ISR (`video_isr`) runs on core 1**, in IRAM, and is extremely timing-sensitive — anything it
  calls must be in IRAM, and contention/optimization (see DEBUG trap above) shows up as glitches.
  The PAL code path (`blit_pal`/`burst_pal`/`pal_sync*`) exists but is never called in NTSC.
- `IRE(x)` macro `((x+40)*255/3.3/147.5)<<8` — `BLACK_LEVEL=IRE(7.5)`, `WHITE_LEVEL=IRE(100)`,
  `BLANKING_LEVEL=IRE(0)`. Byte-identical across all the projects in this family.
- **Audio** is generated per-scanline via LEDC; `audio_write_16()` is the sink.

### Color encoding & calibration

`composite_encode_rgb()` (in `atari/main/main.cpp`) does RGB→YUV→4 composite phase samples, tunable
by NVS calibration keys written by the launcher: `VCHR2` (chroma), `VPHA2` (phase), `VBRI2`
(bright), `VCON2` (contrast). **The RGB-encode path alone gets Atari hues wrong** (greens read blue).
The fix in `atari` is a verbatim **`atari_4_phase_ntsc[256]`** composite palette copied from the
known-good `ESP32_TV_EMU_AtariNES` reference — `build_palette()` runs the RGB loop (only to keep
`composite_encode_rgb` linked / layout stable) then **overrides** `s_apal[i] = atari_4_phase_ntsc[i]`.
For Atari color work, edit the 4-phase table, not the RGB path.

## Per-App Notes

### launcher (factory)
Carousel UI inherited from upstream RetroESP32, so `EMULATORS[]` lists many systems (NES, GB, SMS, GG,
Coleco, ZX Spectrum, 2600, 7800, Lynx, PC Engine, Tyrian, 800). Only a subset is wired in this repo:
**NES (nofrendo) and SMS/GG (smsplus) run in-process** in the factory app; the rest route out by
carousel position / ROM extension via `PROGRAMS[]` + `get_application()` →
`odroid_system_application_set()` + reboot. Implemented routes: `.gb`→ota_1, `.xex`(800)→ota_0,
`.a26`(2600)→ota_0, `.gbc`→ota_1. Other listed systems are placeholders (no app built for them here).
GBC ("NINTENDO GAME BOY COLOR") was re-added as the **last** carousel entry (STEP 15) — appended, not
inserted next to GB, so no existing indices shift (avoids the `STEP==` hardcode bug class). Its games
live in `/sd/roms/gbc/`. ROM scan: `matches_rom_extension()` (note the `step == 14` special-case that
adds `.atr` for 800).

### atari (ota_0)
Atari 800 via `libatari800` at 384×240 composite (`EMU_ATARI`). Features: 4-phase NTSC palette (above),
an **in-game X-menu**, save states, joystick + trigger + console keys (START/SELECT, 5200 START).
`main.cpp` was reconstructed from the session transcript after an overwrite — it's the color-fixed
("bingo") build. **Atari 2600 (Stella) is integrated but deferred**: `Makefile` has
`EXCLUDE_COMPONENTS := stella` so it's not built (the carousel `.a26` entry is intentionally kept).

### gb (ota_1)
Game Boy **and Game Boy Color** via gnuboy, 256-wide composite, GB-style fill scaling. X-menu + save
states. gnuboy auto-detects CGB from the ROM header, so the same app serves both `.gb` and `.gbc`.
NOTE: GBC was once dropped after a ~40-cycle hunt that blamed a "fundamental memory-bus contention"
whole-screen shake — **that verdict was wrong; it was the DEBUG (`-Og`) build.** At RELEASE/`-O2` the
CGB core runs smooth and full-speed (tighter code cuts core-0's SRAM traffic enough to stop starving
the composite DMA). See the optimization section above.

## Cores & Source
Emulator cores were copied from the upstream reference at `../RetroESP32-master/Emulators/*/components/`
(e.g. `stella-odroid-go`, `atari800-odroid-go`, the go-play cores). When adding/refreshing a core,
copy from there, then re-apply the composite `odroid`/`video_out.h` integration + `-O2`.

## Key Gotchas
- **DEBUG vs RELEASE optimization** — see the big section above. This is the #1 thing.
- **C++ exceptions**: only **Stella** needs them (`Cartridge::create` throws) →
  `CONFIG_CXX_EXCEPTIONS=y`. They add unwinding overhead that slows 6502 loops, so when Stella is
  active, `atari800` + `main` carry `-fno-exceptions`. With Stella excluded, exceptions are **off**.
- **IRAM is the binding constraint** for Stella: the composite ISR permanently occupies the IRAM that
  Stella's ~17KB `M6502::execute` needs. On the 4MB ESP32 they don't both fit → `execute` runs from
  flash → ~30% slow. No slot arrangement fixes this (the limit is IRAM, not flash). See below.
- **build/ has absolute paths** — moving/renaming an app dir requires `rm -rf build/` (it's
  gitignored; `build_idf.sh` is self-locating so the rest is portable).
- **NVS at 0x9000** holds settings + composite calibration — don't repartition over it.

## Atari 2600 (Stella) — deferred to a 16MB ESP32
Fully integrated (`atari/components/stella`, driver `stella_run.cpp`, the composite bridge in
`atari/main/main.cpp`) but excluded from the build. Hard-won facts for the revisit:
- Trimmed `emucore/DefProps.hxx` (the 3250-entry game DB, ~600KB) to fit flash.
- Uses 256-wide `EMU_SMS` geometry; allocate the composite framebuffer **after** the Stella console
  (Stella grabs ~100KB internal first, else `bad_alloc`).
- Flicker: render 2 consecutive frames, skip 2 (`(fr++ & 2)==0`) — a 1-of-2 skip aligns with 2600
  2-frame flicker and hides objects (Asteroids).
- The "too slow" verdict was reached **under DEBUG** — at RELEASE/`-O2` it was never retested. On a
  16MB ESP32 (more IRAM headroom for `execute`) + RELEASE, full speed may well be reachable.
