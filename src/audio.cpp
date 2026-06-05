#include "audio.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <ESP_I2S.h>

/* -------------------------------------------------------------------------
   Board pin assignments (Waveshare ESP32-S3-Touch-AMOLED-1.8)
   ------------------------------------------------------------------------- */
#define PIN_I2S_MCLK   16
#define PIN_I2S_BCLK    9
#define PIN_I2S_WS     45
#define PIN_I2S_DOUT    8   /* ESP32 data out → ES8311 DSDIN (speaker path) */
#define PIN_PA         46   /* Speaker-amp enable, active HIGH */

#define ES8311_ADDR   0x18

static const int   SAMPLE_RATE = 16000;
static const float PCM_AMP     = 28000.0f;  /* ~85% of int16_t max — headroom for add */

/* -------------------------------------------------------------------------
   ES8311 register access (shares the existing Wire bus at 0x18)
   All accesses are from the main-loop task — no concurrency with touch driver.
   ------------------------------------------------------------------------- */
static bool es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool es_probe() {
    uint8_t id = 0;
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(0xFD);  /* chip ID register */
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1);
    if (!Wire.available()) return false;
    id = Wire.read();
    Serial.printf("[audio] ES8311 ID = 0x%02X\n", id);
    return true;
}

static bool es_init_dac() {
    if (!es_probe()) {
        Serial.println("[audio] ES8311 not found on I2C bus");
        return false;
    }

    /* Reset → power-on */
    es_write(0x00, 0x1F);
    delay(20);
    es_write(0x00, 0x00);
    es_write(0x00, 0x80);

    /* Clocks: MCLK from the MCLK pin.
     * I2S master (ESP32) drives MCLK = 16000 × 256 = 4.096 MHz.
     * These dividers are from the ES8311 coeff table for (4096000, 16000). */
    es_write(0x01, 0x3F);  /* enable all clocks, select external MCLK pin */
    es_write(0x02, 0x00);  /* pre_div = 1, no pre-multiplier */
    es_write(0x03, 0x10);  /* ADC OSR = 16 */
    es_write(0x04, 0x10);  /* DAC OSR = 16 */
    es_write(0x05, 0x00);  /* ADC/DAC clock dividers = 1 */
    es_write(0x06, 0x03);  /* BCLK divider = 4 */
    es_write(0x07, 0x00);  /* LRCK divider high byte */
    es_write(0x08, 0xFF);  /* LRCK divider low byte */

    /* Serial format: I2S slave, 16-bit */
    es_write(0x09, 0x0C);  /* DAC SDP: 16-bit I2S */
    es_write(0x0A, 0x0C);  /* ADC SDP: 16-bit I2S */

    /* System: power-up DAC output path */
    es_write(0x0D, 0x01);  /* power up analog circuitry */
    es_write(0x0E, 0x02);  /* enable ADC PGA (required for DAC path too) */
    es_write(0x12, 0x00);  /* power up DAC */
    es_write(0x13, 0x10);  /* enable HP output drive */
    es_write(0x1C, 0x6A);  /* ADC equalizer bypass, cancel DC offset */
    es_write(0x37, 0x08);  /* bypass DAC equalizer */

    return true;
}

static void es_set_volume(uint8_t vol) {
    /* REG 0x32: DAC output volume. 0 = mute, 255 = full.
     * Formula from Espressif reference driver: reg = (vol*256/100) - 1. */
    uint8_t reg32 = (vol == 0) ? 0u
                               : (uint8_t)(((int)vol * 256 / 100) - 1);
    es_write(0x32, reg32);
}

/* -------------------------------------------------------------------------
   PCM helpers — stereo interleaved int16 (L == R)
   ------------------------------------------------------------------------- */
static void gen_tone(int16_t *buf, int mono_samples, float freq) {
    const float phase_inc = 2.0f * (float)M_PI * freq / (float)SAMPLE_RATE;
    const int   fade      = min(400, mono_samples / 4);
    for (int i = 0; i < mono_samples; i++) {
        float env = 1.0f;
        if (i < fade)                     env = (float)i / (float)fade;
        else if (i > mono_samples - fade) env = (float)(mono_samples - i) / (float)fade;
        int16_t s        = (int16_t)(PCM_AMP * env * sinf(phase_inc * (float)i));
        buf[2 * i]       = s;
        buf[2 * i + 1]   = s;
    }
}

