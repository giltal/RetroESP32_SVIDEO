// odroid_display.cpp - COMPOSITE video bridge for RetroESP32_SVIDEO
//
// Replaces the original odroid ILI9341 SPI-LCD driver. The launcher (and later the
// emulators) call the same ili9341_* API, but here those calls render into the
// composite video framebuffer driven by video_out.h (the proven 1a NTSC pipeline).
//
// Stage B step 1: GRAYSCALE. The launcher renders RGB565 at 320x240; we downscale
// to the 256x240 composite (EMU_SMS geometry, full 256-entry palette) and map each
// pixel to a luma level. Composite COLOR comes next (real NTSC palette + RGB->index).

extern "C" {
#include "odroid_display.h"
}
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

// blit()'s machine switch needs these (normally from emu.h)
#define EMU_ATARI 1
#define EMU_NES   2
#define EMU_SMS   3
#include "video_out.h"   // the composite driver: video_init(), _lines, blit(), ISR...

// Launcher native canvas
#define SRC_W 320
#define SRC_H 240
// Composite active area (EMU_SMS/NES geometry: 256 px over the NTSC active line, 240 lines)
#define FB_W  256
#define FB_H  240
// One shared screen buffer for EVERYTHING (launcher + NES + SMS + Atari), sized to the
// widest emulator (Atari 384). Static + internal: the IRAM video ISR reads it every line,
// it's guaranteed contiguous, and because no emulator allocates its own framebuffer there's
// no heap fragmentation and the freed heap is left for the cores' working RAM. Each user
// addresses it with its own stride (256 for NES/SMS/launcher, 384 for Atari).
// 256-wide composite buffer with headroom for NES's 272-pitch (8px) overdraw. This is the
// proven size that coexists with the SD/FATFS internal allocations. Atari (384 native) is
// too big to also fit here alongside SD, so it renders to PSRAM and downscales into this.
#define SHARED_SZ  (FB_W * FB_H + 12 * 1024)
uint8_t* g_emu_screen = 0;                 // shared composite framebuffer (heap, alloc'd at boot)

static uint8_t*  s_fb = 0;                 // launcher view (256-wide), one index per pixel
static uint8_t*  s_line_ptrs[FB_H];        // -> handed to the driver as _lines
static uint32_t  s_palette[256];           // gray composite ramp (DAC byte x4)
static bool      s_started = false;

// --- Composite COLOR tunables: runtime-adjustable from the launcher's Video Calibration
//     screen (composite_set_color_params) and persisted to NVS by the launcher. Defaults
//     match the original compile-time constants. ---
static float g_chroma   = 25.0f;  // chroma amplitude (saturation); tuned on hardware
static float g_phase    = -70.0f; // degrees; hue tuned on hardware (aligns with NES palette)
static float g_bright   = 0.0f;   // luma offset, normalized (-0.5 .. +0.5)
static float g_contrast = 1.0f;   // luma gain (0.3 .. 2.0)

// RGB565 (native uint16) -> 3-3-2 RGB palette index (256-color cube).
static inline uint8_t rgb565_to_index(uint16_t p)
{
    uint8_t r3 = (p >> 13) & 0x07;   // top 3 bits of R5
    uint8_t g3 = (p >> 8)  & 0x07;   // top 3 bits of G6
    uint8_t b2 = (p >> 3)  & 0x03;   // top 2 bits of B5
    return (r3 << 5) | (g3 << 2) | b2;
}

// Build the 256-entry NTSC composite palette for the 3-3-2 cube. Each entry packs
// 4 DAC samples (one color clock) as p0<<24|p1<<16|p2<<8|p3, matching the EMU_SMS
// blit and the original ESP_8_BIT make_nes_palette() convention:
//   sample = luma + chroma, chroma over the 4 phases = [+P, +Q, -P, -Q]
//   P = in-phase (burst axis), Q = quadrature, derived from YUV.
// Encode one 8-bit RGB color into a composite palette word (4 phase samples,
// p0<<24|p1<<16|p2<<8|p3). Shared by the launcher (3-3-2 cube) and emulators with
// dynamic palettes (e.g. SMS). Honors CHROMA_GAIN / PHASE_OFFSET.
extern "C" uint32_t composite_encode_rgb(int r, int g, int b)
{
    const float lo   = (float)BLACK_LEVEL;
    const float span = (float)(WHITE_LEVEL - BLACK_LEVEL);
    const float th   = g_phase * (float)M_PI / 180.0f;
    const float ct = cosf(th), st = sinf(th);

    float Y = 0.299f*r + 0.587f*g + 0.114f*b;
    float U = -0.147407f*r - 0.289391f*g + 0.436798f*b;
    float V =  0.614777f*r - 0.514799f*g - 0.099978f*b;
    float Ur = U*ct - V*st;
    float Vr = U*st + V*ct;

    float yv = (Y / 255.0f) * g_contrast + g_bright;   // brightness/contrast on luma
    if (yv < 0.0f) yv = 0.0f;
    if (yv > 1.0f) yv = 1.0f;
    float luma = lo + yv * span;
    float P = -g_chroma * Ur;
    float Q =  g_chroma * Vr;
    float s[4] = { luma + P, luma + Q, luma - P, luma - Q };

    uint32_t word = 0;
    for (int j = 0; j < 4; j++) {
        float v = s[j];
        if (v < 0)        v = 0;
        if (v > 65280.0f) v = 65280.0f;
        word = (word << 8) | ((uint32_t)v >> 8);
    }
    return word;
}

