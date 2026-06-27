# RetroESP32_SVIDEO — Port Plan

Porting the **ESP_8_BIT** composite/S-Video engine (rossumur) + Atari800/NES cores
from the Arduino sketch `ESP32_TV_EMU_AtariNES` to **ESP-IDF v3.3.1** (GNU Make build),
compiled with **-Ofast**, video on **GPIO25** (DAC), audio on **GPIO26** (LEDC PWM).

Toolchain: `C:\Users\97254\esp\v3.3.1` (IDF v3.3.1, legacy `make/project.mk` build).
Target project: `C:\ESPIDFprojects\RetroESP32_SVIDEO`.

---

## Decisions (locked)

| Topic | Choice |
|---|---|
| First milestone | **1a: test pattern** (signal validation), then NES |
| First emulator | **NES (nofrendo)** |
| Input | **CH559 USB-host + direct-GPIO Atari joystick**; **no Wiimote/BT** |
| Build system | **GNU Make** (`component.mk`, `-Ofast` via per-component CFLAGS) |
| Video pin | DAC `DAC_CHANNEL_1` = **GPIO25** (fixed in silicon) |
| Audio pin | LEDC PWM = **GPIO26** |

---

## What ports cleanly vs. what needs work

The driver core in `video_out.h` is **already IDF-native** and must stay byte-for-byte
where possible (it is timing-critical):

- I2S0 + DMA + APLL setup (`start_dma`), `rtc_clk_apll_enable(1,0x46,0x97,0x4,2)` —
  signature matches IDF v3.3.1.
- `video_isr` / `blit` / `blit_pal` / `sync` / `burst` / `blanking` — pure register + math.
- `IRAM_ATTR`, `esp_intr_alloc`, `dac_output_enable`, `dac_i2s_enable` — native IDF.

**Must be replaced** (Arduino-only):

| Arduino API | IDF replacement |
|---|---|
| `ledcSetup` / `ledcAttachPin` / `ledcWrite` | `driver/ledc.h` (`ledc_timer_config` + `ledc_channel_config`) |
| `setup()` / `loop()` | `app_main()` + pinned FreeRTOS tasks |
| `EEPROM` | NVS (`nvs_flash`) |
| `Serial2` (CH559 UART) | `driver/uart.h` |
| `SPI.h` / `SD.h` | `driver/sdspi` or SPIFFS (start with SPIFFS) |
| `millis()` / `delay()` | `esp_timer_get_time()` / `vTaskDelay()` |
| `pinMode` / `digitalRead` / `analogRead` | `driver/gpio.h` / `driver/adc.h` |
| `ESP.getCycleCount()` | `xthal_get_ccount()` (already used elsewhere) |
| `psramInit()` / `ESP.getFreePsram()` | `esp_spiram_*` / `heap_caps_*` (config via menuconfig) |
| `rtc_clk_cpu_freq_set(RTC_CPU_FREQ_240M)` | set 240 MHz in `sdkconfig` |
| `Wiimote.*` | **removed** |

---

## -Ofast strategy

IDF defaults to `-Og`/`-O2`. `-Ofast` is applied **per component** so the global build
stays stable while the timing-sensitive code is fully optimized:

```make
# main/component.mk
CFLAGS   += -Ofast
CPPFLAGS += -Ofast
CXXFLAGS += -Ofast
```

Emulator cores get the same in their own `component.mk`. Keep `-fno-strict-aliasing`
where the original relies on type-punning (the blit reads 32-bit words from byte buffers).

---

## Milestones

### 1a — Test pattern (signal validation)  ← first build
- Minimal `app_main`: init video HW, feed the **real** ISR/blit pipeline a synthetic
  framebuffer (grayscale/color bars) + a small palette.
- Goal: stable NTSC picture on the TV via S-Video, correct sync/burst, no rolling.
- Proves: toolchain, `-Ofast`, APLL clock, DAC on GPIO25, LEDC audio carrier on GPIO26.
- **No emulator, no input, no filesystem.**

### 1b — NES (nofrendo)
- Bring in `emu.cpp`, `gui.cpp`, `emu_nofrendo.cpp`, `nofrendo/` as IDF component(s).
- Mount SPIFFS; auto-populate sample ROM (as the sketch does).
- Replace `setup()/loop()` with `emu_task` pinned to core 0 + video pump on core 1.
- Goal: NES boots to GUI + game, video + audio.

