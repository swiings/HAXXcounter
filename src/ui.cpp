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

/* ---- edge indicator fills (updated by ui_update_* functions) ---- */
static lv_obj_t *g_rssi_fill = nullptr;
static lv_obj_t *g_bat_fill  = nullptr;

/* ---- cached values so tap callbacks can show current readings ---- */
static int g_last_rssi_dbm = -80;
static int g_last_bat_pct  = -1;

/* ---- RSSI overlay ---- */
static lv_obj_t   *g_rssi_box   = nullptr;
static lv_obj_t   *g_rssi_label = nullptr;
static lv_obj_t   *g_rssi_bar   = nullptr;
static lv_timer_t *g_rssi_timer = nullptr;

/* ---- battery overlay ---- */
static lv_obj_t   *g_bat_box    = nullptr;
static lv_obj_t   *g_bat_label  = nullptr;
static lv_obj_t   *g_bat_bar    = nullptr;
static lv_timer_t *g_bat_timer  = nullptr;

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

/* Create a thin edge strip. Returns the fill child.
 * out_track (optional): receives the track object so the caller can
 * register a click handler on it. */
static lv_obj_t *make_edge_strip(lv_obj_t *parent, int x,
                                  lv_color_t track_col, lv_color_t fill_col,
                                  lv_obj_t **out_track = nullptr) {
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
    /* Extend the hit area so the 3 px strip is actually tappable */
    lv_obj_set_ext_click_area(track, 11);   /* 3 + 11 + 11 = 25 px total */

    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_set_style_bg_color(fill, fill_col, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(fill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(fill, 0, LV_PART_MAIN);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    /* Fill must not be independently clickable or touches on it won't
     * propagate to the track's click handler. */
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(fill, W, 0);
    lv_obj_set_pos(fill, 0, DISPLAY_HEIGHT);

    if (out_track) *out_track = track;
    return fill;
}

/* =========================================================================
   Generic overlay builder (shared between RSSI and battery popups)
   ========================================================================= */
static lv_obj_t *make_overlay_box(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_style_bg_color(box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 16, LV_PART_MAIN);
    lv_obj_set_size(box, 280, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    return box;
}

static lv_obj_t *make_overlay_bar(lv_obj_t *parent, int min_v, int max_v,
                                   lv_color_t fill_col) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 240, 18);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 36);
    lv_bar_set_range(bar, min_v, max_v);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, fill_col, LV_PART_INDICATOR);
    return bar;
}

static lv_obj_t *make_overlay_end_label(lv_obj_t *parent, const char *text,
                                         lv_align_t align) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_align(lbl, align, 0, 62);
    return lbl;
}

/* =========================================================================
   RSSI overlay
   ========================================================================= */
