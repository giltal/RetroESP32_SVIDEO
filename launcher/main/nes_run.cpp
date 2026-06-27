// nes_run.cpp - composite NES driver for RetroESP32_SVIDEO.
//
// Drives the nofrendo core (components/nofrendo, composite-native osd.c) frame-at-
// a-time and feeds its 256x240 8-bit framebuffer into our composite video pump with
// the NES composite palette. Launched in-process from the launcher (no OTA/reboot);
// returns to the launcher when the player presses MENU/Home.
//
// osd.c already provides most osd_* hooks (video driver, nes_emulate_init/frame,
// osd_getinput via _input_mask). Here we add the few it leaves to the front-end:
// the ROM data source, the sound info/callback, and the NES composite palette.

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
#include "odroid_sdcard.h"

// from nofrendo's osd.c / nes.c
int      nes_emulate_init(const char* path, int width, int height);
uint8_t** nes_emulate_frame(bool draw_flag);
void     nes_reset(int reset_type);        // 1 = HARD_RESET
int      state_save(void);                 // nofrendo SNSS save-state (current slot)
int      state_load(void);
void     state_setslot(int slot);
extern uint8_t* nofrendo_extfb;            // vid_drv.c: if set, NES renders into this buffer
extern int _input_mask;                    // osd.c input bitmask (osd_getinput reads it)
// launcher font (main.c, C linkage): 5x7 glyphs, [row][char*5 + col], 0 = bg
extern const uint16_t FONT_5x7[7][250];
}

// nofrendo's sound-info struct (mirror of nofrendo/osd.h's sndinfo_s; declared
// locally to avoid pulling the C-only nofrendo headers into this C++ TU).
typedef struct { int sample_rate; int bps; } sndinfo_t;

// ---- NES composite palette (4 samples/color-clock), from ESP_8_BIT make_nes_palette.
// 64 entries (EMU_NES blit masks the index to 0x3F). Packed p0<<24|p1<<16|p2<<8|p3.
extern "C" uint32_t nes_4_phase[64] = {
    0x2C2C2C2C,0x241D1F26,0x221D2227,0x1F1D2426,0x1D1F2624,0x1D222722,0x1D24261F,0x1F26241D,
    0x2227221D,0x24261F1D,0x26241D1F,0x27221D22,0x261F1D24,0x14141414,0x14141414,0x14141414,
    0x38383838,0x2C25272E,0x2A252A2F,0x27252C2E,0x25272E2C,0x252A2F2A,0x252C2E27,0x272E2C25,
    0x2A2F2A25,0x2C2E2725,0x2E2C2527,0x2F2A252A,0x2E27252C,0x1F1F1F1F,0x15151515,0x15151515,
    0x45454545,0x3A33353C,0x3732373C,0x35333A3C,0x33353C3A,0x32373C37,0x333A3C35,0x353C3A33,
    0x373C3732,0x3A3C3533,0x3C3A3335,0x3C373237,0x3C35333A,0x2B2B2B2B,0x16161616,0x16161616,
    0x45454545,0x423B3D44,0x403B4045,0x3D3B4244,0x3B3D4442,0x3B404540,0x3B42443D,0x3D44423B,
    0x4045403B,0x42443D3B,0x44423B3D,0x45403B40,0x443D3B42,0x39393939,0x17171717,0x17171717,
};

// ---- OSD hooks the front-end must supply ----
static uint8_t* s_rom = 0;
static void   (*s_snd_cb)(void* buffer, int length) = 0;

extern "C" char* osd_getromdata(void) { return (char*)s_rom; }
extern "C" void  osd_setsound(void (*playfunc)(void* buffer, int length)) { s_snd_cb = playfunc; }
// The video ISR plays one audio sample per scanline (262 lines/frame * ~60fps),
// so the APU runs at that rate and we feed exactly 262 samples per frame.
#define NES_AUDIO_RATE   15720
#define NES_AUDIO_FRAME  262
extern "C" void  osd_getsoundinfo(sndinfo_t* info) { info->sample_rate = NES_AUDIO_RATE; info->bps = 8; }