### 1c — Input (CH559 + GPIO joystick, no Wiimote)
- Port `CH559` + `SNEScont` libs over `driver/uart` (needs Arduino `libraries` folder).
- Map to the existing `SNESbuttons` flow; keep direct-GPIO Atari joystick reads.
- EEPROM emulator-select → NVS.

### Stage 2 — Launcher + emulators (RetroESP32-master)
- Source: `C:\ESPIDFprojects\RetroESP32-master` (odroid-go based, IDF v3.2/3.3, `mkfw.py`).
- Key reconciliation: RetroESP32 targets the odroid-go **SPI LCD**; here the display is
  the **composite driver**. The launcher's display/backlight/LCD layer must be retargeted
  to `video_out`'s framebuffer, or the launcher rendered into a `_lines` buffer.
- Pull launcher (`Launchers/retro-esp32`) + selected emulators; align partition/`mkfw` layout.
- Detailed design after stage 1 is stable on hardware.

---

## Project layout (Make build)

```
RetroESP32_SVIDEO/
  Makefile                  # PROJECT_NAME + include $(IDF_PATH)/make/project.mk
  sdkconfig.defaults        # 240MHz, PSRAM, SPIFFS, partition table
  partitions.csv
  main/
    component.mk            # -Ofast
    main.cpp               # app_main (1a test pattern first)
    video_out.h            # ported (audio -> IDF LEDC, AUDIO_PIN=26)
  components/               # (1b+) nofrendo, atari800, emu glue, ch559
```

## Open dependency
- **Arduino `libraries` folder** (CH559, SNEScont sources) required for milestone 1c.
  Test pattern (1a) and NES video (1b) do not need it.

## Hardware-test loop
I cannot flash or scope the board. Each milestone ships buildable code + exact
`make flash monitor` steps; you validate on the TV and report symptoms, I iterate.

---

## VERIFIED build + flash recipe (2026-06-22)

Milestone **1a is DONE and validated on hardware** (8 grey bars confirmed on TV).
Non-obvious environment facts discovered during bring-up:

- IDF v3.3.1 legacy Make needs the **old `xtensa-esp32-elf` GCC 5.2.0** at
  `C:\Users\97254\esp\toolchains\xtensa-esp32-elf` (NOT the modern GCC14 also installed).
- The build **must run in msys64 MINGW32 mode**. Use `build_idf.sh`:
  `MSYSTEM=MINGW32 C:\msys64\usr\bin\bash -l build_idf.sh defconfig|all`
- **Flashing must use NATIVE WINDOWS python**, not msys/cygwin python — the cygwin
  pyserial uses the POSIX backend and never toggles RTS/DTR, so ESP32 auto-reset
  fails. Use `flash.py` (port form `//./COM6`): `python flash.py COM6 [mon]`.
  Auto-reset works, no BOOT/EN buttons needed.

---

## Stage B — REVISED PLAN (supersedes Milestones 1b + Stage 2 above)

Decision (2026-06-22): instead of porting the Arduino-sketch nofrendo, build a
**single monolithic app** = **RetroESP32-master carousel launcher + emulators**,
all retargeted from the odroid-go SPI LCD to our **composite `video_out`**.
Constraints: **4 MB flash, 8 MB PSRAM**. Start with NES, add emulators incrementally.

Locked sub-decisions:
- **NES core**: RetroESP32-master `nesemu-go` (retro-go nofrendo), with NEW
  8-bit-index -> composite video glue (bypass its RGB565 `ili9341_write_frame_scaled`).
- **First deliverable**: **carousel GUI on composite** (solve RGB565->composite up front),
  then hang NES off it.

### Source architecture (RetroESP32-master)
- Launcher: `Launchers/retro-esp32/main/main.c` (~3500 lines), RGB565, 320x240.
  Display choke-point is almost entirely **`ili9341_write_frame_rectangleLE(l,t,w,h,uint16_t*)`**
  (38 calls); only other display calls are `ili9341_init/clear/prepare` +
  `is_backlight_initialized`. HAL is `components/odroid/` (display/input/audio/
  settings/system/sdcard).
- Emulators are SEPARATE OTA apps switched via `esp_ota_set_boot_partition`+reboot
  (`odroid_system_application_set`). Monolithic merge must replace OTA with in-process
  calls and share the ~176 KB internal DRAM serially (alloc on entry / free on exit).
  Flash for NES-only is fine; **RAM coexistence is the real constraint**.