static void rssi_overlay_create() {
    lv_obj_t *scr = lv_scr_act();
    g_rssi_box = make_overlay_box(scr);

    g_rssi_label = lv_label_create(g_rssi_box);
    lv_obj_set_style_text_font(g_rssi_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_rssi_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_width(g_rssi_label, LV_SIZE_CONTENT);
    lv_obj_align(g_rssi_label, LV_ALIGN_TOP_MID, 0, 0);

    /* far (-95, loose) on left → close (-60, tight) on right */
    g_rssi_bar = make_overlay_bar(g_rssi_box, RSSI_MIN, RSSI_MAX,
                                   lv_color_hex(0x4080FF));
    make_overlay_end_label(g_rssi_box, "far",   LV_ALIGN_TOP_LEFT);
    make_overlay_end_label(g_rssi_box, "close", LV_ALIGN_TOP_RIGHT);
}

static void rssi_hide_cb(lv_timer_t * /*t*/) {
    if (g_rssi_box) lv_obj_add_flag(g_rssi_box, LV_OBJ_FLAG_HIDDEN);
    g_rssi_timer = nullptr;
}

static void on_rssi_strip_tap(lv_event_t * /*e*/) {
    ui_show_rssi_overlay(g_last_rssi_dbm);
}

/* =========================================================================
   Battery overlay
   ========================================================================= */
static void bat_overlay_create() {
    lv_obj_t *scr = lv_scr_act();
    g_bat_box = make_overlay_box(scr);

    g_bat_label = lv_label_create(g_bat_box);
    lv_obj_set_style_text_font(g_bat_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_bat_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_width(g_bat_label, LV_SIZE_CONTENT);
    lv_obj_align(g_bat_label, LV_ALIGN_TOP_MID, 0, 0);

    g_bat_bar = make_overlay_bar(g_bat_box, 0, 100,
                                  lv_color_hex(0x40C040));
    make_overlay_end_label(g_bat_box, "0%",   LV_ALIGN_TOP_LEFT);
    make_overlay_end_label(g_bat_box, "100%", LV_ALIGN_TOP_RIGHT);
}

static void bat_hide_cb(lv_timer_t * /*t*/) {
    if (g_bat_box) lv_obj_add_flag(g_bat_box, LV_OBJ_FLAG_HIDDEN);
    g_bat_timer = nullptr;
}

static void on_bat_strip_tap(lv_event_t * /*e*/) {
    ui_show_battery_overlay(g_last_bat_pct);
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

    /* Left edge — RSSI indicator, tap shows RSSI overlay */
    lv_obj_t *rssi_track = nullptr;
    g_rssi_fill = make_edge_strip(scr, 0,
                                   lv_color_hex(0x0A1015),
                                   lv_color_hex(0x1A6080),
                                   &rssi_track);
    lv_obj_add_event_cb(rssi_track, on_rssi_strip_tap, LV_EVENT_CLICKED, nullptr);

    /* Right edge — battery indicator, tap shows battery overlay */
    lv_obj_t *bat_track = nullptr;
    g_bat_fill = make_edge_strip(scr, DISPLAY_WIDTH - 3,
                                  lv_color_hex(0x0A1010),
                                  lv_color_hex(0x206020),
                                  &bat_track);
    lv_obj_add_event_cb(bat_track, on_bat_strip_tap, LV_EVENT_CLICKED, nullptr);

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

void ui_show_battery_overlay(int pct) {
    if (!g_bat_box) bat_overlay_create();
    char buf[20];
    if (pct < 0) snprintf(buf, sizeof(buf), "battery: --");
    else         snprintf(buf, sizeof(buf), "battery: %d%%", pct);
    lv_label_set_text(g_bat_label, buf);
    if (pct >= 0) {
        lv_bar_set_value(g_bat_bar, pct, LV_ANIM_ON);
        lv_color_t col = (pct > 50) ? lv_color_hex(0x40C040)
                       : (pct > 20) ? lv_color_hex(0xC08020)
                                    : lv_color_hex(0xC02020);
        lv_obj_set_style_bg_color(g_bat_bar, col, LV_PART_INDICATOR);
    }
    lv_obj_clear_flag(g_bat_box, LV_OBJ_FLAG_HIDDEN);
    if (g_bat_timer) lv_timer_reset(g_bat_timer);
    else {
        g_bat_timer = lv_timer_create(bat_hide_cb, 2000, nullptr);
        lv_timer_set_repeat_count(g_bat_timer, 1);
    }
}

void ui_update_rssi_indicator(int dbm) {
    g_last_rssi_dbm = dbm;   /* cache for tap callback */
    if (!g_rssi_fill) return;
    float pct = (float)(dbm - RSSI_MIN) / (float)(RSSI_MAX - RSSI_MIN);
    pct = constrain(pct, 0.0f, 1.0f);
    int h = (int)(pct * DISPLAY_HEIGHT);
    lv_obj_set_pos(g_rssi_fill, 0, DISPLAY_HEIGHT - h);
    lv_obj_set_height(g_rssi_fill, h);
}

void ui_update_battery_indicator(int pct) {
    g_last_bat_pct = pct;    /* cache for tap callback */
    if (!g_bat_fill || pct < 0) return;
    int h = (pct * DISPLAY_HEIGHT) / 100;
    lv_obj_set_pos(g_bat_fill, 0, DISPLAY_HEIGHT - h);
    lv_obj_set_height(g_bat_fill, h);
    lv_color_t col = (pct > 50) ? lv_color_hex(0x206020)
                   : (pct > 20) ? lv_color_hex(0x705020)
                                : lv_color_hex(0x702020);
    lv_obj_set_style_bg_color(g_bat_fill, col, LV_PART_MAIN);
}
