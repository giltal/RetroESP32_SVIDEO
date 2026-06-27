// odroid_input.c - input for RetroESP32_SVIDEO.
//
// Ported from the original ESP32_TV_EMU_AtariNES sketch (controllerTask):
//   * CH559 USB-HID host over UART2 (RX=19, TX=23, 1 Mbaud) - keyboards/gamepads
//   * direct-GPIO Atari-style joystick (active-low buttons)
// CH559 takes priority; direct GPIO fills any button the USB pad isn't pressing.
// Results are mapped into the odroid_gamepad_state the launcher/emulators expect.

#include "odroid_input.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"

// ---------------------------------------------------------------------------
// Direct-GPIO Atari joystick (active-low; board provides pulls on input-only pins)
// ---------------------------------------------------------------------------
#define PIN_LEFT   14
#define PIN_DOWN   21
#define PIN_UP     22
#define PIN_HOME   26   // freed now that audio moved to GPIO5
#define PIN_RIGHT  27
#define PIN_FIRE   32
#define PIN_START  34   // input-only, no internal pull (external pull on board)
#define PIN_SEL    35   // input-only, no internal pull
// GPIO36 = analog paddle - not used by the launcher

static inline int pressed(int pin) { return gpio_get_level((gpio_num_t)pin) == 0; }

// ---------------------------------------------------------------------------
// CH559 UART USB-HID host  (faithful C port of the CH559 Arduino library)
// ---------------------------------------------------------------------------
#define CH559_UART      UART_NUM_2
#define CH559_TX        23
#define CH559_RX        19
#define CH559_BAUD      1000000
#define CH559_RXBUF     2048

#define MAX_HID_DATA_SIZE       8
#define START_OF_FRAME          0xFE
#define MSG_TYPE_DEVICE_POLL    4
#define DEV_TYPE_GAMEPAD        5
#define DEV_TYPE_JOYSTICK       4
#define DEV_TYPE_MOUSE          2
#define DEV_TYPE_KEYBOARD       6

enum { WAIT_FOR_START_OF_FRAME=0, STATE_READ_LEN_LOW, STATE_READ_LEN_HIGH,
       STATE_MSG_TYPE, STATE_DEV_TYPE, STATE_READ_PORT, STATE_READ_ID,
       STATE_READ_DATA, STATE_READ_ALL };

typedef struct {
    unsigned int  ID, type, port, dataSize;
    unsigned char rawData[MAX_HID_DATA_SIZE];
} HIDdevice;

typedef struct {
    unsigned char analogX1, analogY1, analogX2, analogY2;
    unsigned char select, start, a, b, x, y;
    unsigned char leftS1, leftS2, rightS1, rightS2;
} joyStick;

typedef struct {
    unsigned int  deviceID;
    unsigned char analogX1index, analogY1index, analogX2index, analogY2index;
    unsigned char selectIndex, selectMask, startIndex, startMask;
    unsigned char aIndex, aMask, bIndex, bMask, xIndex, xMask, yIndex, yMask;
    unsigned char leftS1Index, leftS1Mask, leftS2Index, leftS2Mask;
    unsigned char rightS1Index, rightS1Mask, rightS2Index, rightS2Mask;
} joystickMapper;

// CH559 frame-parser state (persists across calls, like the C++ object)
static int   ch_state = WAIT_FOR_START_OF_FRAME;
static short ch_len, ch_dataIndex, ch_counter, ch_leftData;
static unsigned int ch_deviceType;
static unsigned char ch_port;
static bool s_ch559_ok = false;

static inline int ch_available(void) {
    size_t n = 0;
    uart_get_buffered_data_len(CH559_UART, &n);
    return (int)n;
}
static inline int ch_read(void) {
    uint8_t b = 0;
    return (uart_read_bytes(CH559_UART, &b, 1, 0) == 1) ? b : -1;
}