### Build/merge approach (this project)
Evolve `RetroESP32_SVIDEO`:
- Bring launcher `main.c` + `includes/` + `sprites/` into `main/`.
- Copy `components/odroid/` but **replace `odroid_display.c`** with a composite impl:
  `ili9341_init()` -> start `video_out` pump + alloc framebuffer;
  `ili9341_write_frame_rectangleLE()` -> blit RGB565 rect into a 256x240 framebuffer
  (downscale 320->256 to NES geometry); `clear/prepare/backlight` -> trivial.
- Neuter OTA in `odroid_system`; stub SD -> SPIFFS; retarget input->GPIO, audio->LEDC.
- Partitions: single `factory` app + `spiffs` (no OTA). Enable PSRAM in sdkconfig.

### Display color staging
1. **Grayscale-first**: RGB565 -> luma byte -> 256-entry gray composite palette
   (reuses the proven 1a gray-palette + NES blit). Gets the carousel on the TV with
   ~zero signal risk; validates merge/build/geometry/frame-pump.
2. **Composite color**: proper NTSC composite palette + RGB565->index LUT.

### Stage B progress (confirmed on hardware)
- [x] Monolithic launcher builds (one ~690KB app w/ NES, fits 4MB) and boots.
- [x] Carousel renders over composite via odroid_display.cpp bridge.
- [x] Task-watchdog glitch fixed: odroid_input_gamepad_read yields ~10ms (idle task runs).
- [x] Input working: direct-GPIO Atari joystick + CH559 USB host (core-1... now core-0 task).
- [x] SD card working (retargeted to this board's SPI pins) + FATFS long filenames.
- [x] Composite COLOR (256-entry 3-3-2 NTSC palette; CHROMA_GAIN/PHASE_OFFSET tunable).
- [x] **NES RUNNING** at full speed, rock-steady picture (see learnings below).
- [x] **In-game menu** (button X): Continue / Save State / Load State / Reset / Quit.
- [x] **Save/Load state** (nofrendo SNSS) to `<rom>.ss0` on SD.
- [x] Settings tab trimmed to THEMES / VOLUME / CLEAR RECENTS; icons always colored; no battery.
- [x] **NES audio** working (APU -> LEDC PWM on GPIO5).
- [x] Boots straight to carousel (SPLASH=false); recents fixed; settings nav/layout fixed.
- [ ] Composite color hue/saturation fine-tuning (CHROMA_GAIN/PHASE_OFFSET, on hardware).
- [ ] More emulators (GB/SMS/...) via the same in-process pattern.
- [ ] In-process return from NES instead of reboot (currently esp_restart on exit).

### NES audio (main/nes_run.cpp + odroid_display.cpp)
- Infra already existed: video ISR calls audio_sample() once per scanline, draining
  video_out's _audio_buffer (LEDC PWM on AUDIO_PIN=GPIO5). It just wasn't fed.
- nes_pump_audio() renders one frame of APU output (NES_AUDIO_FRAME=262 8-bit samples
  -> 16-bit) and composite_audio_write() -> audio_write_16(). Rate NES_AUDIO_RATE=15720
  = 262 samples/frame (matches the per-scanline drain rate).
- The per-frame producer is bursty while the drain is steady, so the ring ran near
  empty and any jitter underflowed it (audio "slowness"). Fix: PRE-FILL ~3 frames
  before the loop (cushion). Also vTaskPrioritySet(NULL, 8) so emulation runs above
  the CH559 input task (prio 6) and isn't preempted mid-frame.

### Misc launcher fixes (main.c)
- SPLASH=false -> boot straight into the carousel (no logo).
- draw_volume() y aligned to the VOLUME row (was at POS.y+86 / old 3rd row -> +66).
- Settings LEFT/RIGHT browse the carousel unless on VOLUME (was: only on THEMES) so
  you can leave the config page from any row.
- Recents: add_recent() rewritten - it only wrote the file if it already existed
  (so recents never started) and fclose'd a NULL handle; now always writes + creates
  /sd/odroid/data/<folder>/ via ensure_data_dir().

### In-game menu + save/load (main/nes_run.cpp + nofrendo)
- Button X surfaced from CH559 decode via odroid_input_x_held() (odroid_input.c).
- Menu is drawn DIRECTLY into the 8-bit NES framebuffer (NES palette indices) using
  the launcher's FONT_5x7 (extern'd from main.c) - the launcher's RGB renderer/fb is
  freed during a game, so we compose over the paused frame. nes_menu() in nes_run.cpp.