static void build_color_palette()
{
    for (int idx = 0; idx < 256; idx++) {
        int r = ((idx >> 5) & 7) * 255 / 7;
        int g = ((idx >> 2) & 7) * 255 / 7;
        int b = ( idx       & 3) * 255 / 3;
        s_palette[idx] = composite_encode_rgb(r, g, b);
    }
}

// video_init registers the I2S interrupt on whatever core runs it. We run it from a
// task pinned to CORE 1 so the timing-critical video ISR lives away from the busy
// launcher/emulator on core 0 -> steady NTSC sync (core 0 activity was delaying the
// ISR and making the picture jump, exactly like the original ESP_8_BIT 2-core split).
static volatile bool s_vinit_done = false;
static void video_init_core1(void* arg)
{
    (void)arg;
    video_init(4 /*cc_width*/, EMU_SMS /*machine*/, s_palette, 1 /*ntsc*/);
    s_vinit_done = true;
    vTaskDelete(NULL);
}

extern "C" void ili9341_init()
{
    if (s_started) return;

    // Allocate the single shared screen buffer in internal RAM (the IRAM video ISR reads it).
    // Done once at boot while the big internal region is intact; every emulator reuses it,
    // so there are no per-emulator framebuffer allocations and no fragmentation.
    g_emu_screen = (uint8_t*)heap_caps_malloc(SHARED_SZ, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!g_emu_screen) { printf("composite: FAILED to alloc %d-byte shared screen!\n", SHARED_SZ); return; }
    s_fb = g_emu_screen;
    memset(s_fb, 0, FB_W * FB_H);
    for (int y = 0; y < FB_H; y++)
        s_line_ptrs[y] = s_fb + y * FB_W;

    build_color_palette();

    // Hand the framebuffer to the driver BEFORE starting the pump (ISR early-outs on null).
    _lines = s_line_ptrs;

    // Start the A/V pump with its ISR pinned to core 1.
    s_vinit_done = false;
    xTaskCreatePinnedToCore(video_init_core1, "vinit", 4096, NULL, 10, NULL, 1);
    while (!s_vinit_done) vTaskDelay(pdMS_TO_TICKS(2));

    s_started = true;
    printf("composite: video pump started on core 1 (%dx%d color 3-3-2, from %dx%d)\n",
           FB_W, FB_H, SRC_W, SRC_H);
}

// Core blit: copy an RGB565 rectangle (launcher 320-wide coords) into the 256-wide
// luma framebuffer, downscaling X by 256/320. Both rectangle and rectangleLE map here
// (we read the native uint16 either way; the LCD byte-swap is irrelevant for composite).
static void blit_rect(short left, short top, short width, short height, const uint16_t* buf)
{
    if (!s_fb || !buf) return;
    for (int sy = 0; sy < height; sy++) {
        int fy = top + sy;
        if (fy < 0 || fy >= FB_H) continue;
        const uint16_t* srow = buf + (size_t)sy * width;
        uint8_t* drow = s_fb + (size_t)fy * FB_W;
        for (int sx = 0; sx < width; sx++) {
            int fx = ((left + sx) * FB_W) / SRC_W;   // 320 -> 256
            if (fx < 0 || fx >= FB_W) continue;
            drow[fx] = rgb565_to_index(srow[sx]);
        }
    }
}

extern "C" void ili9341_write_frame_rectangleLE(short left, short top, short width, short height, uint16_t* buffer)
{
    blit_rect(left, top, width, height, buffer);
}

extern "C" void ili9341_write_frame_rectangle(short left, short top, short width, short height, uint16_t* buffer)
{
    blit_rect(left, top, width, height, buffer);
}

extern "C" void ili9341_clear(uint16_t color)
{
    if (!s_fb) return;
    memset(s_fb, rgb565_to_index(color), FB_W * FB_H);
}

extern "C" void ili9341_prepare() { /* no-op on composite */ }
extern "C" void ili9341_poweroff() { /* no-op */ }

// ---- composite pump control (composite_video.h) ----
// Emulators borrow the running video pump by swapping the source line pointers,
// palette, and machine flavor (EMU_NES/EMU_SMS share cc_width=4 / 256px geometry,
// so no re-init is needed). The ISR reads these globals each scanline.
extern "C" void composite_use_nes(uint8_t** lines, const uint32_t* palette)
{
    _machine = EMU_NES;     // 6-bit (0x3F) palette index, 256 px
    _palette = palette;     // 64-entry NES composite palette
    _lines   = lines;       // nofrendo's 256x240 framebuffer line pointers
}

extern "C" void composite_use_sms(uint8_t** lines, const uint32_t* palette)
{
    _machine = EMU_SMS;     // full 8-bit palette index, 256 px
    _palette = palette;
    _lines   = lines;
}