// Returns true when a full HID frame has been decoded into *hid.
static bool ch559_update(HIDdevice* hid) {
    int t;
    while (ch_available()) {
        switch (ch_state) {
        case WAIT_FOR_START_OF_FRAME:
            t = ch_read();
            if (t == START_OF_FRAME) ch_state = STATE_READ_LEN_LOW;
            continue;
        case STATE_READ_LEN_LOW:
            ch_len = ch_read(); ch_state = STATE_READ_LEN_HIGH; continue;
        case STATE_READ_LEN_HIGH:
            t = ch_read();
            ch_len = ((unsigned short)t << 8) | ch_len;
            if (ch_len > MAX_HID_DATA_SIZE) { ch_state = STATE_READ_ALL; ch_leftData = 8 + ch_len; continue; }
            hid->dataSize = ch_len; ch_dataIndex = ch_len; ch_state = STATE_MSG_TYPE; continue;
        case STATE_MSG_TYPE:
            t = ch_read();
            if (t == MSG_TYPE_DEVICE_POLL) { ch_state = STATE_DEV_TYPE; continue; }
            ch_state = STATE_READ_ALL; ch_leftData = 7 + ch_len; continue;
        case STATE_DEV_TYPE:
            ch_deviceType = ch_read();
            if (ch_deviceType==DEV_TYPE_GAMEPAD || ch_deviceType==DEV_TYPE_JOYSTICK ||
                ch_deviceType==DEV_TYPE_MOUSE   || ch_deviceType==DEV_TYPE_KEYBOARD) {
                ch_state = STATE_READ_PORT; hid->type = ch_deviceType; continue;
            }
            ch_state = STATE_READ_ALL; ch_leftData = 6 + ch_len; continue;
        case STATE_READ_PORT:
            ch_port = ch_read();
            if (ch_port <= 2) { hid->port = ch_port; hid->ID = 0; ch_counter = 5; ch_state = STATE_READ_ID; continue; }
            ch_state = STATE_READ_ALL; ch_leftData = 5 + ch_len; continue;
        case STATE_READ_ID:
            if (ch_counter > 0) { t = ch_read(); hid->ID = (hid->ID << 8) | t; ch_counter--; continue; }
            ch_state = STATE_READ_DATA; continue;
        case STATE_READ_DATA:
            if (ch_len > 0) { t = ch_read(); hid->rawData[ch_dataIndex - ch_len] = t; ch_len--; continue; }
            ch_state = WAIT_FOR_START_OF_FRAME; return true;
        case STATE_READ_ALL:
            if (ch_leftData > 0) { (void)ch_read(); ch_leftData--; continue; }
            ch_state = WAIT_FOR_START_OF_FRAME; continue;
        default:
            ch_state = WAIT_FOR_START_OF_FRAME; continue;
        }
    }
    return false;
}

static void decodeJoystick(const HIDdevice* h, const joystickMapper* m, joyStick* j) {
    j->analogX1 = h->rawData[m->analogX1index];
    j->analogY1 = h->rawData[m->analogY1index];
    j->analogX2 = h->rawData[m->analogX2index];
    j->analogY2 = h->rawData[m->analogY2index];
    j->a      = ((h->rawData[m->aIndex]      & m->aMask)      == m->aMask);
    j->b      = ((h->rawData[m->bIndex]      & m->bMask)      == m->bMask);
    j->x      = ((h->rawData[m->xIndex]      & m->xMask)      == m->xMask);
    j->y      = ((h->rawData[m->yIndex]      & m->yMask)      == m->yMask);
    j->select = ((h->rawData[m->selectIndex] & m->selectMask) == m->selectMask);
    j->start  = ((h->rawData[m->startIndex]  & m->startMask)  == m->startMask);
    j->leftS1 = ((h->rawData[m->leftS1Index] & m->leftS1Mask) == m->leftS1Mask);
    j->rightS1= ((h->rawData[m->rightS1Index]& m->rightS1Mask)== m->rightS1Mask);
}

