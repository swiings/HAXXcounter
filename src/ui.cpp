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

/* ---- main labels ---- */
static lv_obj_t *g_count_label  = nullptr;
static lv_obj_t *g_footer_label = nullptr;
static char      g_count_buf[16];

/* ---- edge indicator fills ---- */
static lv_obj_t *g_rssi_fill = nullptr;
static lv_obj_t *g_bat_fill  = nullptr;

/* ---- RSSI overlay ---- */
static lv_obj_t   *g_rssi_box   = nullptr;
static lv_obj_t   *g_rssi_label = nullptr;
static lv_obj_t   *g_rssi_bar   = nullptr;
static lv_timer_t *g_rssi_timer = nullptr;

/* =========================================================================
   Helpers
   ========================================================================= */
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

/* Create a thin edge strip.  Returns the fill child object. */
static lv_obj_t *make_edge_strip(lv_obj_t *parent, int x,
                                  lv_color_t track_col, lv_color_t fill_col) {
    constexpr int W = 3;

    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_set_pos(track, x, 0);
    lv_obj_set_size(track, W, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(track, track_col, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(track, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(track, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(track, 0, LV_PART_MAIN);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_set_style_bg_color(fill, fill_col, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(fill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(fill, 0, LV_PART_MAIN);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    /* Start empty — callers set position and height */
    lv_obj_set_size(fill, W, 0);
    lv_obj_set_pos(fill, 0, DISPLAY_HEIGHT);

    return fill;
}

/* =========================================================================
   RSSI overlay (lazy-created so it sits on top in z-order)
   ========================================================================= */
static void rssi_overlay_create() {
    lv_obj_t *scr = lv_scr_act();

    g_rssi_box = lv_obj_create(scr);
    lv_obj_set_style_bg_color(g_rssi_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_rssi_box, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_rssi_box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_rssi_box, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_rssi_box, 16, LV_PART_MAIN);
    lv_obj_set_size(g_rssi_box, 280, LV_SIZE_CONTENT);
    lv_obj_center(g_rssi_box);
    lv_obj_clear_flag(g_rssi_box, LV_OBJ_FLAG_SCROLLABLE);

    g_rssi_label = lv_label_create(g_rssi_box);
    lv_obj_set_style_text_font(g_rssi_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_rssi_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_width(g_rssi_label, LV_SIZE_CONTENT);
    lv_obj_align(g_rssi_label, LV_ALIGN_TOP_MID, 0, 0);

    /* Bar: RSSI_MIN (far/loose) on left → RSSI_MAX (close/tight) on right */
    g_rssi_bar = lv_bar_create(g_rssi_box);
    lv_obj_set_size(g_rssi_bar, 240, 18);
    lv_obj_align(g_rssi_bar, LV_ALIGN_TOP_MID, 0, 36);
    lv_bar_set_range(g_rssi_bar, RSSI_MIN, RSSI_MAX);
    lv_obj_set_style_bg_color(g_rssi_bar, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_rssi_bar, lv_color_hex(0x4080FF),
                               LV_PART_INDICATOR);

    /* Labels: "far" on left (loose, -95), "close" on right (tight, -60) */
    lv_obj_t *lbl_far = lv_label_create(g_rssi_box);
    lv_label_set_text(lbl_far, "far");
    lv_obj_set_style_text_font(lbl_far, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_far, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_align(lbl_far, LV_ALIGN_TOP_LEFT, 0, 62);

    lv_obj_t *lbl_close = lv_label_create(g_rssi_box);
    lv_label_set_text(lbl_close, "close");
    lv_obj_set_style_text_font(lbl_close, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_close, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_align(lbl_close, LV_ALIGN_TOP_RIGHT, 0, 62);

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

    /* Background image */
    lv_obj_t *bg = lv_img_create(scr);
    lv_img_set_src(bg, &bg_image);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_size(bg, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    /* Left edge — RSSI threshold indicator (dim teal) */
    g_rssi_fill = make_edge_strip(scr, 0,
                                   lv_color_hex(0x0A1015),  /* track: near-black */
                                   lv_color_hex(0x1A6080)); /* fill: dim teal    */

    /* Right edge — battery indicator (colour set dynamically) */
    g_bat_fill = make_edge_strip(scr, DISPLAY_WIDTH - 3,
                                  lv_color_hex(0x0A1010),   /* track: near-black */
                                  lv_color_hex(0x206020));  /* fill: dim green   */

    /* Header */
    lv_obj_t *title_box = make_label_backdrop(scr, LV_ALIGN_TOP_MID, 14);
    lv_obj_t *title = lv_label_create(title_box);
    lv_label_set_text(title, "HAXXcounter");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xC0C0FF), LV_PART_MAIN);
    lv_obj_center(title);

    /* Count */
    g_count_label = lv_label_create(scr);
    lv_label_set_text(g_count_label, "0");
    lv_obj_set_style_text_font(g_count_label, font_for_count(0), LV_PART_MAIN);
    lv_obj_set_style_text_color(g_count_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(g_count_label);

    /* Footer */
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

    char buf[24];
    snprintf(buf, sizeof(buf), "signal: %d dBm", dbm);
    lv_label_set_text(g_rssi_label, buf);
    lv_bar_set_value(g_rssi_bar, dbm, LV_ANIM_ON);
    lv_obj_clear_flag(g_rssi_box, LV_OBJ_FLAG_HIDDEN);

    if (g_rssi_timer) lv_timer_reset(g_rssi_timer);
    else {
        g_rssi_timer = lv_timer_create(rssi_hide_cb, 2000, nullptr);
        lv_timer_set_repeat_count(g_rssi_timer, 1);
    }
}

void ui_update_rssi_indicator(int dbm) {
    if (!g_rssi_fill) return;
    /* Fill bottom-to-top: empty = far/loose (-95), full = close/tight (-60) */
    float pct = (float)(dbm - RSSI_MIN) / (float)(RSSI_MAX - RSSI_MIN);
    pct = constrain(pct, 0.0f, 1.0f);
    int h = (int)(pct * DISPLAY_HEIGHT);
    lv_obj_set_pos(g_rssi_fill, 0, DISPLAY_HEIGHT - h);
    lv_obj_set_height(g_rssi_fill, h);
}

void ui_update_battery_indicator(int pct) {
    if (!g_bat_fill || pct < 0) return;
    int h = (pct * DISPLAY_HEIGHT) / 100;
    lv_obj_set_pos(g_bat_fill, 0, DISPLAY_HEIGHT - h);
    lv_obj_set_height(g_bat_fill, h);

    /* Colour shifts: green → amber → red as battery depletes */
    lv_color_t col;
    if      (pct > 50) col = lv_color_hex(0x206020);
    else if (pct > 20) col = lv_color_hex(0x705020);
    else               col = lv_color_hex(0x702020);
    lv_obj_set_style_bg_color(g_bat_fill, col, LV_PART_MAIN);
}
