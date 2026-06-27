// sms_run.cpp - composite Sega Master System driver (RetroESP32 smsplus core).
//
// Like nes_run.cpp but for the smsplus core, with two SMS-specific wrinkles:
//   * the palette is DYNAMIC (SMS CRAM) - rebuilt into a composite palette each frame
//   * the screen is 192 lines - centered in the 240-line composite active area
// Launched in-process from the launcher; Home/MENU reboots back to it.

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

extern "C" {
#include "composite_video.h"
#include "odroid_input.h"
#include "shared.h"          // smsplus core: bitmap, sms, snd, input, cart, option, system_*

// smsplus save-state (state.c). NOTE: despite the void* name these take a FILE*.
int  system_save_state(void *mem);
void system_load_state(void *mem);
// launcher font (main.c, C linkage): 5x7 glyphs [row][char*5 + col], 0 = bg
extern const uint16_t FONT_5x7[7][250];
// Game Gear on-the-fly upscale (render.c): scale the 160x144 window to full width directly in
// the line copy. Enabled for GG only; SMS keeps the native path.
void gg_set_scale(uint8 *fb, int enable);
}

#define GG_SCALE 1   // 1 = upscale Game Gear to fill the screen; 0 = native (small) GG

// Cart SRAM management hook the core calls; battery saves not wired yet.
extern "C" void system_manage_sram(uint8 *sram, int slot, int mode) { (void)sram; (void)slot; (void)mode; }

#define SMS_W       256
#define SMS_H       192
#define FB_H        240
#define SMS_TOP     ((FB_H - SMS_H) / 2)   // center 192 lines in 240 -> 24
#define BORDER_IDX  0xFF                    // palette slot forced to black for the border
#define SMS_AUDIO_RATE 15720               // matches the composite per-scanline drain rate
#define SMS_AUDIO_FRAME (SMS_AUDIO_RATE / 60)  // ~262 samples per emulated frame

static uint8_t* s_fb = 0;                   // 256x240, SMS renders into the centered rows
static uint8_t* s_lines[FB_H];
static uint32_t s_pal[256];
static int16_t  s_abuf[512];

// Rebuild the 256-entry composite palette from the (dynamic) SMS CRAM - but only when
// CRAM actually changed. The composite encode uses float trig, so re-running it for all
// 32 colors every frame (when most frames don't touch CRAM) wastes time we need for the
// emulation/audio. Cache the last RGB565 set and early-out on no change.
static void sms_build_palette(void) {
    static uint16 last_rgb[PALETTE_SIZE];
    static bool   have_last = false;
    uint16 rgb[PALETTE_SIZE];
    render_copy_palette(rgb);
    if (have_last && memcmp(rgb, last_rgb, sizeof(rgb)) == 0) return;
    memcpy(last_rgb, rgb, sizeof(rgb));
    have_last = true;

    uint32_t uniq[PALETTE_SIZE];
    for (int i = 0; i < PALETTE_SIZE; i++) {
        int r = ((rgb[i] >> 11) & 0x1F) * 255 / 31;   // native RGB565 (render.c no longer byte-swaps)
        int g = ((rgb[i] >> 5)  & 0x3F) * 255 / 63;
        int b = ( rgb[i]        & 0x1F) * 255 / 31;
        uniq[i] = composite_encode_rgb(r, g, b);
    }
    for (int v = 0; v < 256; v++) s_pal[v] = uniq[v & (PALETTE_SIZE - 1)];
    s_pal[BORDER_IDX] = composite_encode_rgb(0, 0, 0);
}

static void sms_pump_audio(void) {
    int n = snd.sample_count;
    if (n <= 0) return;
    if (n > (int)(sizeof(s_abuf) / sizeof(s_abuf[0]))) n = sizeof(s_abuf) / sizeof(s_abuf[0]);
    for (int i = 0; i < n; i++)
        s_abuf[i] = (int16_t)(((int)snd.output[0][i] + (int)snd.output[1][i]) >> 1);
    composite_audio_write(s_abuf, n);
}

