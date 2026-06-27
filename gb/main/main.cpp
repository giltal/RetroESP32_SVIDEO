// RetroESP32_GB - standalone Game Boy / Game Boy Color app (composite video), launched via
// OTA from the SuperRetroPack launcher. The gnuboy core keeps GBC's 32KB WRAM + 16KB VRAM
// as static arrays (too big for the monolith), so it gets its own OTA slot with full RAM.
//
// gnuboy renders 160x144 RGB565; we convert to the 3-3-2 composite cube and center it in the
// 256x240 EMU_SMS composite frame. Reads ROM path + color calibration from NVS; exits to factory.

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

extern "C" {
#include "odroid_settings.h"
#include "odroid_system.h"
#include "odroid_input.h"
#include "odroid_sdcard.h"
}

#define EMU_ATARI 1
#define EMU_NES   2
#define EMU_SMS   3
#include "video_out.h"

extern "C" {
#include "gnuboy.h"
#include "loader.h"
#include "hw.h"
#include "lcd.h"
#include "fb.h"
#include "cpu.h"
#include "mem.h"
#include "sound.h"
#include "pcm.h"
#include "regs.h"
#include "rtc.h"
// app-provided globals the gnuboy core externs:
struct fb fb;
struct pcm pcm;
// The reference author coupled gnuboy to its app: these symbols live in the app, not the core.
int frame = 0;                              // lcd.c scanline counter (cosmetic)
uint16_t* displayBuffer[2] = { 0, 0 };      // legacy double-buffer ptrs (set to our fb below)
const char* SD_BASE_PATH = "/sd";           // loader.c rom path base
int g_lcd_render = 1;                        // per-frame render enable (CGB frame-skip; see gb_run_frame)
void odroid_display_lock_gb_display(void) {}    // no-op: single-task composite, no LCD lock
void odroid_display_unlock_gb_display(void) {}
void odroid_display_show_sderr(int n) { (void)n; }
}

#include "gb_font.h"   // const FONT_5x7[7][250] for the in-game menu

static void exit_to_launcher(void);
extern "C" void die(char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\ngnuboy die() -> launcher\n");
    exit_to_launcher();
}

#define GB_W 160
#define GB_H 144
#define FB_W 256
#define FB_H 240
#define GB_X ((FB_W - GB_W) / 2)     // 48 : center 160 in 256
#define GB_Y ((FB_H - GB_H) / 2)     // 48 : center 144 in 240

// ---- upscaling: blit the 160x144 GB frame larger so it fills the composite width ----
// The native image used a small centered portion of the screen; scale it up (nearest-neighbor).
// Set GB_SCALE 0 to revert to the native 160x144 centered layout (e.g. if it ever costs too much).
#define GB_SCALE 1
#if GB_SCALE
  #define DST_W 256                  // fill the full composite width (no side bars)
  #define DST_H 230                  // 1.6x -> preserves the GB aspect (~5px top/bottom border)
#else
  #define DST_W GB_W
  #define DST_H GB_H
#endif
#define DST_X ((FB_W - DST_W) / 2)
#define DST_Y ((FB_H - DST_H) / 2)
#define A_RATE   15720
#define A_FRAME  (A_RATE / 60)

// ---- composite color params from NVS (shared with the launcher calibration) ----
static float g_chroma = 25.0f, g_phase = -70.0f, g_bright = 0.0f, g_contrast = 1.0f;

