// stella_run.cpp - Atari 2600 (Stella) driver for the composite ota_0 app.
//
// Lives in the stella component so it compiles WITH C++ exceptions (Stella's cart/console
// creation throws). It must NOT include video_out.h (that lives in main.cpp, one TU, and main is
// built -fno-exceptions). Instead it uses the small composite interface main.cpp exposes:
//   atari_composite_start(fb, rgb_palette)  - encode RGB->composite into s_apal, start the pump
//   atari_composite_audio(buf, n)           - feed audio
//   atari_composite_fc()                    - frame counter (for vsync pacing)
//   atari_exit_to_launcher()                - Home/quit
//
// Output geometry matches the atari800 path: a 384x240 8-bit composite framebuffer. The 2600's
// 160xN TIA frame is scaled up to fill the screen, GB-style (nearest-neighbor byte copy; the TIA
// framebuffer is already 8-bit palette indices, so no per-pixel conversion).

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <exception>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

extern "C" {
#include "odroid_input.h"
}

// Stella core
#include "Console.hxx"
#include "Cart.hxx"
#include "Props.hxx"
#include "PropsSet.hxx"
#include "MD5.hxx"
#include "Sound.hxx"
#include "SoundSDL.hxx"
#include "OSystem.hxx"
#include "Settings.hxx"
#include "TIA.hxx"
#include "Switches.hxx"
#include "Control.hxx"
#include "Event.hxx"
#include "EventHandler.hxx"

// composite interface (implemented in main.cpp, which owns video_out.h)
extern "C" void atari_composite_start(uint8_t* fb, const uint32_t* rgb_palette);
extern "C" void atari_composite_audio(const int16_t* buf, int n);
extern "C" int  atari_composite_fc(void);
extern "C" void atari_exit_to_launcher(void);

// The 2600 is 160 wide, so it uses the 256-wide SMS composite geometry (like GB/GG) rather than
// the Atari 800's 384 width. Smaller framebuffer (60KB vs 92KB) -> fits the fragmented heap left
// after Stella allocates its TIA buffers, and 160->256 is the same GB-style fill scaling.
#define FB_W 256
#define FB_H 240

// Stella's TIA references this global frameskip flag (the odroid-go port defined it). true = render
// every frame into the TIA framebuffer; set false on frames we skip drawing if we need frameskip.
bool RenderFlag = true;

static OSystem*   osystem   = 0;
static Console*   console   = 0;
static Cartridge* cartridge = 0;
static Settings*  settings  = 0;

static uint8_t*  s_fb = 0;            // 384x240 composite framebuffer
static uint16_t  s_xmap[FB_W];        // dst x -> src x (nearest-neighbor)
static int       s_srcW = 160, s_srcH = 192;
static int       s_dstY0 = 5, s_dstH = 230;   // GB-style: fill width, ~5px top/bottom border
static int32_t*  s_sample = 0;
static int       s_spf = 524;         // stella samples per frame (~31400/60)

// Scale the 2600's 8-bit frame into the full-width composite framebuffer. H-scale each distinct
// source row once; memcpy the rows vertical scaling duplicates (same trick as the GB blit).
static void stella_blit(const uint8_t* src) {
    int prev_sy = -1; uint8_t* prev = 0;
    for (int dy = 0; dy < s_dstH; dy++) {
        int sy = (dy * s_srcH) / s_dstH;
        uint8_t* dst = s_fb + (s_dstY0 + dy) * FB_W;
        if (sy == prev_sy) { memcpy(dst, prev, FB_W); }
        else {
            const uint8_t* srow = src + sy * s_srcW;
            for (int dx = 0; dx < FB_W; dx++) dst[dx] = srow[s_xmap[dx]];
            prev_sy = sy; prev = dst;
        }
    }
}

