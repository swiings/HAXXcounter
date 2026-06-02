#include "ui.h"
#include "bg_image.h"
#include "counter.h"   /* RSSI_MIN, RSSI_MAX */
#include <lvgl.h>
#include "display.h"

extern const lv_font_t font_counter;
extern const lv_font_t font_counter_2d;
extern const lv_font_t font_counter_3d;
extern const lv_font_t font_counter_4d;

static const lv_font_t *font_for_count(uint32_t n) {
    if (n < 10)   return &font_counter;
    if (n < 100)  return &font_counter_2d;
    if (n < 1000) return &font_counter_3d;
    return        &font_counter_4d;
}

static lv_obj_t *g_count_label  = nullptr;
static lv_obj_t *g_footer_label = nullptr;
static char      g_count_buf[16];

/* ---- RSSI overlay statics ---- */
static lv_obj_t   *g_rssi_box   = nullptr;   /* semi-transparent container */
static lv_obj_t   *g_rssi_label = nullptr;   /* dBm value text */
static lv_obj_t   *g_rssi_bar   = nullptr;   /* position-in-range bar */
static lv_timer_t *g_rssi_timer = nullptr;   /* auto-hide timer */

static lv_obj_t *make_label_backdrop(lv_obj_t *parent,
                                     lv_align_t align, int32_t y_ofs) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_style_bg_color(box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(box, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(box, 6,  LV_PART_MAIN);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(box, align, 0, y_ofs);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

/* Build the RSSI overlay on first use (lazy, so it sits on top in z-order) */
static void rssi_overlay_create() {
    lv_obj_t *scr = lv_scr_act();

    /* Outer box — semi-transparent black, rounded */
    g_rssi_box = lv_obj_create(scr);
    lv_obj_set_style_bg_color(g_rssi_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_rssi_box, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_rssi_box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_rssi_box, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_rssi_box, 16, LV_PART_MAIN);
    lv_obj_set_size(g_rssi_box, 280, LV_SIZE_CONTENT);
    lv_obj_center(g_rssi_box);
    lv_obj_clear_flag(g_rssi_box, LV_OBJ_FLAG_SCROLLABLE);

    /* dBm value label */
    g_rssi_label = lv_label_create(g_rssi_box);
    lv_obj_set_style_text_font(g_rssi_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_rssi_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_width(g_rssi_label, LV_SIZE_CONTENT);
    lv_obj_align(g_rssi_label, LV_ALIGN_TOP_MID, 0, 0);

    /* Range bar — fills left=close(-60) → right=far(-95)
     * We invert the range so a higher (less negative) threshold fills more */
    g_rssi_bar = lv_bar_create(g_rssi_box);
    lv_obj_set_size(g_rssi_bar, 240, 18);
    lv_obj_align(g_rssi_bar, LV_ALIGN_TOP_MID, 0, 36);
    lv_bar_set_range(g_rssi_bar, RSSI_MIN, RSSI_MAX);
    lv_obj_set_style_bg_color(g_rssi_bar, lv_color_hex(0x404040),
                               LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_rssi_bar, lv_color_hex(0x4080FF),
                               LV_PART_INDICATOR);

    /* Range endpoint labels */
    lv_obj_t *lbl_close = lv_label_create(g_rssi_box);
    lv_label_set_text(lbl_close, "close");
    lv_obj_set_style_text_font(lbl_close, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_close, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_align(lbl_close, LV_ALIGN_TOP_LEFT, 0, 62);

    lv_obj_t *lbl_far = lv_label_create(g_rssi_box);
    lv_label_set_text(lbl_far, "far");
    lv_obj_set_style_text_font(lbl_far, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_far, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_align(lbl_far, LV_ALIGN_TOP_RIGHT, 0, 62);

    lv_obj_add_flag(g_rssi_box, LV_OBJ_FLAG_HIDDEN);
}

static void rssi_hide_cb(lv_timer_t * /*t*/) {
    if (g_rssi_box) lv_obj_add_flag(g_rssi_box, LV_OBJ_FLAG_HIDDEN);
    g_rssi_timer = nullptr;
}

/* =========================================================================
   Public API
   ========================================================================= */
void ui_init() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *bg = lv_img_create(scr);
    lv_img_set_src(bg, &bg_image);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_size(bg, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    lv_obj_t *title_box = make_label_backdrop(scr, LV_ALIGN_TOP_MID, 14);
    lv_obj_t *title = lv_label_create(title_box);
    lv_label_set_text(title, "HAXXcounter");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xC0C0FF), LV_PART_MAIN);
    lv_obj_center(title);

    g_count_label = lv_label_create(scr);
    lv_label_set_text(g_count_label, "0");
    lv_obj_set_style_text_font(g_count_label, font_for_count(0), LV_PART_MAIN);
    lv_obj_set_style_text_color(g_count_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(g_count_label);

    lv_obj_t *footer_box = make_label_backdrop(scr, LV_ALIGN_BOTTOM_MID, -14);
    g_footer_label = lv_label_create(footer_box);
    lv_label_set_text(g_footer_label, "all devices");
    lv_obj_set_style_text_font(g_footer_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_footer_label, lv_color_hex(0x80C0FF), LV_PART_MAIN);
    lv_obj_center(g_footer_label);
}

void ui_set_count(uint32_t count) {
    if (!g_count_label) return;
    snprintf(g_count_buf, sizeof(g_count_buf), "%lu", (unsigned long)count);
    lv_obj_set_style_text_font(g_count_label, font_for_count(count), LV_PART_MAIN);
    lv_label_set_text(g_count_label, g_count_buf);
    lv_obj_center(g_count_label);
}

void ui_set_footer(const char *text) {
    if (!g_footer_label) return;
    lv_label_set_text(g_footer_label, text);
    lv_obj_center(g_footer_label);
}

void ui_show_rssi_overlay(int dbm) {
    if (!g_rssi_box) rssi_overlay_create();

    /* Update label and bar */
    char buf[24];
    snprintf(buf, sizeof(buf), "signal: %d dBm", dbm);
    lv_label_set_text(g_rssi_label, buf);
    lv_bar_set_value(g_rssi_bar, dbm, LV_ANIM_ON);

    lv_obj_clear_flag(g_rssi_box, LV_OBJ_FLAG_HIDDEN);

    /* Reset or start the auto-hide timer (2 s) */
    if (g_rssi_timer) {
        lv_timer_reset(g_rssi_timer);
    } else {
        g_rssi_timer = lv_timer_create(rssi_hide_cb, 2000, nullptr);
        lv_timer_set_repeat_count(g_rssi_timer, 1);
    }
}
