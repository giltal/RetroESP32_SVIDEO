// RetroESP32_Atari - standalone Atari 800 app (composite video) launched via OTA from the
// SuperRetroPack launcher. Runs the libatari800 core with the FULL internal RAM to itself:
// native 384-wide Screen_atari + 6502 work RAM both internal -> full speed. Composite output
// uses the same video_out.h NTSC pipeline as the launcher (EMU_ATARI geometry, audio on GPIO5).
//
// Flow: launcher writes ROM path + color calibration to NVS, sets boot to this OTA slot,
// reboots. We read the ROM path, run, and on exit set boot back to factory (launcher).

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"

extern "C" {
#include "odroid_settings.h"
#include "odroid_system.h"
#include "odroid_input.h"
#include "odroid_sdcard.h"
}

// Composite driver (video_out.h has definitions, so it's included in exactly one TU - here;
// odroid_display.cpp is excluded from this app's build).
#define EMU_ATARI 1
#define EMU_NES   2
#define EMU_SMS   3
#include "video_out.h"

// ---- atari800 core ----
#include "config.h"
#include "libatari800.h"
#include "atari.h"
#include "akey.h"
#include "input.h"
#include "memory.h"
#include "screen.h"
#include "sound.h"
#include "statesav.h"
#include "libatari800_statesav.h"
void Atari800_Coldstart(void);

extern int   libatari800_init(int argc, char **argv);
extern int   libatari800_next_frame(input_template_t *input);
extern ULONG *Screen_atari;
extern UBYTE *MEMORY_mem;
extern UBYTE *under_atarixl_os;
extern UBYTE *under_cart809F;
extern UBYTE *under_cartA0BF;
extern int   Atari800_machine_type;
extern int   INPUT_key_code;
extern int   INPUT_key_consol;
void Sound_Callback(UBYTE *buffer, unsigned int size);

#define ATARI_W   384
#define ATARI_H   240
#define A_AUDIO_RATE   15720
#define A_AUDIO_FRAME  (A_AUDIO_RATE / 60)

// ---- composite color params (loaded from NVS, written by the launcher's calibration) ----
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

// ---- platform layer the core reads ----
int  atari800_draw_frame = 1;
unsigned char AtariPot = 228;
int  PLATFORM_kbd_joy_0_enabled = 0;
int  PLATFORM_kbd_joy_1_enabled = 0;
Sound_setup_t Sound_desired = { A_AUDIO_RATE, 1, 1, 0, 0 };
static int _joy[4] = {0,0,0,0};
static int _trig[4] = {0,0,0,0};
#define JOY_FWD 0x01
#define JOY_BCK 0x02
#define JOY_LFT 0x04
#define JOY_RGT 0x08
int PLATFORM_Keyboard(void) { return INPUT_key_code; }
int PLATFORM_PORT(int num) {
    if (num == 0) return (_joy[0] | (_joy[1] << 4)) ^ 0xFF;
    if (num == 1) return (_joy[2] | (_joy[3] << 4)) ^ 0xFF;
    return 0xFF;
}
int PLATFORM_TRIG(int num) { if (num < 0 || num >= 4) return 1; return _trig[num] ^ 1; }
void LIBATARI800_Mouse(void) {}
int  LIBATARI800_Input_Initialise(int *argc, char *argv[]) { (void)argc; (void)argv; return TRUE; }