// Mapper tables copied verbatim from the sketch's setup().
static const joystickMapper RetroJoy = {
    .deviceID=0x79000600, .analogX1index=0,.analogY1index=1,.analogX2index=2,.analogY2index=3,
    .selectIndex=6,.selectMask=0x10, .startIndex=6,.startMask=0x20,
    .aIndex=6,.aMask=0x08, .bIndex=5,.bMask=0x80, .xIndex=6,.xMask=0x04, .yIndex=5,.yMask=0x40,
    .leftS1Index=6,.leftS1Mask=0x01, .leftS2Index=4,.leftS2Mask=0x80,
    .rightS1Index=5,.rightS1Mask=0x10, .rightS2Index=4,.rightS2Mask=0x80,
};
static const joystickMapper SNESmap = {
    .deviceID=0x79001100, .analogX1index=1,.analogY1index=2,.analogX2index=3,.analogY2index=4,
    .selectIndex=6,.selectMask=0x10, .startIndex=6,.startMask=0x20,
    .aIndex=5,.aMask=0x20, .bIndex=5,.bMask=0x40, .xIndex=5,.xMask=0x10, .yIndex=5,.yMask=0x80,
    .leftS1Index=6,.leftS1Mask=0x01, .leftS2Index=0,.leftS2Mask=0xFF,
    .rightS1Index=6,.rightS1Mask=0x02, .rightS2Index=0,.rightS2Mask=0xFF,
};
static const joystickMapper SparkFun = {
    .deviceID=0x4f1b0692, .analogX1index=2,.analogY1index=4,.analogX2index=6,.analogY2index=0,
    .selectIndex=1,.selectMask=0x08, .startIndex=1,.startMask=0x02,
    .aIndex=1,.aMask=0x01, .bIndex=0,.bMask=0xF0, .xIndex=0,.xMask=0xF0, .yIndex=0,.yMask=0xF0,
    .leftS1Index=1,.leftS1Mask=0x04, .leftS2Index=0,.leftS2Mask=0xF0,
    .rightS1Index=1,.rightS1Mask=0x10, .rightS2Index=0,.rightS2Mask=0xF0,
};

// Latched USB-pad button state. Written by ch559_task (core 1), read in
// odroid_input_gamepad_read (core 0). Byte writes are atomic; races are benign.
static volatile uint8_t u_up,u_down,u_left,u_right,u_a,u_b,u_start,u_select,u_home,u_r,u_x;

static void apply_pad(const HIDdevice* hid, const joystickMapper* m, int sparkfun) {
    joyStick j = {0};
    decodeJoystick(hid, m, &j);
    u_a=j.a; u_b = sparkfun ? 0 : j.b; u_start=j.start; u_select=j.select;
    u_home=j.leftS1; u_r=j.rightS1; u_x = sparkfun ? 0 : j.x;
    if (m == &SNESmap) {            // SNES decodes dpad from analog axis 2
        u_up   = (j.analogY2 == 0x00); u_down  = (j.analogY2 == 0xff);
        u_left = (j.analogX2 == 0x00); u_right = (j.analogX2 == 0xff);
    } else if (sparkfun) {          // SparkFun centers at 0x01
        u_up   = (j.analogY1 == 0x01); u_down  = (j.analogY1 == 0xff);
        u_left = (j.analogX1 == 0x01); u_right = (j.analogX1 == 0xff);
    } else {                        // RetroJoy: analog axis 1
        u_up   = (j.analogY1 == 0x00); u_down  = (j.analogY1 == 0xff);
        u_left = (j.analogX1 == 0x00); u_right = (j.analogX1 == 0xff);
    }
}

