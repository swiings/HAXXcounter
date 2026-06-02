#include "display.h"
#include <Arduino_GFX_Library.h>
#include <Wire.h>

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
#define PIN_TP_INT     21   /* FT3168 touch interrupt */

/* TCA9554 GPIO expander — manages LCD & touch resets on this board.
   Address 0x20 = A2/A1/A0 all tied to GND on Waveshare schematic. */
#define TCA9554_ADDR   0x20
#define TCA9554_REG_OUT  0x01
#define TCA9554_REG_CFG  0x03

/* -------------------------------------------------------------------------
   Statics
   ------------------------------------------------------------------------- */
static Arduino_ESP32QSPI *g_bus = nullptr;
static Arduino_SH8601    *g_gfx = nullptr;

/* LVGL draw buffer — two halves in PSRAM for DMA double-buffering */
static lv_color_t *g_buf0 = nullptr;
static lv_color_t *g_buf1 = nullptr;
static lv_disp_draw_buf_t g_draw_buf;
static lv_disp_drv_t      g_disp_drv;
static lv_indev_drv_t     g_indev_drv;

/* -------------------------------------------------------------------------
   TCA9554 helper — assert / deassert LCD reset via I2C expander
   ------------------------------------------------------------------------- */
static void tca9554_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static void lcd_reset_sequence() {
    /* Configure all TCA9554 pins as outputs */
    tca9554_write(TCA9554_REG_CFG, 0x00);
    /* Assert reset LOW (active-low) */
    tca9554_write(TCA9554_REG_OUT, 0x00);
    delay(10);
    /* Release reset HIGH */
    tca9554_write(TCA9554_REG_OUT, 0xFF);
    delay(120);  /* SH8601 requires ≥120 ms after reset release */
}

/* -------------------------------------------------------------------------
   LVGL callbacks
   ------------------------------------------------------------------------- */
static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_p) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    g_gfx->draw16bitBeRGBBitmap(area->x1, area->y1,
                                 (uint16_t *)color_p, w, h);
    lv_disp_flush_ready(drv);
}

/* Touch read stub — FT3168 not yet wired up; always reports released */
static void touch_read_cb(lv_indev_drv_t * /*drv*/, lv_indev_data_t *data) {
    // TODO: read FT3168 via Wire, populate data->point and data->state
    data->state = LV_INDEV_STATE_RELEASED;
}

/* -------------------------------------------------------------------------
   Public API
   ------------------------------------------------------------------------- */
void display_init() {
    /* I2C — used by TCA9554 expander (LCD reset) and FT3168 touch */
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);

    lcd_reset_sequence();

    /* QSPI bus → SH8601 AMOLED */
    g_bus = new Arduino_ESP32QSPI(
        PIN_LCD_CS, PIN_LCD_SCLK,
        PIN_LCD_SDIO0, PIN_LCD_SDIO1, PIN_LCD_SDIO2, PIN_LCD_SDIO3
    );
    g_gfx = new Arduino_SH8601(g_bus, GFX_NOT_DEFINED /*rst via expander*/,
                                0 /*rotation*/,
                                DISPLAY_WIDTH, DISPLAY_HEIGHT);

    if (!g_gfx->begin()) {
        Serial.println("[display] Arduino_SH8601::begin() failed");
    }
    g_gfx->fillScreen(0x0000 /*BLACK, RGB565*/);

    /* ------------------------------------------------------------------
       LVGL init
       ------------------------------------------------------------------ */
    lv_init();

    /* Allocate two half-screen draw buffers in PSRAM for smooth rendering */
    const size_t buf_px = (size_t)DISPLAY_WIDTH * (DISPLAY_HEIGHT / 4);
    g_buf0 = (lv_color_t *)heap_caps_malloc(
        buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_buf1 = (lv_color_t *)heap_caps_malloc(
        buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    lv_disp_draw_buf_init(&g_draw_buf, g_buf0, g_buf1, buf_px);

    /* Display driver */
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.hor_res  = DISPLAY_WIDTH;
    g_disp_drv.ver_res  = DISPLAY_HEIGHT;
    g_disp_drv.flush_cb = disp_flush_cb;
    g_disp_drv.draw_buf = &g_draw_buf;
    lv_disp_drv_register(&g_disp_drv);

    /* Touch input device stub */
    lv_indev_drv_init(&g_indev_drv);
    g_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    g_indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&g_indev_drv);

    Serial.printf("[display] %dx%d AMOLED ready, LVGL %d.%d.%d\n",
                  DISPLAY_WIDTH, DISPLAY_HEIGHT,
                  LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
}