// Atari NTSC palette generated from the reference (ESP32_TV_EMU atari800: start angle 303 deg,
// 26.8 deg/hue, gamma 2.35) with saturation 2x so hues read clearly through our composite encoder
// at chroma 25 (the old hand-made table was muted -> dark greens read as blue). Baked as a static
// table so build_palette stays a simple loop (no runtime float-gen -> no binary-layout shake).
static const uint32_t atari_palette_rgb[256] = {
    0x000000,0x0f0f0f,0x1b1b1b,0x272727,0x333333,0x414141,0x4f4f4f,0x5e5e5e,
    0x686868,0x787878,0x898989,0x9a9a9a,0xababab,0xbfbfbf,0xd3d3d3,0xeaeaea,
    0x002800,0x0e3400,0x1a3f00,0x264b00,0x325700,0x406500,0x4f7300,0x5d8100,
    0x678c00,0x779c00,0x88ad00,0x9abe00,0xabcf00,0xbee200,0xd2f614,0xe9ff2b,
    0x340b00,0x3f1700,0x4b2300,0x572f00,0x633b00,0x704900,0x7f5700,0x8d6500,
    0x977000,0xa78000,0xb89100,0xc9a200,0xdab300,0xedc614,0xffda29,0xfff140,
    0x5a0000,0x650000,0x710000,0x7d1000,0x881c00,0x962a00,0xa43900,0xb24700,
    0xbd5200,0xcc6200,0xdd7317,0xee8429,0xff953a,0xffa94e,0xffbd62,0xffd47a,
    0x6e0000,0x790000,0x840000,0x900000,0x9c0013,0xa91121,0xb81f2f,0xc62e3e,
    0xd03848,0xe04858,0xf15a69,0xff6b7b,0xff7c8c,0xff909f,0xffa4b3,0xffbbcb,
    0x6b003d,0x760049,0x810054,0x8d0060,0x99006c,0xa6007a,0xb51088,0xc31f96,
    0xcd29a0,0xdd39b0,0xee4bc1,0xff5cd2,0xff6de3,0xff81f7,0xff95ff,0xffacff,
    0x52008a,0x5d0095,0x6800a0,0x7400ac,0x8000b8,0x8e00c5,0x9c0ed3,0xaa1de1,
    0xb528ec,0xc438fb,0xd549ff,0xe65bff,0xf76cff,0xff7fff,0xff94ff,0xffabff,
    0x2800b9,0x3300c4,0x3f00cf,0x4b00db,0x5700e7,0x650cf4,0x731bff,0x8129ff,
    0x8c34ff,0x9b44ff,0xac55ff,0xbe67ff,0xcf78ff,0xe28bff,0xf6a0ff,0xffb7ff,
    0x0000c2,0x0000cd,0x0d00d8,0x1900e4,0x2516f0,0x3324fd,0x4232ff,0x5041ff,
    0x5b4bff,0x6a5bff,0x7c6dff,0x8d7eff,0x9e8fff,0xb2a3ff,0xc6b7ff,0xddceff,
    0x0000a2,0x0010ad,0x001cb8,0x0028c4,0x0034d0,0x0042de,0x1350ec,0x215efa,
    0x2c69ff,0x3c79ff,0x4e8aff,0x5f9bff,0x70acff,0x84c0ff,0x98d4ff,0xafebff,
    0x002261,0x002e6c,0x003977,0x004583,0x00518f,0x005f9d,0x006dab,0x007cb9,
    0x0a86c3,0x1a96d3,0x2ca7e4,0x3db8f5,0x4fc9ff,0x62dcff,0x77f0ff,0x8effff,
    0x00390a,0x004516,0x005021,0x005c2e,0x00683a,0x007647,0x008456,0x009264,
    0x009d6f,0x0cac7e,0x1ebd90,0x30cfa1,0x41e0b2,0x55f3c5,0x69ffd9,0x81fff0,
    0x004500,0x005000,0x005c00,0x006800,0x007400,0x008100,0x008f00,0x009e0c,
    0x00a817,0x16b827,0x28c939,0x39da4a,0x4beb5b,0x5efe6f,0x73ff83,0x8aff9b,
    0x004200,0x004e00,0x005900,0x006500,0x007100,0x007f00,0x0c8d00,0x1a9b00,
    0x25a600,0x35b500,0x46c600,0x58d700,0x69e815,0x7dfc29,0x91ff3e,0xa8ff55,
    0x003300,0x003e00,0x004900,0x105500,0x1d6100,0x2a6f00,0x397d00,0x478c00,
    0x529600,0x62a600,0x73b700,0x84c800,0x96d900,0xa9ec00,0xbdff18,0xd4ff2f,
    0x1f1900,0x2b2400,0x363000,0x423c00,0x4f4800,0x5c5500,0x6a6400,0x797200,
    0x837d00,0x938c00,0xa49d00,0xb5af00,0xc6c000,0xdad300,0xeee71b,0xfffe32,
};

