/* Screen for the round 466x466 AMOLED of the Waveshare 1.75C.
 *
 *        [flag]  >  [flag]       tap a flag to change its language,
 *       Spanish     English      tap the arrow to swap them
 *
 *        Hold BOOT and speak     status
 *      Spain is the best.        what was heard
 *
 * Flags are drawn from stripes; English gets a simplified USA flag. */
#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "nd_whistle.h"
#include "ui.h"

LV_FONT_DECLARE(font_nunito_22);   /* Nunito 700 with Latin-1 + Latin Extended-A, */
LV_FONT_DECLARE(font_nunito_30);   /* so ñ, é, ü, ß, ł, ż all render */

#define CX      (BSP_LCD_H_RES / 2)
#define FW      120                /* flag size */
#define FH      80
#define FLAG_Y  140                /* flag centre */
#define FLAG_DX 100                /* screen centre to flag centre */

/* nd_whistle's language order: en de fr es it nl pl. Stripe weights run
 * along the stripe axis (Spain's yellow band is twice the red ones). */
static const struct {
    const char *name;
    uint8_t     vertical, n, w[3];
    uint32_t    col[3];
} LANG[ND_WH_LANGS] = {
    { "English", 0, 0, { 0 }, { 0 } },                                  /* USA, drawn apart */
    { "German",  0, 3, { 1, 1, 1 }, { 0x000000, 0xDD0000, 0xFFCE00 } },
    { "French",  1, 3, { 1, 1, 1 }, { 0x0055A4, 0xFFFFFF, 0xEF4135 } },
    { "Spanish", 0, 3, { 1, 2, 1 }, { 0xAA151B, 0xF1BF00, 0xAA151B } },
    { "Italian", 1, 3, { 1, 1, 1 }, { 0x009246, 0xFFFFFF, 0xCE2B37 } },
    { "Dutch",   0, 3, { 1, 1, 1 }, { 0xAE1C28, 0xFFFFFF, 0x21468B } },
    { "Polish",  0, 2, { 1, 1 },    { 0xFFFFFF, 0xDC143C } },
};

static volatile int s_lang[2] = { 3, 0 };   /* Spanish > English */
static lv_obj_t    *s_flag[2], *s_name[2], *s_status, *s_text;

static void rect(lv_obj_t *p, int x, int y, int w, int h, uint32_t c)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_set_clickable(r, false);   /* taps go to the flag */
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
}

static void draw_flag(int side)
{
    int       l = s_lang[side], i, j;
    lv_obj_t *f = s_flag[side];

    lv_obj_clean(f);
    if (l == 0) {   /* USA: 13 stripes, a blue canton, 4 x 5 stars */
        for (i = 0; i < 13; i++)
            rect(f, 0, FH * i / 13, FW, FH * (i + 1) / 13 - FH * i / 13,
                 i & 1 ? 0xFFFFFF : 0xB22234);
        rect(f, 0, 0, FW * 2 / 5, FH * 7 / 13, 0x3C3B6E);
        for (i = 0; i < 4; i++)
            for (j = 0; j < 5; j++)
                rect(f, 4 + j * 9, 4 + i * 9, 3, 3, 0xFFFFFF);
    } else {
        int len = LANG[l].vertical ? FW : FH, total = 0, acc = 0;
        for (i = 0; i < LANG[l].n; i++)
            total += LANG[l].w[i];
        for (i = 0; i < LANG[l].n; i++) {
            int a = len * acc / total, b = len * (acc += LANG[l].w[i]) / total;
            if (LANG[l].vertical)
                rect(f, a, 0, b - a, FH, LANG[l].col[i]);
            else
                rect(f, 0, a, FW, b - a, LANG[l].col[i]);
        }
    }
    lv_label_set_text(s_name[side], LANG[l].name);
}

static void on_flag(lv_event_t *e)
{
    int side = (int)(intptr_t)lv_event_get_user_data(e);
    s_lang[side] = (s_lang[side] + 1) % ND_WH_LANGS;
    draw_flag(side);
}

static void on_swap(lv_event_t *e)
{
    int t = s_lang[0];
    (void)e;
    s_lang[0] = s_lang[1];
    s_lang[1] = t;
    draw_flag(0);
    draw_flag(1);
}

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *font, uint32_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_label_set_text(l, "");
    return l;
}

void ui_init(void)
{
    lv_obj_t *scr, *arrow;
    int       side;

    bsp_display_start();
    bsp_display_backlight_on();
    bsp_display_lock(-1);   /* negative = wait forever; 0 would not wait */
    scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scrollable(scr, false);

    for (side = 0; side < 2; side++) {
        int dx = side ? FLAG_DX : -FLAG_DX;
        lv_obj_t *f = s_flag[side] = lv_obj_create(scr);
        lv_obj_remove_style_all(f);
        lv_obj_set_scrollable(f, false);
        lv_obj_set_size(f, FW, FH);
        lv_obj_set_pos(f, CX + dx - FW / 2, FLAG_Y - FH / 2);
        lv_obj_set_style_outline_width(f, 1, 0);   /* Poland's white half on black */
        lv_obj_set_style_outline_color(f, lv_color_hex(0x606060), 0);
        lv_obj_add_event_cb(f, on_flag, LV_EVENT_CLICKED, (void *)(intptr_t)side);
        s_name[side] = label(scr, &font_nunito_22, 0xFFFFFF);
        lv_obj_align(s_name[side], LV_ALIGN_TOP_MID, dx, FLAG_Y + FH / 2 + 8);
        draw_flag(side);
    }

    arrow = label(scr, &font_nunito_30, 0xFFFFFF);
    lv_label_set_text(arrow, ">");
    lv_obj_align(arrow, LV_ALIGN_TOP_MID, 0, FLAG_Y - 20);
    lv_obj_set_clickable(arrow, true);
    lv_obj_set_ext_click_area(arrow, 24);
    lv_obj_add_event_cb(arrow, on_swap, LV_EVENT_CLICKED, NULL);

    s_status = label(scr, &font_nunito_22, 0x909090);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 232);

    /* 330 px wide: the circle is ~334 px across at the third line's depth */
    s_text = label(scr, &font_nunito_30, 0xFFFFFF);
    lv_obj_set_width(s_text, 330);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_text, LV_ALIGN_TOP_MID, 0, 272);
    bsp_display_unlock();
}

void ui_status(const char *s)
{
    bsp_display_lock(-1);
    lv_label_set_text(s_status, s);
    bsp_display_unlock();
}

void ui_text(const char *s)
{
    bsp_display_lock(-1);
    /* three lines of the big font fit inside the circle; past ~60 characters
     * the small one keeps a whole sentence on screen */
    lv_obj_set_style_text_font(s_text, strlen(s) > 60 ? &font_nunito_22 : &font_nunito_30, 0);
    lv_label_set_text(s_text, s);
    bsp_display_unlock();
}

int ui_in_lang(void) { return s_lang[0]; }
