#include "ui.h"
#include "bg_image.h"
#include <lvgl.h>
#include "display.h"   /* DISPLAY_WIDTH, DISPLAY_HEIGHT */

/* Impact fonts sized to fit worst-case digit strings within DISPLAY_WIDTH.
 *   1 digit  → 360 px    2 digits → 316 px
 *   3 digits → 211 px    4+ digits → 158 px
 */
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

void ui_init() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Background image */
    lv_obj_t *bg = lv_img_create(scr);
    lv_img_set_src(bg, &bg_image);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_size(bg, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    /* "HAXXcounter" header */
    lv_obj_t *title_box = make_label_backdrop(scr, LV_ALIGN_TOP_MID, 14);
    lv_obj_t *title = lv_label_create(title_box);
    lv_label_set_text(title, "HAXXcounter");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xC0C0FF), LV_PART_MAIN);
    lv_obj_center(title);

    /* Main count label */
    g_count_label = lv_label_create(scr);
    lv_label_set_text(g_count_label, "0");
    lv_obj_set_style_text_font(g_count_label, font_for_count(0), LV_PART_MAIN);
    lv_obj_set_style_text_color(g_count_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(g_count_label);

    /* Footer — shows active mode, updated by ui_set_footer() */
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
