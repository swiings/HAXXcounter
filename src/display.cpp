#include "display.h"
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <lvgl.h>

/* -------------------------------------------------------------------------
   Pin assignments — Waveshare ESP32-S3-Touch-AMOLED-1.8
   ------------------------------------------------------------------------- */
#define PIN_LCD_SDIO0  4
#define PIN_LCD_SDIO1  5
#define PIN_LCD_SDIO2  6
#define PIN_LCD_SDIO3  7
#define PIN_LCD_SCLK   11
#define PIN_LCD_CS     12

#define PIN_I2C_SDA    15
#define PIN_I2C_SCL    14

/* TCA9554 GPIO expander — manages LCD & touch resets */
#define TCA9554_ADDR     0x20
#define TCA9554_REG_OUT  0x01
#define TCA9554_REG_CFG  0x03

/* FT3168 capacitive touch controller
 * Standard address for FT series on Waveshare boards is 0x38.
 * Register map (FT5x / FT6x / FT3168 family):
 *   0x01 : Gesture ID (0x10=up, 0x14=down, 0x18=left, 0x1C=right)
 *   0x02 : TD_STATUS  (touch point count, bits 3:0)
 *   0x03 : P1_XH      (event[7:6], x_hi[3:0])
 *   0x04 : P1_XL      (x_lo[7:0])
 *   0x05 : P1_YH      (id[7:4],   y_hi[3:0])
 *   0x06 : P1_YL      (y_lo[7:0])
 */
#define FT3168_ADDR       0x38
#define FT_REG_GESTURE    0x01
#define FT_SWIPE_UP       0x10
#define FT_SWIPE_DOWN     0x14
#define SWIPE_MIN_DIST    60   /* px — minimum travel to count as a swipe */

/* -------------------------------------------------------------------------
   Statics
   ------------------------------------------------------------------------- */
static Arduino_ESP32QSPI *g_bus = nullptr;
static Arduino_SH8601    *g_gfx = nullptr;

static lv_color_t        *g_buf0 = nullptr;
static lv_color_t        *g_buf1 = nullptr;
static lv_disp_draw_buf_t g_draw_buf;
static lv_disp_drv_t      g_disp_drv;
static lv_indev_drv_t     g_indev_drv;

/* Touch gesture state */
static volatile TouchGesture g_pending_gesture = GESTURE_NONE;
static int16_t  g_touch_start_y  = -1;
static int16_t  g_touch_start_x  = -1;
static int16_t  g_touch_last_y   = -1;
static bool     g_was_touched    = false;

/* Long-press detection */
#define HOLD_THRESHOLD_MS  500
#define HOLD_DEADZONE_PX    12
static uint32_t g_hold_start_ms = 0;
static bool     g_in_long_press = false;

/* Brightness levels — cycles dim→bright, wraps */
static const uint8_t k_brightness[]  = {10, 30, 55, 80, 105, 130, 160, 190, 220, 255};
static const int     k_num_levels    = 10;
static int           g_bright_idx    = 9;    /* start at 255 (full) */
static int           g_bright_dir    = -1;   /* -1 = dimming, +1 = brightening */

/* -------------------------------------------------------------------------
   TCA9554 LCD reset
   ------------------------------------------------------------------------- */
static void tca9554_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static void lcd_reset_sequence() {
    tca9554_write(TCA9554_REG_CFG, 0x00);   /* all outputs */
    tca9554_write(TCA9554_REG_OUT, 0x00);   /* assert reset LOW */
    delay(10);
    tca9554_write(TCA9554_REG_OUT, 0xFF);   /* release HIGH */
    delay(120);
}

/* -------------------------------------------------------------------------
   LVGL display flush
   ------------------------------------------------------------------------- */
static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_p) {
    g_gfx->draw16bitBeRGBBitmap(area->x1, area->y1,
                                 (uint16_t *)color_p,
                                 area->x2 - area->x1 + 1,
                                 area->y2 - area->y1 + 1);
    lv_disp_flush_ready(drv);
}

/* -------------------------------------------------------------------------
   FT3168 touch read callback (called by LVGL every LV_INDEV_DEF_READ_PERIOD)
   ------------------------------------------------------------------------- */