extern "C" void composite_use_atari(uint8_t** lines, const uint32_t* palette)
{
    _machine = EMU_ATARI;   // 2 px/color-clock, 384-wide rows, shows center 336
    _palette = palette;     // 256-entry Atari composite palette
    _lines   = lines;       // 240 pointers into Screen_atari (each 384 px wide)
}

extern "C" void composite_use_launcher(void)
{
    s_fb = g_emu_screen;                                  // shared buffer, 256-wide view
    memset(s_fb, 0, FB_W * FB_H);
    for (int y = 0; y < FB_H; y++) s_line_ptrs[y] = s_fb + y * FB_W;
    _machine = EMU_SMS;     // full 8-bit palette index
    _palette = s_palette;   // launcher 3-3-2 color palette
    _lines   = s_line_ptrs; // launcher framebuffer
}

// Idle the ISR before an emulator repurposes the shared screen buffer with its own stride.
// (Nothing is freed - the buffer is static and shared; this just parks the pump cleanly.)
extern "C" void composite_release_launcher_fb(void)
{
    _lines = 0;                       // idle the ISR (early-outs on null)
    vTaskDelay(pdMS_TO_TICKS(20));    // let any in-flight ISR (captured old _lines) finish
}

extern "C" uint8_t* composite_shared_screen(void) { return g_emu_screen; }

extern "C" void composite_audio_write(const int16_t* samples, int count)
{
    audio_write_16(samples, count, 1);   // video_out.h: fills the per-scanline audio ring
}

// Samples currently queued in the audio ring (drained by the ISR at the fixed sample rate).
// Emulators pace on this instead of vsync: it has sub-frame granularity, so a frame that
// runs slightly over budget doesn't lose a whole vsync beat (which would halve the rate).
extern "C" int composite_audio_pending(void)
{
    return (int)(_audio_w - _audio_r);
}

// ---- composite color calibration (composite_video.h) ----
// The launcher's Video Calibration screen sets these live; all palette encoding
// (launcher 3-3-2 cube + NES/SMS dynamic palettes) goes through composite_encode_rgb,
// so one set of params calibrates every screen. Rebuild the launcher palette after a
// change so the carousel/test pattern recolors instantly (the framebuffer indices stay
// put; only the index->composite-word mapping changes).
extern "C" void composite_set_color_params(float chroma, float phase, float bright, float contrast)
{
    g_chroma   = chroma;
    g_phase    = phase;
    g_bright   = bright;
    g_contrast = contrast;
}

extern "C" void composite_get_color_params(float* chroma, float* phase, float* bright, float* contrast)
{
    if (chroma)   *chroma   = g_chroma;
    if (phase)    *phase    = g_phase;
    if (bright)   *bright   = g_bright;
    if (contrast) *contrast = g_contrast;
}

extern "C" void composite_rebuild_palette(void)
{
    build_color_palette();   // re-encode the launcher 3-3-2 cube with the current params
}

extern "C" void composite_wait_vsync(void)
{
    // Lock the caller (emulator loop) to the composite frame rate: wait until the
    // ISR's frame counter advances. Keeps each emulated frame in step with the
    // fixed 60Hz composite so a single-buffer tear stays stationary instead of
    // drifting into a visible "jump" every second or two.
    int start = _frame_counter;
    int guard = 0;
    while (_frame_counter == start && ++guard < 200)
        vTaskDelay(1);   // ~1ms; a composite frame is ~16.6ms
}

// ---- backlight: no backlight on a composite TV ----
extern "C" int  is_backlight_initialized()       { return 1; }
extern "C" void backlight_percentage_set(int v)  { (void)v; }

// ---- emulator frame paths (filled in when we wire NES); stubs keep the link clean ----
extern "C" void ili9341_write_frame_gb(uint16_t* b, int s) { (void)b; (void)s; }
extern "C" void ili9341_write_frame_nes(uint8_t* b, uint16_t* pal, uint8_t s) { (void)b; (void)pal; (void)s; }
extern "C" void ili9341_write_frame_sms(uint8_t* b, uint16_t pal[], uint8_t gg, uint8_t s) { (void)b; (void)pal; (void)gg; (void)s; }

// ---- misc display plumbing the API declares; harmless no-ops here ----
extern "C" void send_reset_drawing(int l, int t, int w, int h) { (void)l; (void)t; (void)w; (void)h; }
extern "C" void send_continue_wait() {}
extern "C" void send_continue_line(uint16_t* line, int width, int lineCount) { (void)line; (void)width; (void)lineCount; }
extern "C" void display_tasktonotify_set(int v) { (void)v; }
extern "C" void odroid_display_drain_spi() {}
extern "C" void odroid_display_lock_gb_display() {}
extern "C" void odroid_display_unlock_gb_display() {}
extern "C" void odroid_display_lock_nes_display() {}
extern "C" void odroid_display_unlock_nes_display() {}
extern "C" void odroid_display_lock_sms_display() {}
extern "C" void odroid_display_unlock_sms_display() {}
