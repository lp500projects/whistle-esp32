/* Wi-Fi and DeepL: the translation step after Whistle's transcript. */
#pragma once

#include <stddef.h>

void net_start(void);   /* connects in the background; reconnects on drops */

/* Translate text between two languages in nd_whistle's order. Returns 0 and
 * the translation in out, or -1 and a short reason in out. Blocks ~3 s (a
 * fresh TLS handshake each call). */
int net_translate(const char *text, int from, int to, char *out, size_t n);
