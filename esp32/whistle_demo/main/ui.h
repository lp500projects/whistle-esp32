/* The 1.75C's round screen: input flag > output flag, a status line, and
 * what was heard. Safe to call from any task (each call takes the LVGL lock). */
#pragma once

enum { UI_READY, UI_LISTEN, UI_THINK };

void ui_init(void);
/* READY: a hint, then just an arrow to BOOT; LISTEN: a ring that follows the
 * voice; THINK: a spinner. Each also sets the status line. */
void ui_mode(int m);
void ui_level(float v);          /* mic level 0..1 for the ring; takes no lock */
void ui_status(const char *s);   /* the small grey line: Translating..., errors */
void ui_heard(const char *s);    /* small, above the big text: what was heard ("" hides it) */
void ui_text(const char *s);     /* the big text: the translation, or the transcript */
void ui_note(const char *s);     /* big text that clears itself, like the hint */
int  ui_in_lang(void);           /* input language, an index in nd_whistle's order */
int  ui_out_lang(void);          /* output language, the same */