static uint32_t composite_encode_rgb(int r, int g, int b) {
    const float lo = (float)BLACK_LEVEL, span = (float)(WHITE_LEVEL - BLACK_LEVEL);
    const float th = g_phase * (float)M_PI / 180.0f, ct = cosf(th), st = sinf(th);
    float Y = 0.299f*r + 0.587f*g + 0.114f*b;
    float U = -0.147407f*r - 0.289391f*g + 0.436798f*b;
    float V =  0.614777f*r - 0.514799f*g - 0.099978f*b;
    float Ur = U*ct - V*st, Vr = U*st + V*ct;
    float yv = (Y / 255.0f) * g_contrast + g_bright;
    if (yv < 0) yv = 0; if (yv > 1) yv = 1;
    float luma = lo + yv * span, P = -g_chroma * Ur, Q = g_chroma * Vr;
    float s[4] = { luma + P, luma + Q, luma - P, luma - Q };
    uint32_t word = 0;
    for (int j = 0; j < 4; j++) { float v = s[j]; if (v < 0) v = 0; if (v > 65280.0f) v = 65280.0f; word = (word << 8) | ((uint32_t)v >> 8); }
    return word;
}
static void load_calibration(void) {
    nvs_handle h;
    if (nvs_open("storage", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "VCHR2", &v) == ESP_OK) g_chroma   = v / 100.0f;
    if (nvs_get_i32(h, "VPHA2", &v) == ESP_OK) g_phase    = v / 100.0f;
    if (nvs_get_i32(h, "VBRI2", &v) == ESP_OK) g_bright   = v / 1000.0f;
    if (nvs_get_i32(h, "VCON2", &v) == ESP_OK) g_contrast = v / 1000.0f;
    nvs_close(h);
}

static uint32_t s_apal[256];                 // 3-3-2 composite cube
// Only the GB active area changes; the border is static black. So we double-buffer just the 144
// GB rows (256 wide) and point every border row at one shared black row -> ~72KB, not 2x60KB.
static uint8_t* s_fb = 0;                     // full 256x240 composite framebuffer (scaled GB image)
static uint8_t* s_lines[2][FB_H];            // row-pointer arrays (only [0] used); 1:1 into s_fb
static uint8_t* s_gbfb = 0;                   // 160x144, gnuboy now renders 3-3-2 cube indices here
static uint16_t s_xmap[FB_W];                 // dest-x -> source-x (nearest-neighbor h-scale)

static void build_palette(void) {
    for (int i = 0; i < 256; i++) {
        int r = ((i >> 5) & 7) * 255 / 7, g = ((i >> 2) & 7) * 255 / 7, b = (i & 3) * 255 / 3;
        s_apal[i] = composite_encode_rgb(r, g, b);
    }
}
// Upscale gnuboy's 160x144 cube-index frame into the full composite framebuffer (nearest-neighbor,
// DST_WxDST_H; see GB_SCALE). gnuboy already wrote final cube indices, so this is a pure copy - no
// conversion. H-scale each distinct source row once; memcpy the rows that v-scale duplicates.
static void gb_blit(void) {
    int prev_sy = -1;
    uint8_t* prev_dst = 0;
    for (int dy = 0; dy < DST_H; dy++) {
        int sy = (dy * GB_H) / DST_H;                          // nearest-neighbor v-scale
        uint8_t* dst = s_fb + (DST_Y + dy) * FB_W + DST_X;
        if (sy == prev_sy) {
            memcpy(dst, prev_dst, DST_W);
        } else {
            const uint8_t* src = s_gbfb + sy * GB_W;
            for (int dx = 0; dx < DST_W; dx++) dst[dx] = src[s_xmap[dx]];
            prev_sy = sy; prev_dst = dst;
        }
    }
}

// ---- in-game menu (X button), drawn into the 8-bit composite framebuffer over the paused frame ----
// GB uses the full 3-3-2 cube, so the menu paints with direct cube indices (no reserved slots needed):
// 0x00 = black, 0xFF = white (R7 G7 B3), 0x2A = blue-ish highlight. Row pointers are 1:1 into the
// full framebuffer, so the menu draws at display coords directly over the paused (scaled) frame.
#define GB_MBG 0x00
#define GB_MFG 0xFF
#define GB_MHL 0x2A