- Save/Load: nofrendo state_save()/state_load()/state_setslot() (SNSS format).
  Two fixes were required:
  * nes_rom.c rom_load() (the osd_getromdata path) now sets rominfo->filename so the
    state file derives a real path (was empty -> ".ss0").
  * osd.c osd_newextension() actually swaps the extension (.nes -> .ss0) so the save
    sits next to the ROM instead of overwriting it.
  * libsnss.c SNSS_OpenFile() allocates the ~30KB SNSS_FILE from PSRAM (internal heap
    is too tight mid-game; was failing with SNSS_OUT_OF_MEMORY).
- Settings cleanup: draw_settings + the STEP-0 LEFT/RIGHT/UP/DOWN/A handlers renumbered
  to 3 items; get_toggle() forces COLOR=1; #define BATTERY removed (definitions.h).

### NES integration — how it works (main/nes_run.cpp + components/nofrendo)
- Core = the **sketch's nofrendo** (composite-native osd.c: `nes_emulate_init`/
  `nes_emulate_frame` drive it frame-at-a-time, return primary_buffer->line). Same
  emulator as nesemu-go; chosen for the ready composite frame driver.
- Launcher hook: `launch_inprocess()` in main.c rom_run/rom_resume detects `.nes`
  by ROM.name extension (ROM.ext is NOT populated on the browser path) and calls
  `nes_run(full_path)` instead of the OTA reboot.
- `nes_run`: load ROM into PSRAM (nofrendo reads via osd_getromdata), free the
  launcher fb, nes_emulate_init, then loop: wait vblank -> render -> feed input.
  Exit (MENU) -> esp_restart back to launcher.
- NES palette = precomputed `nes_4_phase[64]` (EMU_NES, 6-bit index). composite_use_nes()
  swaps the pump's machine/palette/lines; composite_use_launcher() restores.

### CRITICAL video-timing learnings (hard-won; do NOT regress)
- **Video ISR MUST be on core 1**, away from the busy launcher/emulator on core 0
  (odroid_display.cpp starts video_init from a core-1 task). Core-0 activity was
  delaying the ISR -> picture jumped.
- **Video ISR is ESP_INTR_FLAG_LEVEL3** (video_out.h start_dma), not LEVEL1. The DMA
  has only ~1 line (63.5us) of slack; at LEVEL1 the FreeRTOS tick etc. delayed the
  refill and corrupted a few scanlines every few seconds ("jump a few lines"). LEVEL3
  lets it preempt them -> ROCK SOLID. This was THE fix for the residual jerk.
- CH559 input task is on **core 0** (input task on core 1 also disturbed the ISR).
- Do NOT alternate 262/263 lines per frame — the TV tries to interlace it and the
  picture shimmers ("missing vertical lines"). Fixed 262-line progressive is correct.
- Internal RAM is tight: launcher fb (padded, FB_ALLOC) is FREED before NES so
  nofrendo's primary_buffer (~64KB, must be internal for the ISR) fits the hole.
- PSRAM enabled (8MB) holds the NES ROM; rev1 cache workaround on.

### Confirmed pin map (this board)
| Function | GPIO |
|---|---|
| Composite video (DAC1) | 25 |
| Audio (LEDC PWM) | 5 |
| CH559 USB host UART2 | RX 19, TX 23 |
| D-pad | Up 22, Down 21, Left 14, Right 27 |
| Buttons | Fire/A 32, Start 34, Select 35, Home/Menu 26 |
| SD-SPI | CLK 15, MISO 18, MOSI 13, CS 4 |

### Input architecture (odroid_input.c)
CH559 USB-HID host (ported from the CH559 Arduino lib) is drained by a dedicated
**core-1 task** (`ch559_task`) - draining on core 0 let the video ISR + launcher
redraws starve the 1Mbaud RX, overflowing the buffer and desyncing the frame parser
(~75% frame loss). Core-1 task -> 0% loss. Decoded pads (SNES/RetroJoy/SparkFun
mappers, by USB device ID) update shared state; gamepad_read merges that with the
direct-GPIO joystick. The user's USB pad enumerates as SNES mapper ID 0x79001100.
NOTE: the bit-banged SNEScont library is NOT used (CH559 path only).
