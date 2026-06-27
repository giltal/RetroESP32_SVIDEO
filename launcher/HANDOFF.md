# HANDOFF — RetroESP32_SVIDEO

Handoff for continuing this port in **Claude Code** (terminal). Read this, then
`PLAN.md`, then `README.md`. Everything is on disk; nothing to recover.

---

## What this project is
Porting the **ESP_8_BIT** composite/**S-Video** engine (rossumur) + Atari800/NES cores
from the Arduino sketch `ESP32_TV_EMU_AtariNES` to **ESP-IDF v3.3.1** (GNU Make build),
compiled with **-Ofast**. Video = DAC on **GPIO25** (fixed in HW), audio = LEDC PWM on
**GPIO26**. The video driver is timing-critical; keep it intact.

## Key paths
| What | Path |
|---|---|
| This project (target) | `C:\ESPIDFprojects\RetroESP32_SVIDEO` |
| IDF v3.3.1 toolchain | `C:\Users\97254\esp\v3.3.1\esp-idf` |
| Original Arduino sketch (reference) | `C:\Users\97254\My Drive\ArduinoProjects\ESP32_TV_EMU_AtariNES\ESP32_TV_EMU_AtariNES` |
| Stage-2 source (launcher/emulators) | `C:\ESPIDFprojects\RetroESP32-master` |
| Arduino libs (CH559/SNEScont) — **needed for 1c, location TBD** | `…\Arduino\libraries` (not yet located) |

## Locked decisions
- Milestones: **1a test pattern (DONE)** -> 1b NES -> 1c input -> stage 2 launcher.
- First emulator: **NES (nofrendo)**.
- Input: **CH559 USB-host + direct-GPIO Atari joystick**. **No Wiimote / no BT.**
- Build: **GNU Make** (`component.mk`), `-Ofast` per component.

---

## Current state (what's done)
Milestone **1a is complete and buildable**:
```
Makefile                  project makefile -> $(IDF_PATH)/make/project.mk
sdkconfig.defaults        240 MHz CPU, 1ms tick, 4MB flash
partitions.csv            ready for 1b SPIFFS (NOT yet enabled in sdkconfig)
main/component.mk         CFLAGS/CXXFLAGS += -Ofast -fno-strict-aliasing
main/main.cpp             stage-1a: 8 grey-bar test pattern via the REAL ISR/blit path
main/video_out.h          ported driver
```
`video_out.h` is the original **byte-for-byte** except the audio init: Arduino
`ledcSetup/ledcAttachPin/ledcWrite` were replaced by `audio_init_ledc()` using
`driver/ledc.h`; `AUDIO_PIN` = 26. ISR, blit, sync, burst, APLL setup are untouched.

## First thing to do in Claude Code
Verify 1a actually compiles and runs on hardware before adding the emulator.
```bat
set IDF_PATH=C:\Users\97254\esp\v3.3.1\esp-idf
cd C:\ESPIDFprojects\RetroESP32_SVIDEO
make defconfig
make -j flash monitor
```
Expected: 8 steady grey bars (black->white) on the TV, no rolling; serial prints an
incrementing `frame=`. If it doesn't build, fix compiler/linker errors (this is the
main reason for moving to Claude Code — it can see and fix the build output).

## Next: milestone 1b (NES / nofrendo)
1. Bring `emu.cpp`, `gui.cpp`, `emu_nofrendo.cpp`, and `src/nofrendo/` from the Arduino
   sketch into IDF component(s) under `components/` (own `component.mk`, `-Ofast`).
2. Enable SPIFFS: turn on the custom partition table (`partitions.csv`) in menuconfig,
   add `spiffs` component, mount it, auto-populate sample ROM as the sketch does.
3. Replace the Arduino `setup()/loop()` model (see sketch `.ino`) with `app_main()` +
   `emu_task` pinned to core 0 and the video pump on core 1.
4. Swap remaining Arduino APIs: `EEPROM`->NVS, `millis/delay`->`esp_timer`/`vTaskDelay`,
   `ESP.getCycleCount`->`xthal_get_ccount`, PSRAM via menuconfig.
5. For 1b you can stub input (no controller yet); wire real input in 1c.

## Milestone 1c (input — needs Arduino libs)
Port `CH559` + `SNEScont` over `driver/uart`; map into the existing `SNESbuttons`
flow; keep the direct-GPIO Atari joystick reads. **Requires the Arduino `libraries`
folder** — locate `CH559.h/.cpp` and `SNEScont.h/.cpp` first.

## Stage 2 (launcher + emulators)
From `C:\ESPIDFprojects\RetroESP32-master` (odroid-go based, IDF v3.2/3.3, `mkfw.py`).
Main challenge: that code renders to the odroid-go **SPI LCD**; here the display is the
**composite driver**, so the launcher's display/backlight/LCD layer must be retargeted
to write into `video_out`'s `_lines` framebuffer. Design in detail after stage 1 is
stable on hardware.

---

## Suggested first prompt for Claude Code
> Read PLAN.md, README.md and HANDOFF.md in this folder
> (C:\ESPIDFprojects\RetroESP32_SVIDEO). Milestone 1a is done. First, build it with the
> ESP-IDF v3.3.1 Make toolchain (IDF_PATH=C:\Users\97254\esp\v3.3.1\esp-idf) and fix any
> compile/link errors so `make flash monitor` produces the grey-bar test pattern. Do not
> modify the timing-critical parts of video_out.h (ISR/blit/sync/burst/APLL). Once it
> builds clean, stop and report before we start milestone 1b (NES/nofrendo).