static void gen_silence(int16_t *buf, int mono_samples) {
    memset(buf, 0, (size_t)mono_samples * 4u);
}

/* -------------------------------------------------------------------------
   PCM buffers (allocated in PSRAM)
   ------------------------------------------------------------------------- */
static int16_t *g_alert_buf   = nullptr;
static size_t   g_alert_bytes = 0;

static void build_alert_pcm() {
    /* Three ascending tones: A4 (440 Hz) → A5 (880 Hz) → E6 (1320 Hz)
     * with brief gaps, totalling 0.5 s. */
    const int T1 = (int)(SAMPLE_RATE * 0.15f);   /* 2400 mono samples */
    const int S1 = (int)(SAMPLE_RATE * 0.03f);   /*  480 */
    const int T2 = T1;
    const int S2 = S1;
    const int T3 = (int)(SAMPLE_RATE * 0.14f);   /* 2240 */
    const int total_mono = T1 + S1 + T2 + S2 + T3;   /* 8000 → 0.5 s */

    const size_t bytes = (size_t)total_mono * 2u * sizeof(int16_t);
    g_alert_buf   = (int16_t *)heap_caps_malloc(bytes,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_alert_bytes = bytes;

    if (!g_alert_buf) {
        Serial.println("[audio] PSRAM alloc failed for alert PCM");
        return;
    }

    int16_t *p = g_alert_buf;
    gen_tone   (p, T1,  440.0f); p += T1 * 2;
    gen_silence(p, S1);          p += S1 * 2;
    gen_tone   (p, T2,  880.0f); p += T2 * 2;
    gen_silence(p, S2);          p += S2 * 2;
    gen_tone   (p, T3, 1320.0f);

    Serial.printf("[audio] alert PCM ready — %u bytes in PSRAM\n",
                  (unsigned)g_alert_bytes);
}

/* -------------------------------------------------------------------------
   Audio FreeRTOS task — owns I2S writes so they never block the main loop
   ------------------------------------------------------------------------- */
enum AudioCmd : uint8_t { CMD_ALERT };

static I2SClass         g_i2s;
static QueueHandle_t    g_queue       = nullptr;
static TaskHandle_t     g_task_handle = nullptr;
static volatile bool    g_playing     = false;

static void audio_task(void *) {
    AudioCmd cmd;
    while (true) {
        if (xQueueReceive(g_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            g_playing = true;
            switch (cmd) {
                case CMD_ALERT:
                    if (g_alert_buf)
                        g_i2s.write((const uint8_t *)g_alert_buf, g_alert_bytes);
                    break;
            }
            g_playing = false;
        }
    }
}

/* -------------------------------------------------------------------------
   Public API
   ------------------------------------------------------------------------- */
void audio_init() {
    pinMode(PIN_PA, OUTPUT);
    digitalWrite(PIN_PA, HIGH);

    g_i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT,
                  /*din=*/ -1,  PIN_I2S_MCLK);
    if (!g_i2s.begin(I2S_MODE_STD, SAMPLE_RATE,
                     I2S_DATA_BIT_WIDTH_16BIT,
                     I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("[audio] I2S begin() failed");
        return;
    }

    delay(50);  /* let MCLK stabilise before talking to codec */

    if (!es_init_dac()) return;
    es_set_volume(70);

    build_alert_pcm();

    g_queue = xQueueCreate(4, sizeof(AudioCmd));
    xTaskCreate(audio_task, "audio", 4096, nullptr, 3, &g_task_handle);

    Serial.println("[audio] ready");
}

void audio_play_alert() {
    if (!g_queue || g_playing || !g_alert_buf) return;
    const AudioCmd cmd = CMD_ALERT;
    xQueueSend(g_queue, &cmd, 0);
}

bool audio_is_playing() { return g_playing; }

void audio_set_volume(uint8_t vol) {
    if (vol > 100) vol = 100;
    es_set_volume(vol);
}
