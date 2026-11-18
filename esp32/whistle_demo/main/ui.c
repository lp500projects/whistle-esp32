/* Screen for the round 466x466 AMOLED of the Waveshare 1.75C.
 *
 *        [flag]  >  [flag]       tap a flag to change its language,
 *       Spanish     English      tap the arrow to swap them
 *
 *     Hold the button and speak  status
 *      España es la mejor.       what was heard
 *      Spain is the best.     ↘  its translation; the arrow points at BOOT
 *              ( )               voice ring / spinner while busy
 *
 * Flags are drawn from stripes; English gets a simplified USA flag. */
#include <math.h>
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
#define SLEEP_MS 10000             /* screen off after this long with no touch or update */
#define CLEAR_MS 5000              /* the hint and notes clear after this long */
#define BOOT_DEG 35                /* BOOT's place on the rim, degrees below 3 o'clock */
#define BOOT_R   205               /* screen centre to arrow centre */
#define BUSY_Y   395               /* ring / spinner centre, under the text */

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
static lv_obj_t    *s_flag[2], *s_name[2], *s_status, *s_heard, *s_text;
static lv_obj_t    *s_ring, *s_spin, *s_arrow;
static int          s_mode = -1, s_note;   /* -1 while loading: no arrow, no sleep */
static volatile float s_level;

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

/* Touches reset LVGL's idle time, and so does every ui_status/ui_text, so
 * BOOT and new results wake the screen too. Runs in the LVGL task, so it
 * calls lv_* directly, never ui_*. */
static void on_tick(lv_timer_t *t)
{
    static int   asleep, ring;
    static float shown;
    uint32_t     quiet = lv_display_get_inactive_time(NULL);
    int          idle = s_mode == UI_READY && quiet > SLEEP_MS, d;

    (void)t;
    if (s_mode == UI_LISTEN) {
        shown = fmaxf(s_level, shown * 0.85f);   /* jump up, ease down */
        d = 24 + (int)(40 * shown);
        if (d != ring)
            lv_obj_set_size(s_ring, d, d);
        ring = d;
    }
    if (s_mode == UI_READY && quiet > CLEAR_MS && (lv_label_get_text(s_status)[0] || s_note)) {
        lv_label_set_text(s_status, "");
        if (s_note)
            lv_label_set_text(s_text, "");
        s_note = 0;
    }
    if (idle == asleep)
        return;
    asleep = idle;
    bsp_display_brightness_set(idle ? 0 : 100);
    /* while dark, the top layer takes the waking tap so it can't change a flag */
    lv_obj_set_clickable(lv_layer_top(), idle);
    lv_obj_set_hidden(s_arrow, idle || s_mode != UI_READY);   /* no redraws while dark */
}

static void nudge(void *arrow, int32_t v)
{
    lv_obj_set_style_translate_x(arrow, (int32_t)(v * cosf(BOOT_DEG * (float)M_PI / 180)), 0);
    lv_obj_set_style_translate_y(arrow, (int32_t)(v * sinf(BOOT_DEG * (float)M_PI / 180)), 0);
}