static void touch_read_cb(lv_indev_drv_t * /*drv*/, lv_indev_data_t *data) {
    /* Read 6 registers starting at 0x01 */
    Wire.beginTransmission(FT3168_ADDR);
    Wire.write(FT_REG_GESTURE);
    if (Wire.endTransmission(false) != 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    Wire.requestFrom((uint8_t)FT3168_ADDR, (uint8_t)6);
    if (Wire.available() < 6) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint8_t r[6];
    for (int i = 0; i < 6; i++) r[i] = Wire.read();

    /* r[0]=gesture, r[1]=touch count, r[2]=XH, r[3]=XL, r[4]=YH, r[5]=YL */
    uint8_t hw_gesture = r[0];
    uint8_t touches    = r[1] & 0x0F;

    /* Honour hardware gesture register if available */
    if (hw_gesture == FT_SWIPE_UP)
        g_pending_gesture = GESTURE_SWIPE_UP;
    else if (hw_gesture == FT_SWIPE_DOWN)
        g_pending_gesture = GESTURE_SWIPE_DOWN;

    if (touches == 0) {
        /* Lift — check for software swipe only when not a long-press */
        if (g_was_touched && !g_in_long_press
            && g_pending_gesture == GESTURE_NONE && g_touch_start_y >= 0) {
            int16_t dy = g_touch_last_y - g_touch_start_y;
            if      (dy < -SWIPE_MIN_DIST) g_pending_gesture = GESTURE_SWIPE_UP;
            else if (dy >  SWIPE_MIN_DIST) g_pending_gesture = GESTURE_SWIPE_DOWN;
        }
        g_was_touched   = false;
        g_in_long_press = false;
        g_hold_start_ms = 0;
        g_touch_start_y = -1;
        g_touch_start_x = -1;
        data->state     = LV_INDEV_STATE_RELEASED;
        return;
    }

    int16_t x = ((r[2] & 0x0F) << 8) | r[3];
    int16_t y = ((r[4] & 0x0F) << 8) | r[5];
    x = constrain(x, 0, DISPLAY_WIDTH  - 1);
    y = constrain(y, 0, DISPLAY_HEIGHT - 1);

    if (!g_was_touched) {
        g_touch_start_y = y;
        g_touch_start_x = x;
        g_hold_start_ms = millis();
        g_in_long_press = false;
        g_was_touched   = true;
    } else if (!g_in_long_press) {
        /* Invalidate long-press if finger drifted outside deadzone */
        if (abs(x - g_touch_start_x) > HOLD_DEADZONE_PX ||
            abs(y - g_touch_start_y) > HOLD_DEADZONE_PX) {
            g_hold_start_ms = 0;
        } else if (g_hold_start_ms &&
                   (millis() - g_hold_start_ms) >= HOLD_THRESHOLD_MS) {
            g_in_long_press = true;
        }
    }

    g_touch_last_y  = y;
    data->point.x   = x;
    data->point.y   = y;
    data->state     = LV_INDEV_STATE_PRESSED;
}

/* -------------------------------------------------------------------------
   Public API
   ------------------------------------------------------------------------- */
void display_init() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);

    lcd_reset_sequence();

    /* Probe for FT3168 and log result */
    Wire.beginTransmission(FT3168_ADDR);
    bool touch_ok = (Wire.endTransmission() == 0);
    Serial.printf("[display] FT3168 @ 0x%02X: %s\n",
                  FT3168_ADDR, touch_ok ? "found" : "NOT FOUND");

    /* QSPI bus → SH8601 AMOLED */
    g_bus = new Arduino_ESP32QSPI(
        PIN_LCD_CS, PIN_LCD_SCLK,
        PIN_LCD_SDIO0, PIN_LCD_SDIO1, PIN_LCD_SDIO2, PIN_LCD_SDIO3);
    g_gfx = new Arduino_SH8601(g_bus, GFX_NOT_DEFINED,
                                0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!g_gfx->begin())
        Serial.println("[display] Arduino_SH8601::begin() failed");
    g_gfx->fillScreen(0x0000);

    /* LVGL */
    lv_init();
    const size_t buf_px = (size_t)DISPLAY_WIDTH * (DISPLAY_HEIGHT / 4);
    g_buf0 = (lv_color_t *)heap_caps_malloc(
        buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_buf1 = (lv_color_t *)heap_caps_malloc(
        buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&g_draw_buf, g_buf0, g_buf1, buf_px);

    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.hor_res  = DISPLAY_WIDTH;
    g_disp_drv.ver_res  = DISPLAY_HEIGHT;
    g_disp_drv.flush_cb = disp_flush_cb;
    g_disp_drv.draw_buf = &g_draw_buf;
    lv_disp_drv_register(&g_disp_drv);

    lv_indev_drv_init(&g_indev_drv);
    g_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    g_indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&g_indev_drv);

    g_gfx->setBrightness(k_brightness[g_bright_idx]);
    Serial.printf("[display] %dx%d AMOLED ready, LVGL %d.%d.%d\n",
                  DISPLAY_WIDTH, DISPLAY_HEIGHT,
                  LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
}

TouchGesture display_pop_gesture() {
    TouchGesture g    = g_pending_gesture;
    g_pending_gesture = GESTURE_NONE;
    return g;
}

bool display_long_press_active() {
    return g_in_long_press;
}

void display_step_brightness() {
    g_bright_idx += g_bright_dir;
    if (g_bright_idx >= k_num_levels - 1) { g_bright_idx = k_num_levels - 1; g_bright_dir = -1; }
    else if (g_bright_idx <= 0)           { g_bright_idx = 0;                g_bright_dir = +1; }
    uint8_t level = k_brightness[g_bright_idx];
    g_gfx->setBrightness(level);
    Serial.printf("[display] brightness → %u\n", level);
}