extern "C" void stella_run(const char* path) {
    printf("stella_run: %s\n", path);

    // Load the ROM into PSRAM
    FILE* fp = fopen(path, "rb");
    if (!fp) { printf("stella: cannot open %s\n", path); atari_exit_to_launcher(); }
    fseek(fp, 0, SEEK_END); size_t size = ftell(fp); fseek(fp, 0, SEEK_SET);
    void* data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!data || fread(data, 1, size, fp) != size) { printf("stella: ROM read failed\n"); fclose(fp); atari_exit_to_launcher(); }
    fclose(fp);

    string cartMD5 = MD5((uInt8*)data, (uInt32)size);
    osystem = new OSystem();
    Properties props;
    osystem->propSet().getMD5(cartMD5, props);
    settings = new Settings(osystem);
    settings->setValue("romloadcount", false);

    string cartType = props.get(Cartridge_Type);
    string cartId;
    try {
        cartridge = Cartridge::create((const uInt8*)data, (uInt32)size, cartMD5, cartType, cartId, *osystem, *settings);
    } catch (...) { cartridge = 0; }
    if (!cartridge) { printf("stella: cartridge create failed\n"); atari_exit_to_launcher(); }

    printf("stella: heap before console: internal=%u psram=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    try {
        console = new Console(osystem, cartridge, props);
    } catch (const string& e) { printf("stella: console exception: %s\n", e.c_str()); console = 0; }
      catch (const std::exception& e) { printf("stella: console std::exception: %s\n", e.what()); console = 0; }
      catch (...) { printf("stella: console unknown exception\n"); console = 0; }
    if (!console) { printf("stella: console create failed\n"); atari_exit_to_launcher(); }
    osystem->myConsole = console;

    console->initializeVideo();
    console->initializeAudio();

    TIA& tia = console->tia();
    s_srcW = tia.width();
    s_srcH = tia.height();
    printf("stella: src %dx%d, framerate %.2f\n", s_srcW, s_srcH, console->getFramerate());

    // Fill width (like GB); small top/bottom border so the aspect isn't wildly stretched.
    s_dstH  = FB_H - 10;
    s_dstY0 = (FB_H - s_dstH) / 2;
    for (int dx = 0; dx < FB_W; dx++) s_xmap[dx] = (uint16_t)((dx * s_srcW) / FB_W);

    // Allocate the composite framebuffer AFTER the console (Stella grabs ~100KB internal for its
    // TIA framebuffers first; allocating s_fb earlier left too little -> std::bad_alloc).
    s_fb = (uint8_t*)heap_caps_malloc(FB_W * FB_H, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!s_fb) { printf("stella: fb alloc failed (internal=%u)\n",
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); atari_exit_to_launcher(); }
    memset(s_fb, 0, FB_W * FB_H);

    // Palette: Stella's standard 2600 RGB palette -> our composite encoder (in main.cpp)
    const uint32_t* pal = console->getPalette(0);
    atari_composite_start(s_fb, pal);     // encodes + starts the video pump on core 1

    s_spf = (int)(31400.0f / console->getFramerate());
    s_sample = (int32_t*)malloc(s_spf * sizeof(int32_t));

    Event& ev = osystem->eventHandler().event();
    vTaskPrioritySet(NULL, 8);

    // Prime the audio ring with a cushion of frames so the slightly-under-60fps emulation
    // doesn't underrun it (smooths the choppiness). No render during prime.
    {
        SoundSDL* snd = (SoundSDL*)&osystem->sound();
        static int16_t out[512];
        for (int p = 0; p < 8; p++) {
            RenderFlag = false;
            tia.update();
            snd->processFragment((int16_t*)s_sample, s_spf);
            const int16_t* s16 = (const int16_t*)s_sample;
            int n = s_spf / 2; if (n > 512) n = 512;
            for (int i = 0; i < n; i++) out[i] = s16[i * 2];
            atari_composite_audio(out, n);
        }
    }

    for (;;) {
        odroid_gamepad_state gp;
        odroid_input_poll(&gp);
        if (gp.values[ODROID_INPUT_MENU]) atari_exit_to_launcher();   // Home = quit to launcher

        ev.set(Event::Type(Event::JoystickZeroUp),    gp.values[ODROID_INPUT_UP]);
        ev.set(Event::Type(Event::JoystickZeroDown),  gp.values[ODROID_INPUT_DOWN]);
        ev.set(Event::Type(Event::JoystickZeroLeft),  gp.values[ODROID_INPUT_LEFT]);
        ev.set(Event::Type(Event::JoystickZeroRight), gp.values[ODROID_INPUT_RIGHT]);
        ev.set(Event::Type(Event::JoystickZeroFire),  gp.values[ODROID_INPUT_A]);
        ev.set(Event::Type(Event::ConsoleSelect),     gp.values[ODROID_INPUT_SELECT]);
        ev.set(Event::Type(Event::ConsoleReset),      gp.values[ODROID_INPUT_START]);

        console->controller(Controller::Left).update();
        console->controller(Controller::Right).update();
        console->switches().update();

        // Frameskip 2-of-4: render two consecutive frames, then skip two. That's half the
        // render/blit cost (keeps it playable) but rendering CONSECUTIVE frames captures both
        // phases of 2600 2-frame flicker (Asteroids etc.) - unlike a 1-of-2 skip, which aligned
        // with the flicker and made those objects vanish. CPU + audio still run every frame.
        static unsigned fr = 0;
        bool render = ((fr++ & 2u) == 0);
        RenderFlag = render;

        tia.update();

        // Audio: Stella mixes ~31400 Hz; our composite sink runs 15720 Hz -> decimate by 2.
        SoundSDL* snd = (SoundSDL*)&osystem->sound();
        snd->processFragment((int16_t*)s_sample, s_spf);
        const int16_t* s16 = (const int16_t*)s_sample;
        static int16_t out[512];
        int n = s_spf / 2; if (n > 512) n = 512;
        for (int i = 0; i < n; i++) out[i] = s16[i * 2];
        atari_composite_audio(out, n);

        // Video: scale the TIA frame into the composite framebuffer (only on rendered frames)
        if (render) stella_blit(tia.currentFrameBuffer());

        // Pace to the composite vsync (one frame), like the atari800 path
        int fc = atari_composite_fc(), guard = 0;
        while (atari_composite_fc() == fc && ++guard < 200) vTaskDelay(1);
    }
}