/* A 32 px arrow on the rim pointing at BOOT, nudging towards it. */
static void arrow_create(lv_obj_t *scr)
{
    static lv_point_precise_t p[5];   /* tail, tip, one barb, tip, the other */
    float      c = cosf(BOOT_DEG * (float)M_PI / 180), s = sinf(BOOT_DEG * (float)M_PI / 180);
    lv_anim_t  a;

    p[0] = (lv_point_precise_t){ 24 - 16 * c, 24 - 16 * s };
    p[1] = p[3] = (lv_point_precise_t){ 24 + 16 * c, 24 + 16 * s };
    p[2] = (lv_point_precise_t){ p[1].x - 12 * c + 12 * s, p[1].y - 12 * s - 12 * c };
    p[4] = (lv_point_precise_t){ p[1].x - 12 * c - 12 * s, p[1].y - 12 * s + 12 * c };

    s_arrow = lv_line_create(scr);
    lv_line_set_points(s_arrow, p, 5);
    lv_obj_set_size(s_arrow, 48, 48);
    lv_obj_set_pos(s_arrow, CX + (int)(BOOT_R * c) - 24, CX + (int)(BOOT_R * s) - 24);
    lv_obj_set_style_line_width(s_arrow, 5, 0);
    lv_obj_set_style_line_color(s_arrow, lv_color_white(), 0);
    lv_obj_set_style_line_rounded(s_arrow, true, 0);
    lv_obj_set_clickable(s_arrow, false);
    lv_obj_set_hidden(s_arrow, true);

    lv_anim_init(&a);
    lv_anim_set_var(&a, s_arrow);
    lv_anim_set_exec_cb(&a, nudge);
    lv_anim_set_values(&a, 0, 8);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_reverse_duration(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
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
    lv_obj_t *scr, *arrow, *col;
    int       side;
    /* As bsp_display_start(), but LVGL on core 1 next to the nd_worker,
     * which outranks it: drawn in the worker's gaps, the spinner costs
     * recognition 2.5%; from core 0 (no affinity) it cost 11%. */
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg  = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation        = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags     = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 1 },
    };

    cfg.lv_adapter_cfg.task_core_id = 1;
    bsp_display_start_with_config(&cfg);
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

    /* status, heard, text stacked in a column, so the big text moves up when
     * nothing was heard. 330 px wide: the circle is ~334 px across at the
     * third line's depth */
    col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_scrollable(col, false);
    lv_obj_set_size(col, 330, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 232);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 10, 0);

    s_status = label(col, &font_nunito_22, 0x909090);

    s_heard = label(col, &font_nunito_22, 0xC0C0C0);
    lv_obj_set_width(s_heard, 330);
    lv_label_set_long_mode(s_heard, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_heard, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(s_heard, true);

    s_text = label(col, &font_nunito_30, 0xFFFFFF);
    lv_obj_set_width(s_text, 330);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);

    /* Busy indicators sit below the text, not in the column, so the heard
     * line keeps its place. Small, as each frame redraws their box. */
    s_ring = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_clickable(s_ring, false);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ring, 4, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(0xE5484D), 0);
    lv_obj_set_size(s_ring, 24, 24);
    lv_obj_align(s_ring, LV_ALIGN_CENTER, 0, BUSY_Y - CX);
    lv_obj_set_hidden(s_ring, true);

    s_spin = lv_spinner_create(scr);
    lv_obj_set_clickable(s_spin, false);
    lv_obj_set_size(s_spin, 48, 48);
    lv_obj_align(s_spin, LV_ALIGN_CENTER, 0, BUSY_Y - CX);
    lv_obj_set_style_arc_width(s_spin, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spin, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spin, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_hidden(s_spin, true);

    arrow_create(scr);
    lv_timer_create(on_tick, 33, NULL);
    bsp_display_unlock();
}

void ui_mode(int m)
{
    static const char *const STATUS[] = { "Hold the button and speak", "Listening…", "Thinking…" };

    bsp_display_lock(-1);
    s_mode = m;
    s_level = 0.0f;
    lv_label_set_text(s_status, STATUS[m]);
    lv_obj_set_hidden(s_ring, m != UI_LISTEN);
    lv_obj_set_hidden(s_spin, m != UI_THINK);
    lv_obj_set_hidden(s_arrow, m != UI_READY);
    lv_display_trigger_activity(lv_display_get_default());
    bsp_display_unlock();
}

void ui_level(float v) { s_level = v; }

void ui_status(const char *s)
{
    bsp_display_lock(-1);
    lv_label_set_text(s_status, s);
    lv_display_trigger_activity(lv_display_get_default());
    bsp_display_unlock();
}

void ui_heard(const char *s)
{
    bsp_display_lock(-1);
    lv_label_set_text(s_heard, s);
    lv_obj_set_hidden(s_heard, !s[0]);
    bsp_display_unlock();
}

void ui_text(const char *s)
{
    size_t n;

    bsp_display_lock(-1);
    /* three lines of the big font fit inside the circle; past ~60 characters
     * (counting the heard line above) the small one keeps it all on screen.
     * shortcut: a very long clip (~8 s) can still run past the bottom; cap
     * the heard line if that bites */
    n = strlen(s) + strlen(lv_label_get_text(s_heard));
    lv_obj_set_style_text_font(s_text, n > 60 ? &font_nunito_22 : &font_nunito_30, 0);
    lv_label_set_text(s_text, s);
    s_note = 0;
    lv_display_trigger_activity(lv_display_get_default());
    bsp_display_unlock();
}

void ui_note(const char *s)
{
    ui_text(s);
    s_note = 1;
}

int ui_in_lang(void) { return s_lang[0]; }
int ui_out_lang(void) { return s_lang[1]; }