// THE Atari NTSC palette, copied verbatim from the reference (ESP32_TV_EMU_AtariNES,
// emu_atari800.cpp atari_4_phase_ntsc[]). These are 4-phase COMPOSITE samples (phases 0/90/180/270,
// each byte = sample>>8), same format our composite ISR + composite_encode_rgb use (verified:
// IRE macro identical, [0]=0x18181818=our encoded black). The reference's NTSC generator uses a
// REVERSED hue mapping (angle = start + ((15-cr)-1)*28.6deg) which our RGB-path port got wrong ->
// greens came out blue. Using these exact values gives the correct colors. (PAL has its own table.)
static const uint32_t atari_4_phase_ntsc[256] = {
    0x18181818,0x1A1A1A1A,0x1C1C1C1C,0x1F1F1F1F,0x21212121,0x24242424,0x27272727,0x2A2A2A2A,
    0x2D2D2D2D,0x30303030,0x34343434,0x38383838,0x3B3B3B3B,0x40404040,0x44444444,0x49494949,
    0x1A15210E,0x1C182410,0x1E1A2612,0x211D2915,0x231F2B18,0x26222E1A,0x2925311D,0x2C283420,
    0x2F2B3723,0x322E3A27,0x36323E2A,0x3A36412E,0x3D394532,0x423D4936,0x46424E3A,0x4B46523F,
    0x151A210E,0x171D2310,0x191F2613,0x1C222815,0x1E242B18,0x21272E1B,0x242A311D,0x272D3420,
    0x2A303724,0x2E333A27,0x31373D2A,0x353A412E,0x393E4532,0x3D424936,0x41474D3A,0x464B523F,
    0x101F1F10,0x13212113,0x15232315,0x18262618,0x1A28281A,0x1D2B2B1D,0x202E2E20,0x23313123,
    0x26343426,0x29383729,0x2D3B3B2D,0x303F3F31,0x34434234,0x38474738,0x3D4B4B3D,0x41505041,
    0x0E211A15,0x10231D17,0x13261F19,0x1528221C,0x182B241F,0x1B2E2721,0x1D312A24,0x20342D27,
    0x2337302A,0x273A332E,0x2A3E3731,0x2E413A35,0x32453E39,0x3649423D,0x3A4D4741,0x3F524B46,
    0x0E21151A,0x1024181C,0x12261A1E,0x15291D21,0x182B1F24,0x1A2E2226,0x1D312529,0x2034282C,
    0x23372B2F,0x273A2E33,0x2A3E3236,0x2E41353A,0x3245393E,0x36493D42,0x3A4E4246,0x3F52464B,
    0x101F111E,0x12211320,0x15241623,0x17261825,0x1A291B28,0x1D2C1E2B,0x202F202E,0x23322331,
    0x26352634,0x29382A37,0x2C3B2D3B,0x303F313E,0x34433542,0x38473946,0x3C4B3D4A,0x4150424F,
    0x141B0E21,0x161D1023,0x19201326,0x1B221528,0x1E25182B,0x21281B2E,0x242A1E30,0x272E2133,
    0x2A312436,0x2D34273A,0x30372B3D,0x343B2E41,0x383F3245,0x3C433649,0x40473A4D,0x454C3F52,
    0x19160E21,0x1B181024,0x1E1B1226,0x201D1529,0x2320172B,0x26231A2E,0x29261D31,0x2C292034,
    0x2F2C2337,0x322F273A,0x35322A3E,0x39362E41,0x3D3A3245,0x413E3649,0x45423A4E,0x4A473F52,
    0x1E11101F,0x20141222,0x22161424,0x25191727,0x271B1929,0x2A1E1C2C,0x2D211F2F,0x30242232,
    0x33272535,0x362A2838,0x3A2E2C3C,0x3E323040,0x41353343,0x46393847,0x4A3E3C4C,0x4F424150,
    0x210E131C,0x2311161E,0x25131820,0x28161B23,0x2A181D26,0x2D1B2028,0x301E232B,0x3321262E,
    0x36242931,0x3A272C35,0x3D2B3038,0x412E333C,0x45323740,0x49363B44,0x4D3B4048,0x523F444D,
    0x210E1817,0x24101B19,0x26121D1B,0x29151F1E,0x2B172221,0x2E1A2523,0x311D2826,0x34202B29,
    0x37232E2C,0x3A263130,0x3E2A3533,0x422E3837,0x45313C3B,0x4936403F,0x4E3A4543,0x523F4948,
    0x200F1D12,0x22111F14,0x25142217,0x27162419,0x2A19271C,0x2D1C2A1F,0x2F1F2C22,0x32222F25,
    0x35253328,0x3928362B,0x3C2C392F,0x402F3D32,0x44334136,0x4837453A,0x4C3B493E,0x51404E43,
    0x1C13200F,0x1F152311,0x21172513,0x241A2816,0x261D2A19,0x291F2D1B,0x2C22301E,0x2F253321,
    0x32283624,0x352C3928,0x392F3D2B,0x3C33402F,0x40374433,0x443B4837,0x493F4D3B,0x4D445140,
    0x1818220E,0x1A1A2410,0x1C1C2612,0x1F1F2915,0x21212B17,0x24242E1A,0x2727311D,0x2A2A3420,
    0x2D2D3723,0x30303A26,0x34343E2A,0x3838422E,0x3B3B4531,0x40404A36,0x44444E3A,0x4949533F,
    0x131C200F,0x151F2311,0x17212513,0x1A242816,0x1D262A19,0x1F292D1B,0x222C301E,0x252F3321,
    0x28323624,0x2C353928,0x2F393D2B,0x333C402F,0x37404433,0x3B444837,0x3F494D3B,0x444D5140,
};