// ---- in-game menu, drawn into the 8-bit SMS framebuffer over the paused frame ----
// The game only uses palette indices 0..PIXEL_MASK (0x1F), so high indices are free for
// the menu; we point them at fixed black/white/highlight colors when the menu opens.
#define SMS_MBG 0xFD
#define SMS_MFG 0xFC
#define SMS_MHL 0xFE

static int sms_glyph(char c) {   // -> column offset into FONT_5x7, or -1 for space
    static const char up[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!-'&?.,/()[] ";
    if (c >= 'a' && c <= 'z') c -= 32;
    for (int n = 0; up[n]; n++) if (c == up[n]) return (c == ' ') ? -1 : n * 5;
    return -1;
}
static void sms_text(uint8_t** L, int x, int y, const char* s, uint8_t fg) {
    for (; *s; s++) {
        int dx = sms_glyph(*s);
        if (dx >= 0) {
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (FONT_5x7[r][dx + c] != 0 && (y + r) < FB_H && (x + c) < SMS_W)
                        L[y + r][x + c] = fg;
            x += 6;
        } else x += 4;
    }
}
static void sms_fill(uint8_t** L, int x, int y, int w, int h, uint8_t idx) {
    for (int r = 0; r < h; r++) { int yy = y + r; if (yy < 0 || yy >= FB_H) continue;
        for (int c = 0; c < w; c++) { int xx = x + c; if (xx >= 0 && xx < SMS_W) L[yy][xx] = idx; } }
}
static void sms_menu_colors(void) {     // reserve high palette slots for fixed menu colors
    s_pal[SMS_MBG] = composite_encode_rgb(0, 0, 0);
    s_pal[SMS_MFG] = composite_encode_rgb(255, 255, 255);
    s_pal[SMS_MHL] = composite_encode_rgb(40, 90, 230);
}
static void sms_menu_wait_release(void) {
    odroid_gamepad_state gp;
    for (int i = 0; i < 60; i++) {
        odroid_input_poll(&gp);
        if (!odroid_input_x_held() && !gp.values[ODROID_INPUT_A] &&
            !gp.values[ODROID_INPUT_UP] && !gp.values[ODROID_INPUT_DOWN] &&
            !gp.values[ODROID_INPUT_B]) return;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
static void sms_menu_flash(uint8_t** L, const char* msg) {   // brief centered message
    sms_menu_colors();
    int w = (int)strlen(msg) * 6;
    int bx = SMS_W/2 - w/2 - 8, by = 112;
    sms_fill(L, bx, by, w + 16, 18, SMS_MBG);
    sms_fill(L, bx, by, w + 16, 2, SMS_MFG);
    sms_text(L, bx + 8, by + 6, msg, SMS_MFG);
    vTaskDelay(pdMS_TO_TICKS(800));
}
// Returns 0=continue, 1=save, 2=load, 3=reset, 4=quit.
static int sms_menu(uint8_t** L) {
    static const char* items[] = { "CONTINUE", "SAVE STATE", "LOAD STATE", "RESET", "QUIT TO MENU" };
    const int N = 5;
    int sel = 0, redraw = 1;
    const int bx = (SMS_W - 112) / 2, by = 70, bw = 112, bh = 24 + N * 14;
    sms_menu_colors();
    sms_menu_wait_release();
    for (;;) {
        if (redraw) {
            sms_fill(L, bx, by, bw, bh, SMS_MBG);
            sms_fill(L, bx, by, bw, 2, SMS_MFG);
            sms_text(L, bx + 8, by + 6, "PAUSED", SMS_MFG);
            for (int i = 0; i < N; i++) {
                int iy = by + 24 + i * 14;
                if (i == sel) sms_fill(L, bx + 4, iy - 2, bw - 8, 11, SMS_MHL);
                sms_text(L, bx + 10, iy, items[i], SMS_MFG);
            }
            redraw = 0;
        }
        odroid_gamepad_state gp;
        odroid_input_poll(&gp);
        if (gp.values[ODROID_INPUT_UP])        { sel = (sel + N - 1) % N; redraw = 1; sms_menu_wait_release(); }
        else if (gp.values[ODROID_INPUT_DOWN]) { sel = (sel + 1) % N;     redraw = 1; sms_menu_wait_release(); }
        else if (gp.values[ODROID_INPUT_A])    { sms_menu_wait_release(); return sel; }
        else if (gp.values[ODROID_INPUT_B] || odroid_input_x_held()) { sms_menu_wait_release(); return 0; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---- save state: a file next to the ROM, extension ".sta" ----
static char s_rompath[300];
static void sms_state_path(const char* rom, char* out, int n) {
    strncpy(out, rom, n - 1); out[n - 1] = 0;
    char* dot = strrchr(out, '.');
    if (dot) strcpy(dot, ".sta");
    else { int l = (int)strlen(out); if (l < n - 5) strcpy(out + l, ".sta"); }
}
static bool sms_state_save(void) {
    char p[300]; sms_state_path(s_rompath, p, sizeof(p));
    FILE* f = fopen(p, "wb");
    if (!f) { printf("sms_state_save: fopen failed: %s\n", p); return false; }
    system_save_state(f);
    fclose(f);
    return true;
}
static bool sms_state_load(void) {
    char p[300]; sms_state_path(s_rompath, p, sizeof(p));
    FILE* f = fopen(p, "rb");
    if (!f) { printf("sms_state_load: no save file: %s\n", p); return false; }
    system_load_state(f);
    fclose(f);
    return true;
}

extern "C" void sms_run(const char* path) {
    printf("sms_run: %s\n", path);
    strncpy(s_rompath, path, sizeof(s_rompath) - 1);   // for save-state filename
    s_rompath[sizeof(s_rompath) - 1] = 0;

    // Load the ROM (into PSRAM) while the launcher screen is still displayed, so the video
    // pump keeps a valid framebuffer through the slow SD read instead of idling on NULL.
    set_option_defaults();
    option.sndrate  = SMS_AUDIO_RATE;
    option.overscan = 0;
    option.extra_gg = 0;
    sms.use_fm = 0;

    if (!load_rom((char*)path)) { printf("sms_run: load_rom failed -> reboot\n"); esp_restart(); }

    // The smsplus core reads port $3E bit6 as "enable BIOS, disable cart". Many SMS
    // games write 0xE0 there, which repoints the Z80 read map at bios.rom - NULL on a
    // cart-only system (no BIOS image), so the Z80 reads NULL+addr and faults. Alias the
    // BIOS to the cart ROM and disarm BIOS autodetect so that path stays valid.
    bios.rom     = cart.rom;
    bios.pages   = cart.pages;
    bios.enabled = 2;

    // Use the shared static screen buffer (256-wide view) - no per-emulator alloc.
    composite_release_launcher_fb();                 // idle the pump for the handoff
    s_fb = composite_shared_screen();
    memset(s_fb, BORDER_IDX, SMS_W * FB_H);          // black border
    for (int y = 0; y < FB_H; y++) s_lines[y] = s_fb + y * SMS_W;

    bitmap.width  = SMS_W;
    bitmap.height = SMS_H;
    bitmap.pitch  = SMS_W;
    bitmap.data   = s_fb + SMS_TOP * SMS_W;          // render into the centered rows

    system_init2();
    system_reset();

    // Game Gear: scale the 160x144 window up to fill the screen, on the fly in the core's line
    // copy (directly into s_fb). SMS is left at native size. s_fb borders stay black (memset above).
    gg_set_scale(s_fb, (IS_GG && GG_SCALE) ? 1 : 0);

    sms_build_palette();
    composite_use_sms(s_lines, s_pal);

    // Run emulation above the CH559 input task (prio 6) so it isn't preempted mid-frame.
    vTaskPrioritySet(NULL, 8);

    // Prime the audio ring (~4 frames) so brief jitter doesn't underflow it.
    for (int p = 0; p < 4; p++) { system_frame(0); sms_pump_audio(); }

    int last_x = 0;
    for (;;) {
        odroid_gamepad_state gp;
        odroid_input_poll(&gp);
        if (gp.values[ODROID_INPUT_MENU]) esp_restart();   // Home = quick exit to launcher

        // X (rising edge) opens the in-game menu over the paused frame, like NES.
        int xnow = odroid_input_x_held();
        if (xnow && !last_x) {
            switch (sms_menu(s_lines)) {
                case 1: sms_menu_flash(s_lines, sms_state_save() ? "SAVED" : "SAVE FAILED"); break;
                case 2: sms_state_load(); break;             // restored state renders next frames
                case 3: system_reset(); break;
                case 4: esp_restart();                       // QUIT to launcher
                default: break;                              // 0 = continue
            }
            // The ring drained while paused; re-prime + render a few frames to refill audio
            // and paint over the menu overlay before resuming normal pacing.
            for (int p = 0; p < 4; p++) { system_frame(0); sms_pump_audio(); }
            sms_build_palette();
            last_x = odroid_input_x_held();
            continue;
        }
        last_x = xnow;

        input.pad[0] = 0;
        input.system = 0;
        if (gp.values[ODROID_INPUT_UP])     input.pad[0] |= INPUT_UP;
        if (gp.values[ODROID_INPUT_DOWN])   input.pad[0] |= INPUT_DOWN;
        if (gp.values[ODROID_INPUT_LEFT])   input.pad[0] |= INPUT_LEFT;
        if (gp.values[ODROID_INPUT_RIGHT])  input.pad[0] |= INPUT_RIGHT;
        if (gp.values[ODROID_INPUT_A])      input.pad[0] |= INPUT_BUTTON2;
        if (gp.values[ODROID_INPUT_B])      input.pad[0] |= INPUT_BUTTON1;
        if (gp.values[ODROID_INPUT_START])  input.system |= INPUT_PAUSE;   // SMS Pause
        if (gp.values[ODROID_INPUT_SELECT]) input.system |= INPUT_START;   // SMS Start/menu

        // Pace to the audio drain rate (keep ~4 frames buffered) rather than hard vsync:
        // a frame that runs over budget degrades smoothly instead of halving, and the
        // ~4-frame cushion rides out the occasional 30ms+ emulated frame without underrun.
        while (composite_audio_pending() > SMS_AUDIO_FRAME * 4) vTaskDelay(1);

        // Adaptive frame-skip: when the audio ring is draining faster than we can render
        // (heavy scene), skip the VDP render so the Z80 + sound still run near 60fps -
        // trade dropped video frames for smooth audio. Allow up to 3 skips in a row, which
        // keeps audio cadence in the busiest scenes while still drawing a real frame at
        // least every 4th (>=15fps video) in the worst case.
        static int skipped = 0;
        int skip = (skipped < 3 && composite_audio_pending() < SMS_AUDIO_FRAME * 3) ? 1 : 0;
        skipped = skip ? skipped + 1 : 0;

        system_frame(skip);
        if (!skip) sms_build_palette();      // _palette already points at s_pal; contents update in place
        sms_pump_audio();

        // Idle/watchdog safety: the pace loop above only yields when we're AHEAD of the
        // audio; if we run flat-out behind schedule it never yields and IDLE0 (core 0)
        // starves -> task watchdog. Force a short yield periodically so IDLE still runs.
        static int ytick = 0;
        if (++ytick >= 12) { ytick = 0; vTaskDelay(1); }
    }
}
