# RetroESP32_SVIDEO

ESP_8_BIT composite / **S-Video** output on ESP-IDF **v3.3.1**, built with **-Ofast**.
Video on **GPIO25** (DAC), audio on **GPIO26** (LEDC PWM).

See `PLAN.md` for the full roadmap. This tree currently implements **milestone 1a**
(test pattern). Stages 1b (NES) and 2 (launcher) follow.

## Build & flash (stage 1a)

Use the v3.3.1 IDF environment at `C:\Users\97254\esp\v3.3.1`.

```bat
:: set up the IDF environment for this shell (adjust if your export script differs)
set IDF_PATH=C:\Users\97254\esp\v3.3.1\esp-idf

cd C:\ESPIDFprojects\RetroESP32_SVIDEO
make defconfig
make -j flash monitor
```

Set the serial port if needed: `make menuconfig` -> *Serial flasher config* -> port,
or `make flash ESPPORT=COMx`.

### Expected result
8 vertical grey bars (black -> white), rock steady, no rolling/tearing. The serial
monitor prints an incrementing `frame=` counter. That confirms: toolchain + `-Ofast`,
APLL color clock, DAC video on GPIO25, and the genuine ISR/blit/sync path all work.

If the picture rolls or is missing sync, check NTSC vs PAL (driver call in `main.cpp`
passes `ntsc=1`) and the S-Video luma wiring on GPIO25.

## Layout
```
Makefile              project makefile (includes IDF make/project.mk)
sdkconfig.defaults    240 MHz, 1ms tick, 4MB flash
partitions.csv        ready for 1b (SPIFFS); not yet enabled in sdkconfig
main/
  component.mk        -Ofast + -fno-strict-aliasing
  main.cpp            stage-1a test pattern
  video_out.h         ported driver (audio path -> IDF LEDC, otherwise verbatim)
```

## Notes on the port
- `video_out.h` is byte-for-byte the original ESP_8_BIT driver except the audio init:
  Arduino `ledcSetup/ledcAttachPin/ledcWrite` were replaced by `audio_init_ledc()`
  using `driver/ledc.h`. The ISR, blit, sync, burst and APLL setup are untouched.
- `AUDIO_PIN` set to 26; video DAC is `DAC_CHANNEL_1` = GPIO25 (fixed in hardware).