static uint32_t s_apal[256];
static uint8_t* s_alines[ATARI_H];
static int16_t  s_abuf[512];
static char     s_rompath[300];
static UBYTE*   s_statebuf = 0;          // 210KB in PSRAM, for save states
static statesav_tags_t s_statetags;

static void build_palette(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t c = atari_palette_rgb[i];
        s_apal[i] = composite_encode_rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }
    // Override with the reference's exact NTSC 4-phase composite palette (correct hues; the RGB
    // path above had the hue mapping wrong -> greens read blue). Kept the loop above so
    // composite_encode_rgb stays linked and the binary layout barely shifts (no shake).
    for (int i = 0; i < 256; i++) s_apal[i] = atari_4_phase_ntsc[i];
}

static void pump_audio(void) {
    int n = A_AUDIO_FRAME;
    if (n > (int)(sizeof(s_abuf)/sizeof(s_abuf[0]))) n = sizeof(s_abuf)/sizeof(s_abuf[0]);
    uint8_t a8[512];
    Sound_Callback(a8, n);
    for (int i = 0; i < n; i++) s_abuf[i] = (int16_t)(((int)a8[i] - 128) << 8);
    audio_write_16(s_abuf, n, 1);
}

// Full internal RAM is ours here (standalone app): both Screen_atari and MEMORY_mem internal.
static bool preallocate(void) {
    Screen_atari = (ULONG *)heap_caps_malloc(ATARI_W * ATARI_H, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!Screen_atari) { printf("atari: Screen_atari INTERNAL alloc failed\n"); return false; }
    memset(Screen_atari, 0, ATARI_W * ATARI_H);
    MEMORY_mem = (UBYTE *)heap_caps_malloc(65536 + 4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!MEMORY_mem) MEMORY_mem = (UBYTE *)heap_caps_malloc(65536 + 4, MALLOC_CAP_SPIRAM);
    if (!MEMORY_mem) { printf("atari: MEMORY_mem alloc failed\n"); return false; }
    memset(MEMORY_mem, 0, 65536 + 4);
    under_atarixl_os = (UBYTE *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    under_cart809F   = (UBYTE *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    under_cartA0BF   = (UBYTE *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!under_atarixl_os || !under_cart809F || !under_cartA0BF) { printf("atari: underlay alloc failed\n"); return false; }
    printf("atari: Screen_atari=%p MEMORY_mem=%p\n", (void*)Screen_atari, (void*)MEMORY_mem);
    return true;
}

static const char* get_ext(const char* p) { const char* d=""; for (; *p; p++) if (*p=='.') d=p+1; return d; }
static int ext_ieq(const char* a, const char* b) {
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; a++; b++; }
    return *a==0 && *b==0;
}

static void emu_init(const char* filename) {
    const char* ext = get_ext(filename);
    const char* argv[12]; int argc = 0;
    argv[argc++] = "atari800";
    argv[argc++] = "-xl"; argv[argc++] = "-nobasic"; argv[argc++] = (char*)filename;
    (void)ext;
    argv[argc++] = "-ntsc";
    printf("emu_init: %s (argc=%d)\n", filename, argc);
    int r = libatari800_init(argc, (char**)argv);
    printf("libatari800_init -> %d\n", r);
    Sound_desired.freq = A_AUDIO_RATE;
}

// Start the composite pump on core 1 (away from the busy emulation on core 0).
static volatile bool s_vinit_done = false;
static void video_init_core1(void* arg) {
    (void)arg;
    video_init(4 /*cc_width*/, EMU_ATARI, s_apal, 1 /*ntsc*/);
    s_vinit_done = true;
    vTaskDelete(NULL);
}

static void exit_to_launcher(void) {
    const esp_partition_t* factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) esp_ota_set_boot_partition(factory);
    esp_restart();
}