static int gb_glyph(char c) {   // -> column offset into FONT_5x7, or -1 for space/unknown
    static const char up[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!-'&?.,/()[] ";
    if (c >= 'a' && c <= 'z') c -= 32;
    for (int n = 0; up[n]; n++) if (c == up[n]) return (c == ' ') ? -1 : n * 5;
    return -1;
}
static void gb_text(uint8_t** L, int x, int y, const char* s, uint8_t fg) {
    for (; *s; s++) {
        int dx = gb_glyph(*s);
        if (dx >= 0) {
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (FONT_5x7[r][dx + c] != 0 && (y + r) < FB_H && (x + c) < FB_W)
                        L[y + r][x + c] = fg;
            x += 6;
        } else x += 4;
    }
}
static void gb_fill(uint8_t** L, int x, int y, int w, int h, uint8_t idx) {
    for (int r = 0; r < h; r++) { int yy = y + r; if (yy < 0 || yy >= FB_H) continue;
        for (int c = 0; c < w; c++) { int xx = x + c; if (xx >= 0 && xx < FB_W) L[yy][xx] = idx; } }
}
static void gb_menu_wait_release(void) {
    odroid_gamepad_state gp;
    for (int i = 0; i < 60; i++) {
        odroid_input_poll(&gp);
        if (!odroid_input_x_held() && !gp.values[ODROID_INPUT_A] &&
            !gp.values[ODROID_INPUT_UP] && !gp.values[ODROID_INPUT_DOWN] &&
            !gp.values[ODROID_INPUT_B]) return;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
static void gb_menu_flash(uint8_t** L, const char* msg) {   // brief centered message
    int w = (int)strlen(msg) * 6;
    int bx = FB_W/2 - w/2 - 8, by = FB_H/2 - 9;
    gb_fill(L, bx, by, w + 16, 18, GB_MBG);
    gb_fill(L, bx, by, w + 16, 2, GB_MFG);
    gb_text(L, bx + 8, by + 6, msg, GB_MFG);
    vTaskDelay(pdMS_TO_TICKS(800));
}
// Returns 0=continue, 1=save, 2=load, 3=reset, 4=quit.
static int gb_menu(uint8_t** L) {
    static const char* items[] = { "CONTINUE", "SAVE STATE", "LOAD STATE", "RESET", "QUIT TO MENU" };
    const int N = 5;
    int sel = 0, redraw = 1;
    const int bw = 112, bh = 24 + N * 14;
    const int bx = (FB_W - bw) / 2, by = (FB_H - bh) / 2;
    gb_menu_wait_release();
    for (;;) {
        if (redraw) {
            gb_fill(L, bx, by, bw, bh, GB_MBG);
            gb_fill(L, bx, by, bw, 2, GB_MFG);
            gb_text(L, bx + 8, by + 6, "PAUSED", GB_MFG);
            for (int i = 0; i < N; i++) {
                int iy = by + 24 + i * 14;
                if (i == sel) gb_fill(L, bx + 4, iy - 2, bw - 8, 11, GB_MHL);
                gb_text(L, bx + 10, iy, items[i], GB_MFG);
            }
            redraw = 0;
        }
        odroid_gamepad_state gp;
        odroid_input_poll(&gp);
        if (gp.values[ODROID_INPUT_UP])        { sel = (sel + N - 1) % N; redraw = 1; gb_menu_wait_release(); }
        else if (gp.values[ODROID_INPUT_DOWN]) { sel = (sel + 1) % N;     redraw = 1; gb_menu_wait_release(); }
        else if (gp.values[ODROID_INPUT_A])    { gb_menu_wait_release(); return sel; }
        else if (gp.values[ODROID_INPUT_B] || odroid_input_x_held()) { gb_menu_wait_release(); return 0; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---- save state: a file next to the ROM, extension ".sta" (gnuboy savestate/loadstate) ----
static char s_rompath[300];
static void gb_state_path(char* out, int n) {
    strncpy(out, s_rompath, n - 1); out[n - 1] = 0;
    char* dot = strrchr(out, '.');
    if (dot) strcpy(dot, ".sta");
    else { int l = (int)strlen(out); if (l < n - 5) strcpy(out + l, ".sta"); }
}
static bool gb_state_save(void) {
    if (!s_rompath[0]) return false;
    char p[300]; gb_state_path(p, sizeof(p));
    FILE* f = fopen(p, "wb");
    if (!f) { printf("gb_state_save: fopen failed: %s\n", p); return false; }
    savestate(f);
    fclose(f);
    return true;
}
static bool gb_state_load(void) {
    if (!s_rompath[0]) return false;
    char p[300]; gb_state_path(p, sizeof(p));
    FILE* f = fopen(p, "rb");
    if (!f) { printf("gb_state_load: no save file: %s\n", p); return false; }
    loadstate(f);
    fclose(f);
    vram_dirty(); pal_dirty(); sound_dirty(); mem_updatemap();
    return true;
}

// gnuboy audio sink: the core calls this when pcm.buf fills (we keep pcm.len large and flush
// once per frame instead, so this is just a safety drain).
extern "C" int pcm_submit(void) {
    if (pcm.pos > 0) { audio_write_16(pcm.buf, pcm.pos / 2, 2); pcm.pos = 0; }
    return 1;
}

// One emulated frame (from the reference run_to_vblank, minus its LCD/audio queue plumbing).
static void IRAM_ATTR gb_run_frame(void) {
    lcd_begin();   // reset vdest=fb.ptr for THIS frame; without it vdest runs off s_gbfb after
                   // frame 1 (-> static frame 1 + memory corruption). Reference does this in vid_begin().
    cpu_emulate(2280);
    while (R_LY > 0 && R_LY < 144) emu_step();
    rtc_tick();
    if (!(R_LCDC & 0x80)) cpu_emulate(32832);
    while (R_LY > 0) emu_step();
}

static volatile bool s_vinit_done = false;
static void video_init_core1(void* arg) {
    (void)arg;
    video_init(4 /*cc_width*/, EMU_SMS, s_apal, 1 /*ntsc*/);
    s_vinit_done = true;
    vTaskDelete(NULL);
}
static void exit_to_launcher(void) {
    const esp_partition_t* factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) esp_ota_set_boot_partition(factory);
    esp_restart();
}

extern "C" void app_main(void) {
    printf("\n----- RetroESP32_GB -----\n");
    nvs_flash_init();
    odroid_system_init();
    odroid_input_gamepad_init();

    // Boot fail-safe: hold HOME/MENU during boot to bail back to the launcher. This escapes a
    // crashing app's OTA boot-loop (otadata still points here) WITHOUT reflashing. Runs before
    // any risky init so it works even if later code aborts. Mirrors the launcher's SAFE MODE.
    vTaskDelay(pdMS_TO_TICKS(400));   // let pull-ups settle + CH559 decode one HID frame
    {
        odroid_gamepad_state st;
        odroid_input_poll(&st);
        if (st.values[ODROID_INPUT_MENU]) { printf("gb: HOME held at boot -> launcher\n"); exit_to_launcher(); }
    }

    load_calibration();   // NVS only; gnuboy's loader_init opens the SD card itself

    // One full 256x240 composite framebuffer; the (scaled) GB image is blitted into it, borders
    // stay black. Row pointers are 1:1 into it, so the in-game menu can draw at display coords.
    s_fb   = (uint8_t*)heap_caps_malloc(FB_W * FB_H, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s_gbfb = (uint8_t*)heap_caps_malloc(GB_W * GB_H, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!s_fb || !s_gbfb) {
        printf("gb: framebuffer alloc failed -> launcher\n"); exit_to_launcher();
    }
    memset(s_fb, 0, FB_W * FB_H);                              // black borders (game area overwritten each frame)
    for (int dx = 0; dx < DST_W; dx++) s_xmap[dx] = (uint16_t)((dx * GB_W) / DST_W);
    for (int y = 0; y < FB_H; y++) s_lines[0][y] = s_fb + y * FB_W;
    build_palette();

    // gnuboy framebuffer descriptor: 160x144, 1 byte/pixel (gnuboy writes 3-3-2 cube indices).
    fb.w = GB_W; fb.h = GB_H; fb.pelsize = 1; fb.pitch = GB_W;
    fb.indexed = 0; fb.ptr = (byte*)s_gbfb; fb.enabled = 1; fb.dirty = 0;
    displayBuffer[0] = displayBuffer[1] = (uint16_t*)s_gbfb;   // legacy ptrs (unused) alias our fb

    // gnuboy audio.
    memset(&pcm, 0, sizeof(pcm));
    pcm.hz = A_RATE; pcm.stereo = 1; pcm.len = 2048;
    pcm.buf = (int16_t*)heap_caps_malloc(pcm.len * sizeof(int16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    pcm.pos = 0;

    loader_init(NULL);   // reads ROM path from NVS (rom_load -> odroid_settings_RomFilePath_get)
    { char* rp = odroid_settings_RomFilePath_get();   // remember it for the .sta save-state filename
      if (rp) { strncpy(s_rompath, rp, sizeof(s_rompath) - 1); s_rompath[sizeof(s_rompath) - 1] = 0; free(rp); } }
    { extern void rom_preload_all(void); rom_preload_all(); }   // all banks SD->PSRAM now (no SD
                         // reads during gameplay -> no 15ms core-park that rolls the picture).
                         // Runs before the video pump starts, so its SD stalls aren't visible.
    emu_reset();

    sound_reset();
    lcd_begin();

    // Start the composite pump on core 1. _lines starts on buffer 0 (front); s_back=1.
    _lines = s_lines[0];
    s_vinit_done = false;
    xTaskCreatePinnedToCore(video_init_core1, "vinit", 4096, NULL, 10, NULL, 1);
    while (!s_vinit_done) vTaskDelay(pdMS_TO_TICKS(2));
    _lines = s_lines[0];

    vTaskPrioritySet(NULL, 8);

    unsigned vf = 0;
    int x_prev = 0;

    for (;;) {
        odroid_gamepad_state gp;
        odroid_input_poll(&gp);
        if (gp.values[ODROID_INPUT_MENU]) exit_to_launcher();   // HOME still quick-exits

        // X (rising edge) opens the paused in-game menu over the frozen frame, like SMS/NES.
        int x_now = odroid_input_x_held();
        if (x_now && !x_prev) {
            pcm_submit();                       // flush pending audio before pausing
            switch (gb_menu(s_lines[0])) {
                case 1: gb_menu_flash(s_lines[0], gb_state_save() ? "SAVED" : "SAVE FAILED"); break;
                case 2: gb_menu_flash(s_lines[0], gb_state_load() ? "LOADED" : "NO SAVE"); break;
                case 3: emu_reset(); break;
                case 4: exit_to_launcher(); break;
                default: break;
            }
            x_prev = odroid_input_x_held();
            continue;                            // skip this frame; next loop redraws the game
        }
        x_prev = x_now;

        pad_set(PAD_UP,     gp.values[ODROID_INPUT_UP]);
        pad_set(PAD_DOWN,   gp.values[ODROID_INPUT_DOWN]);
        pad_set(PAD_LEFT,   gp.values[ODROID_INPUT_LEFT]);
        pad_set(PAD_RIGHT,  gp.values[ODROID_INPUT_RIGHT]);
        pad_set(PAD_A,      gp.values[ODROID_INPUT_A]);
        pad_set(PAD_B,      gp.values[ODROID_INPUT_B]);
        pad_set(PAD_START,  gp.values[ODROID_INPUT_START]);
        pad_set(PAD_SELECT, gp.values[ODROID_INPUT_SELECT]);

        if (hw.cgb) {
            // (CGB branch - not used in the force-DMG test below, but kept functional)
            g_lcd_render = (++vf & 1u) == 0;
            gb_run_frame();
            pcm_submit();
            if (g_lcd_render) gb_blit();
            while ((int)(_audio_w - _audio_r) > A_FRAME * 4) vTaskDelay(1);
        } else {
            // DMG: vsync-paced 60fps, single buffer + direct _lines (Atari-style).
            g_lcd_render = 1;
            int fc = _frame_counter, guard = 0;
            while (_frame_counter == fc && ++guard < 200) vTaskDelay(1);
            gb_run_frame();
            gb_blit();
            pcm_submit();
        }
    }
}