// Continuously drain + decode the CH559 UART, pinned to core 1 so neither the
// core-0 video ISR nor the launcher's heavy redraws ever stall the 1Mbaud RX
// (which previously overflowed the buffer and desynced the frame parser).
static void ch559_task(void* arg) {
    (void)arg;
    HIDdevice hid = {0};
    for (;;) {
        bool any = false;
        while (ch559_update(&hid)) {
            any = true;
            if      (hid.ID == SNESmap.deviceID)  apply_pad(&hid, &SNESmap, 0);
            else if (hid.ID == RetroJoy.deviceID) apply_pad(&hid, &RetroJoy, 0);
            else if (hid.ID == SparkFun.deviceID) apply_pad(&hid, &SparkFun, 1);
            // unknown/garbage IDs are ignored - they never clobber button state
        }
        vTaskDelay(pdMS_TO_TICKS(any ? 2 : 5));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void odroid_input_gamepad_init() {
    // Direct-GPIO buttons: input, pull-up where the pin supports it.
    const int pull_pins[] = { PIN_LEFT,PIN_DOWN,PIN_UP,PIN_HOME,PIN_RIGHT,PIN_FIRE };
    for (unsigned i = 0; i < sizeof(pull_pins)/sizeof(pull_pins[0]); i++) {
        gpio_set_direction((gpio_num_t)pull_pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pull_pins[i], GPIO_PULLUP_ONLY);
    }
    gpio_set_direction((gpio_num_t)PIN_START, GPIO_MODE_INPUT);  // input-only, ext. pull
    gpio_set_direction((gpio_num_t)PIN_SEL,   GPIO_MODE_INPUT);

    // CH559 USB host on UART2.
    uart_config_t uc = {
        .baud_rate = CH559_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    if (uart_param_config(CH559_UART, &uc) == ESP_OK &&
        uart_set_pin(CH559_UART, CH559_TX, CH559_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK &&
        uart_driver_install(CH559_UART, CH559_RXBUF, 0, 0, NULL, 0) == ESP_OK) {
        s_ch559_ok = true;
        uart_write_bytes(CH559_UART, "R", 1);   // reset CH559 (re-enumerate)
        // Pin to core 0 (core 1 stays reserved for the timing-critical video ISR), but at
        // priority 9 - ABOVE the emulator task (prio 8). The emulator can run flat-out
        // without yielding during heavy frames; at a lower prio the CH559 poll would be
        // starved and USB input would die mid-game. As a high-prio task that blocks on its
        // own vTaskDelay, it just preempts briefly every few ms to drain USB, then sleeps.
        xTaskCreatePinnedToCore(ch559_task, "ch559", 4096, NULL, 9, NULL, 0);
    } else {
        printf("input: CH559 UART init failed (USB host disabled; direct GPIO still active)\n");
    }
}

void odroid_input_gamepad_terminate() {}

// No-delay read: merge CH559 (u_*) with direct GPIO. Used by the NES frame loop,
// which paces itself on the composite vblank rather than a fixed input delay.
void odroid_input_poll(odroid_gamepad_state* out_state) {
    if (!out_state) return;
    memset(out_state, 0, sizeof(*out_state));
    out_state->values[ODROID_INPUT_UP]     = u_up     || pressed(PIN_UP);
    out_state->values[ODROID_INPUT_DOWN]   = u_down   || pressed(PIN_DOWN);
    out_state->values[ODROID_INPUT_LEFT]   = u_left   || pressed(PIN_LEFT);
    out_state->values[ODROID_INPUT_RIGHT]  = u_right  || pressed(PIN_RIGHT);
    out_state->values[ODROID_INPUT_A]      = u_a      || pressed(PIN_FIRE);
    out_state->values[ODROID_INPUT_B]      = u_b;
    out_state->values[ODROID_INPUT_START]  = u_start  || pressed(PIN_START);
    out_state->values[ODROID_INPUT_SELECT] = u_select || pressed(PIN_SEL);
    out_state->values[ODROID_INPUT_MENU]   = u_home   || pressed(PIN_HOME);
    out_state->values[ODROID_INPUT_VOLUME] = u_r;
}

// Pad X button (USB pad only) - used to open the in-game menu.
int odroid_input_x_held(void) { return u_x; }

void odroid_input_gamepad_read(odroid_gamepad_state* out_state) {
    // Pace the launcher's poll loop (~100 Hz) so CPU-0 idle runs (feeds the WDT).
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!out_state) return;
    memset(out_state, 0, sizeof(*out_state));

    // CH559 is drained by ch559_task (core 1) into u_*; here we just merge.
    // --- merge: USB pad takes priority, direct GPIO fills the rest ---
    out_state->values[ODROID_INPUT_UP]     = u_up     || pressed(PIN_UP);
    out_state->values[ODROID_INPUT_DOWN]   = u_down   || pressed(PIN_DOWN);
    out_state->values[ODROID_INPUT_LEFT]   = u_left   || pressed(PIN_LEFT);
    out_state->values[ODROID_INPUT_RIGHT]  = u_right  || pressed(PIN_RIGHT);
    out_state->values[ODROID_INPUT_A]      = u_a      || pressed(PIN_FIRE);
    out_state->values[ODROID_INPUT_B]      = u_b;
    out_state->values[ODROID_INPUT_START]  = u_start  || pressed(PIN_START);
    out_state->values[ODROID_INPUT_SELECT] = u_select || pressed(PIN_SEL);
    out_state->values[ODROID_INPUT_MENU]   = u_home   || pressed(PIN_HOME);
    out_state->values[ODROID_INPUT_VOLUME] = u_r;
}

odroid_gamepad_state odroid_input_read_raw() {
    odroid_gamepad_state s;
    memset(&s, 0, sizeof(s));
    s.values[ODROID_INPUT_A]    = pressed(PIN_FIRE);
    s.values[ODROID_INPUT_MENU] = pressed(PIN_HOME);
    return s;
}

void odroid_input_battery_level_init() {}
void odroid_input_battery_level_read(odroid_battery_state* out_state) {
    if (out_state) { out_state->millivolts = 4200; out_state->percentage = 100; }
}
void odroid_input_battery_level_force_voltage(float volts) { (void)volts; }
void odroid_input_battery_monitor_enabled_set(int value) { (void)value; }