// ---- in-game menu (X), drawn into Screen_atari with Atari palette indices ----
// 0x00=black, 0x0F=white, 0x88=light blue (from atari_palette_rgb).
#define A_BG 0x00
#define A_FG 0x0F
#define A_HL 0x88
static const uint8_t font5x7[] = {
    0x00,0x00,0x00,0x00,0x00, 0x04,0x04,0x04,0x00,0x04, 0x0A,0x0A,0x00,0x00,0x00,
    0x0A,0x1F,0x0A,0x1F,0x0A, 0x04,0x0F,0x06,0x1E,0x04, 0x19,0x1A,0x04,0x0B,0x13,
    0x06,0x09,0x16,0x09,0x16, 0x04,0x04,0x00,0x00,0x00, 0x08,0x04,0x04,0x04,0x08,
    0x02,0x04,0x04,0x04,0x02, 0x04,0x15,0x0E,0x15,0x04, 0x00,0x04,0x0E,0x04,0x00,
    0x00,0x00,0x00,0x04,0x02, 0x00,0x00,0x0E,0x00,0x00, 0x00,0x00,0x00,0x00,0x04,
    0x10,0x08,0x04,0x02,0x01, 0x0E,0x19,0x15,0x13,0x0E, 0x04,0x06,0x04,0x04,0x0E,
    0x0E,0x11,0x0C,0x02,0x1F, 0x0E,0x11,0x0C,0x11,0x0E, 0x08,0x0C,0x0A,0x1F,0x08,
    0x1F,0x01,0x0F,0x10,0x0F, 0x0E,0x01,0x0F,0x11,0x0E, 0x1F,0x10,0x08,0x04,0x04,
    0x0E,0x11,0x0E,0x11,0x0E, 0x0E,0x11,0x1E,0x10,0x0E, 0x00,0x04,0x00,0x04,0x00,
    0x00,0x04,0x00,0x04,0x02, 0x08,0x04,0x02,0x04,0x08, 0x00,0x0E,0x00,0x0E,0x00,
    0x02,0x04,0x08,0x04,0x02, 0x0E,0x11,0x08,0x00,0x04, 0x0E,0x11,0x1D,0x01,0x0E,
    0x0E,0x11,0x1F,0x11,0x11, 0x0F,0x11,0x0F,0x11,0x0F, 0x0E,0x11,0x01,0x11,0x0E,
    0x07,0x09,0x11,0x09,0x07, 0x1F,0x01,0x0F,0x01,0x1F, 0x1F,0x01,0x0F,0x01,0x01,
    0x0E,0x01,0x19,0x11,0x0E, 0x11,0x11,0x1F,0x11,0x11, 0x0E,0x04,0x04,0x04,0x0E,
    0x1C,0x08,0x08,0x09,0x06, 0x11,0x09,0x07,0x09,0x11, 0x01,0x01,0x01,0x01,0x1F,
    0x11,0x1B,0x15,0x11,0x11, 0x11,0x13,0x15,0x19,0x11, 0x0E,0x11,0x11,0x11,0x0E,
    0x0F,0x11,0x0F,0x01,0x01, 0x0E,0x11,0x15,0x09,0x16, 0x0F,0x11,0x0F,0x09,0x11,
    0x0E,0x01,0x0E,0x10,0x0F, 0x1F,0x04,0x04,0x04,0x04, 0x11,0x11,0x11,0x11,0x0E,
    0x11,0x11,0x0A,0x0A,0x04, 0x11,0x11,0x15,0x1B,0x11, 0x11,0x0A,0x04,0x0A,0x11,
    0x11,0x0A,0x04,0x04,0x04, 0x1F,0x08,0x04,0x02,0x1F, 0x0E,0x02,0x02,0x02,0x0E,
    0x01,0x02,0x04,0x08,0x10, 0x0E,0x08,0x08,0x08,0x0E, 0x04,0x0A,0x11,0x00,0x00,
    0x00,0x00,0x00,0x00,0x1F, 0x02,0x04,0x00,0x00,0x00, 0x00,0x0E,0x12,0x12,0x1C,
    0x01,0x0F,0x11,0x11,0x0F, 0x00,0x0E,0x01,0x01,0x0E, 0x10,0x1E,0x11,0x11,0x1E,
    0x00,0x0E,0x1F,0x01,0x0E, 0x0C,0x02,0x07,0x02,0x02, 0x00,0x1E,0x11,0x1E,0x10,
    0x01,0x0F,0x11,0x11,0x11, 0x04,0x00,0x04,0x04,0x04, 0x08,0x00,0x08,0x08,0x06,
    0x01,0x09,0x07,0x09,0x11, 0x06,0x04,0x04,0x04,0x0E, 0x00,0x0B,0x15,0x15,0x11,
    0x00,0x0F,0x11,0x11,0x11, 0x00,0x0E,0x11,0x11,0x0E, 0x00,0x0F,0x11,0x0F,0x01,
    0x00,0x1E,0x11,0x1E,0x10, 0x00,0x0D,0x13,0x01,0x01, 0x00,0x0E,0x06,0x18,0x0E,
    0x02,0x07,0x02,0x02,0x0C, 0x00,0x11,0x11,0x11,0x1E, 0x00,0x11,0x11,0x0A,0x04,
    0x00,0x11,0x15,0x15,0x0A, 0x00,0x11,0x0A,0x0A,0x11, 0x00,0x11,0x1E,0x10,0x0E,
    0x00,0x1F,0x08,0x04,0x1F,
};
static void a_fill(int x, int y, int w, int h, uint8_t idx) {
    uint8_t* fb = (uint8_t*)Screen_atari;
    for (int r = 0; r < h; r++) { int yy = y + r; if (yy < 0 || yy >= ATARI_H) continue;
        for (int c = 0; c < w; c++) { int xx = x + c; if (xx >= 0 && xx < ATARI_W) fb[yy*ATARI_W + xx] = idx; } }
}
static void a_char(int cx, int cy, char ch, uint8_t idx, int scale) {
    if (ch < 32 || ch > 122) return;
    uint8_t* fb = (uint8_t*)Screen_atari;
    const uint8_t* g = &font5x7[(ch - 32) * 5];
    for (int row = 0; row < 5; row++) { uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) if (bits & (1 << col))
            for (int sy = 0; sy < scale; sy++) for (int sx = 0; sx < scale; sx++) {
                int px = cx + col*scale + sx, py = cy + row*scale + sy;
                if (px >= 0 && px < ATARI_W && py >= 0 && py < ATARI_H) fb[py*ATARI_W + px] = idx; } }
}
static void a_text(int x, int y, const char* s, uint8_t idx, int scale) {
    while (*s) { a_char(x, y, *s, idx, scale); x += 6 * scale; s++; }
}
static void a_menu_wait_release(void) {
    odroid_gamepad_state gp;
    for (int i = 0; i < 60; i++) {
        odroid_input_poll(&gp);
        if (!odroid_input_x_held() && !gp.values[ODROID_INPUT_A] &&
            !gp.values[ODROID_INPUT_UP] && !gp.values[ODROID_INPUT_DOWN] &&
            !gp.values[ODROID_INPUT_B]) return;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

// Save state: file next to the ROM, extension ".sav".
static void a_state_path(char* out, int n) {
    strncpy(out, s_rompath, n - 1); out[n - 1] = 0;
    char* dot = strrchr(out, '.');
    if (dot) strcpy(dot, ".sav");
    else { int l = (int)strlen(out); if (l < n - 5) strcpy(out + l, ".sav"); }
}
static bool a_save_state(void) {
    if (!s_statebuf) return false;
    memset(s_statebuf, 0, STATESAV_MAX_SIZE);
    LIBATARI800_StateSave(s_statebuf, &s_statetags);
    ULONG used = StateSav_Tell();
    char p[300]; a_state_path(p, sizeof(p));
    FILE* f = fopen(p, "wb");
    if (!f) { printf("a_save_state: fopen failed %s\n", p); return false; }
    size_t w = fwrite(s_statebuf, 1, used, f);
    fclose(f);
    return w == (size_t)used;
}
static bool a_load_state(void) {
    if (!s_statebuf) return false;
    char p[300]; a_state_path(p, sizeof(p));
    FILE* f = fopen(p, "rb");
    if (!f) { printf("a_load_state: no save %s\n", p); return false; }
    size_t rd = fread(s_statebuf, 1, STATESAV_MAX_SIZE, f);
    fclose(f);
    if (rd < 16) return false;
    LIBATARI800_StateLoad(s_statebuf);
    return true;
}
static void a_flash_msg(const char* msg) {
    int w = (int)strlen(msg) * 12;
    int bx = ATARI_W/2 - w/2 - 8, by = 116;
    a_fill(bx, by, w + 16, 26, A_BG);
    a_fill(bx, by, w + 16, 2, A_FG);
    a_text(bx + 8, by + 8, msg, A_FG, 2);
    vTaskDelay(pdMS_TO_TICKS(800));
}
// Returns 0=continue, 1=save, 2=load, 3=reset, 4=quit.
static int atari_menu(void) {
    static const char* items[] = { "CONTINUE", "SAVE STATE", "LOAD STATE", "RESET", "QUIT TO MENU" };
    const int N = 5;
    int sel = 0, redraw = 1;
    const int bx = 96, by = 52, bw = 192, bh = 28 + N * 20;
    a_menu_wait_release();
    for (;;) {
        if (redraw) {
            a_fill(bx, by, bw, bh, A_BG);
            a_fill(bx, by, bw, 2, A_FG);
            a_text(bx + 8, by + 7, "PAUSED", A_FG, 2);
            for (int i = 0; i < N; i++) {
                int iy = by + 30 + i * 20;
                if (i == sel) a_fill(bx + 4, iy - 3, bw - 8, 18, A_HL);
                a_text(bx + 10, iy, items[i], A_FG, 2);
            }
            redraw = 0;
        }
        odroid_gamepad_state gp;
        odroid_input_poll(&gp);
        if (gp.values[ODROID_INPUT_UP])        { sel = (sel + N - 1) % N; redraw = 1; a_menu_wait_release(); }
        else if (gp.values[ODROID_INPUT_DOWN]) { sel = (sel + 1) % N;     redraw = 1; a_menu_wait_release(); }
        else if (gp.values[ODROID_INPUT_A])    { a_menu_wait_release(); return sel; }
        else if (gp.values[ODROID_INPUT_B] || odroid_input_x_held()) { a_menu_wait_release(); return 0; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void) {
    printf("\n----- RetroESP32_Atari -----\n");
    nvs_flash_init();
    odroid_system_init();
    odroid_input_gamepad_init();
    odroid_sdcard_open("/sd");
    load_calibration();

    const char* nvsrom = odroid_settings_RomFilePath_get();
    const char* ne = nvsrom ? get_ext(nvsrom) : "";
    bool atari_rom = nvsrom && (ext_ieq(ne,"atr") || ext_ieq(ne,"xex") || ext_ieq(ne,"car") ||
                                ext_ieq(ne,"bin") || ext_ieq(ne,"rom") || ext_ieq(ne,"com") || ext_ieq(ne,"a800"));
    if (!atari_rom) { printf("atari: NVS ROM not an Atari file -> back to launcher\n"); exit_to_launcher(); }
    strncpy(s_rompath, nvsrom, sizeof(s_rompath)-1); s_rompath[sizeof(s_rompath)-1] = 0;
    printf("atari: ROM = %s\n", s_rompath);

    if (!preallocate()) { printf("atari: prealloc failed -> launcher\n"); exit_to_launcher(); }
    emu_init(s_rompath);
    s_statebuf = (UBYTE*)heap_caps_malloc(STATESAV_MAX_SIZE, MALLOC_CAP_SPIRAM);   // save states

    for (int y = 0; y < ATARI_H; y++) s_alines[y] = (uint8_t*)Screen_atari + y * ATARI_W;
    build_palette();

    // Hand the framebuffer to the driver, then start the pump on core 1.
    _lines = s_alines;
    s_vinit_done = false;
    xTaskCreatePinnedToCore(video_init_core1, "vinit", 4096, NULL, 10, NULL, 1);
    while (!s_vinit_done) vTaskDelay(pdMS_TO_TICKS(2));
    _lines = s_alines;   // ensure (video_init may reset)

    vTaskPrioritySet(NULL, 8);
    for (int p = 0; p < 4; p++) { libatari800_next_frame(NULL); pump_audio(); }

    int last_x = 0;
    for (;;) {
        odroid_gamepad_state gp;
        odroid_input_poll(&gp);
        if (gp.values[ODROID_INPUT_MENU]) exit_to_launcher();   // Home = quick exit

        // X (rising edge) opens the in-game menu over the paused frame (like NES/SMS).
        int xnow = odroid_input_x_held();
        if (xnow && !last_x) {
            switch (atari_menu()) {
                case 1: a_flash_msg(a_save_state() ? "SAVED" : "SAVE FAILED"); break;
                case 2: a_load_state(); break;          // restored state renders next frames
                case 3: Atari800_Coldstart(); break;    // RESET
                case 4: exit_to_launcher();             // QUIT to launcher
                default: break;                         // 0 = continue
            }
            for (int p = 0; p < 3; p++) { libatari800_next_frame(NULL); pump_audio(); }  // re-prime + repaint
            last_x = odroid_input_x_held();
            continue;
        }
        last_x = xnow;

        _joy[0] = 0; _trig[0] = 0;
        if (gp.values[ODROID_INPUT_UP])    _joy[0] |= JOY_FWD;
        if (gp.values[ODROID_INPUT_DOWN])  _joy[0] |= JOY_BCK;
        if (gp.values[ODROID_INPUT_LEFT])  _joy[0] |= JOY_LFT;
        if (gp.values[ODROID_INPUT_RIGHT]) _joy[0] |= JOY_RGT;
        if (gp.values[ODROID_INPUT_A] || gp.values[ODROID_INPUT_B]) _trig[0] = 1;
        int consol = 0x07;
        if (gp.values[ODROID_INPUT_START])  consol &= ~0x01;
        if (gp.values[ODROID_INPUT_SELECT]) consol &= ~0x02;
        INPUT_key_consol = consol;
        INPUT_key_code = (Atari800_machine_type == Atari800_MACHINE_5200 &&
                          gp.values[ODROID_INPUT_START]) ? AKEY_5200_START : AKEY_NONE;

        // Sync each frame to the composite vsync so the single-buffer tear stays stationary
        // (otherwise it drifts -> the picture "shakes"). The Atari emulation (~8.6ms) fits
        // well inside the 16.6ms frame, so this locks cleanly to 60fps. The vTaskDelay also
        // yields to IDLE/CH559 (no watchdog). Audio then runs at the matching 60fps rate.
        int fc = _frame_counter, guard = 0;
        while (_frame_counter == fc && ++guard < 200) vTaskDelay(1);

        atari800_draw_frame = 1;
        libatari800_next_frame(NULL);
        pump_audio();
    }
}
