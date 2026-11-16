/* The 1.75C's round screen: input flag > output flag, a status line, and
 * what was heard. Safe to call from any task (each call takes the LVGL lock). */
#pragma once

void ui_init(void);
void ui_status(const char *s);   /* the small grey line: Listening..., Thinking... */
void ui_heard(const char *s);    /* small, above the big text: what was heard ("" hides it) */
void ui_text(const char *s);     /* the big text: the translation, or the transcript */
int  ui_in_lang(void);           /* input language, an index in nd_whistle's order */
int  ui_out_lang(void);          /* output language, the same */