// Render one frame of APU audio (8-bit unsigned -> 16-bit signed) into the LEDC ring.
static void nes_pump_audio(void) {
    if (!s_snd_cb) return;
    int16_t ab[NES_AUDIO_FRAME + 16];
    s_snd_cb(ab, NES_AUDIO_FRAME);
    uint8_t* b8 = (uint8_t*)ab;
    for (int j = NES_AUDIO_FRAME - 1; j >= 0; j--)
        ab[j] = (int16_t)((b8[j] ^ 0x80) << 8);
    composite_audio_write(ab, NES_AUDIO_FRAME);
}

#define NES_ROM_MAX (1024 * 1024)

// ---- in-game menu, drawn directly into the 8-bit NES framebuffer ----
// (the launcher's RGB renderer/framebuffer is freed during a game, so the menu is
//  composed from NES palette indices over the paused frame).
// nes_4_phase indices: 0x0F=black, 0x30=white, 0x11=blue, 0x16=red.
#define MENU_BG 0x0F
#define MENU_FG 0x30
#define MENU_HL 0x11

static int menu_glyph(char c) {   // -> column offset into FONT_5x7, or -1 for space
    static const char up[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!-'&?.,/()[] ";
    if (c >= 'a' && c <= 'z') c -= 32;
    for (int n = 0; up[n]; n++) if (c == up[n]) return (c == ' ') ? -1 : n * 5;
    return -1;
}
static void nes_text(uint8_t** L, int x, int y, const char* s, uint8_t fg) {
    for (; *s; s++) {
        int dx = menu_glyph(*s);
        if (dx >= 0) {
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (FONT_5x7[r][dx + c] != 0 && (y + r) < 240 && (x + c) < 256)
                        L[y + r][x + c] = fg;
            x += 6;
        } else x += 4;
    }
}
static void nes_fill(uint8_t** L, int x, int y, int w, int h, uint8_t idx) {
    for (int r = 0; r < h; r++) { int yy = y + r; if (yy < 0 || yy >= 240) continue;
        for (int c = 0; c < w; c++) { int xx = x + c; if (xx >= 0 && xx < 256) L[yy][xx] = idx; } }
}
static void menu_wait_release(void) {
    odroid_gamepad_state gp;
    for (int i = 0; i < 60; i++) {
        odroid_input_poll(&gp);
        if (!odroid_input_x_held() && !gp.values[ODROID_INPUT_A] &&
            !gp.values[ODROID_INPUT_UP] && !gp.values[ODROID_INPUT_DOWN] &&
            !gp.values[ODROID_INPUT_B]) return;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
// Brief centered message over the frame (e.g. "SAVED"), for ~0.8s.
static void menu_flash(uint8_t** L, const char* msg) {
    int w = (int)strlen(msg) * 6;
    int bx = 128 - w/2 - 8, by = 112;
    nes_fill(L, bx, by, w + 16, 18, MENU_BG);
    nes_fill(L, bx, by, w + 16, 2, MENU_FG);
    nes_text(L, bx + 8, by + 6, msg, MENU_FG);
    vTaskDelay(pdMS_TO_TICKS(800));
}

// Returns 0=continue, 1=save, 2=load, 3=reset, 4=quit.
static int nes_menu(uint8_t** L) {
    static const char* items[] = { "CONTINUE", "SAVE STATE", "LOAD STATE", "RESET", "QUIT TO MENU" };
    const int N = 5;
    int sel = 0, redraw = 1;
    const int bx = 72, by = 70, bw = 112, bh = 24 + N * 14;
    menu_wait_release();
    for (;;) {
        if (redraw) {
            nes_fill(L, bx, by, bw, bh, MENU_BG);
            nes_fill(L, bx, by, bw, 2, MENU_FG);
            nes_text(L, bx + 8, by + 6, "PAUSED", MENU_FG);
            for (int i = 0; i < N; i++) {
                int iy = by + 24 + i * 14;
                if (i == sel) nes_fill(L, bx + 4, iy - 2, bw - 8, 11, MENU_HL);
                nes_text(L, bx + 10, iy, items[i], MENU_FG);
            }
            redraw = 0;
        }
        odroid_gamepad_state gp;
        odroid_input_poll(&gp);
        if (gp.values[ODROID_INPUT_UP])        { sel = (sel + N - 1) % N; redraw = 1; menu_wait_release(); }
        else if (gp.values[ODROID_INPUT_DOWN]) { sel = (sel + 1) % N;     redraw = 1; menu_wait_release(); }
        else if (gp.values[ODROID_INPUT_A])    { menu_wait_release(); return sel; }
        else if (gp.values[ODROID_INPUT_B] || odroid_input_x_held()) { menu_wait_release(); return 0; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Run a .nes ROM (player exits with MENU/Home -> reboot to launcher).
extern "C" void nes_run(const char* path)
{
    printf("nes_run: %s\n", path);

    // Load the ROM into PSRAM; nofrendo's rom_load() reads it via osd_getromdata().
    s_rom = (uint8_t*)heap_caps_malloc(NES_ROM_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rom) s_rom = (uint8_t*)malloc(NES_ROM_MAX);   // fallback to internal
    if (!s_rom) { printf("nes_run: out of memory for ROM\n"); return; }

    size_t sz = odroid_sdcard_copy_file_to_memory(path, s_rom);
    printf("nes_run: loaded %u bytes\n", (unsigned)sz);
    if (sz == 0) { free(s_rom); s_rom = 0; return; }

    // Render NES into the shared static screen buffer (no separate framebuffer alloc).
    composite_release_launcher_fb();          // idle the pump for the handoff
    nofrendo_extfb = composite_shared_screen();

    if (nes_emulate_init(path, 256, 240) != 0) {
        printf("nes_run: nes_emulate_init failed -> reboot\n");
        free(s_rom); s_rom = 0;
        esp_restart();   // launcher fb already freed; reboot for a clean launcher
    }

    uint8_t** lines = nes_emulate_frame(true);     // first frame
    composite_use_nes(lines, nes_4_phase);

    // Run the emulation above the CH559 input task (prio 6) so it isn't preempted
    // mid-frame (that contention + the new audio work caused intermittent hitches).
    // We reboot on exit, so there's no need to restore the priority.
    vTaskPrioritySet(NULL, 8);

    // Pre-fill the audio ring (~3 frames) so the bursty per-frame producer has a
    // cushion and brief frame-time jitter doesn't underflow it (audio stutter).
    for (int p = 0; p < 3; p++) nes_pump_audio();

    int last_x = 0;
    for (;;) {
        odroid_gamepad_state gp;
        odroid_input_poll(&gp);
        if (gp.values[ODROID_INPUT_MENU]) break;   // Home = quick exit to launcher

        // X (rising edge) opens the in-game menu over the paused frame.
        int xnow = odroid_input_x_held();
        if (xnow && !last_x && lines) {
            switch (nes_menu(lines)) {
                case 1: state_setslot(0); menu_flash(lines, state_save() == 0 ? "SAVED" : "SAVE FAILED"); break;
                case 2: state_setslot(0); state_load(); break;     // loaded state renders next frame
                case 3: nes_reset(1); break;                       // RESET (HARD_RESET)
                case 4: free(s_rom); s_rom = 0; esp_restart();     // QUIT
                default: break;                                    // 0 = continue
            }
        }
        last_x = xnow;

        int m = 0;                                  // pack into osd.c's _input_mask
        if (gp.values[ODROID_INPUT_A])      m |= 1 << 0;
        if (gp.values[ODROID_INPUT_B])      m |= 1 << 1;
        if (gp.values[ODROID_INPUT_START])  m |= 1 << 2;
        if (gp.values[ODROID_INPUT_SELECT]) m |= 1 << 3;
        if (gp.values[ODROID_INPUT_UP])     m |= 1 << 4;
        if (gp.values[ODROID_INPUT_DOWN])   m |= 1 << 5;
        if (gp.values[ODROID_INPUT_LEFT])   m |= 1 << 6;
        if (gp.values[ODROID_INPUT_RIGHT])  m |= 1 << 7;
        _input_mask = m;

        // Align the START of the render to the composite vblank, then render. Since
        // nofrendo fills scanlines faster than the ISR scans them out, it stays ahead
        // of the beam -> the single-buffer tear is pushed off-screen.
        composite_wait_vsync();
        lines = nes_emulate_frame(true);
        if (lines) composite_use_nes(lines, nes_4_phase);
        nes_pump_audio();   // one frame of APU audio -> LEDC ring (drained per scanline)
    }

    // Reboot back into the launcher (clean teardown of nofrendo + restores the
    // launcher framebuffer). In-process return can come later.
    printf("nes_run: exiting -> reboot to launcher\n");
    free(s_rom);
    s_rom = 0;
    esp_restart();
}

