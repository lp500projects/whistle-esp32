/* Whistle on the Waveshare ESP32-S3-Touch-AMOLED-1.75C: hold BOOT, speak, and
 * the transcript appears on the screen (ui.c), decoded in the input language
 * picked there.
 *
 * Speech recognition runs entirely on the chip. The depth-4 Whistle model
 * (tools/whistle_strip.py, 14.5 MB) lives in the "model" partition, grown to
 * the end of flash (partitions.csv), and is memory-mapped; the output head is
 * copied to octal PSRAM, which streams faster than the flash mapping.
 *
 *   mics     onboard ES7210 (two mics), through the Waveshare BSP
 *   button   BOOT (GPIO0), pulled up (0 = pressed)
 *
 * Out on serial, one line each:
 *   EVT ...     status, levels and per-phase timings
 *   TXT ...     the transcript
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nd_quant.h"
#include "nd_whistle.h"
#include "ui.h"

#define BUTTON_GPIO    0             /* BOOT; GPIO7 is a display line on the 1.75C */
#define MIC_GAIN_DB    24.0f         /* Waveshare's CODEC_DEFAULT_ADC_VOLUME */

#define SAMPLE_RATE    16000
#define MAX_SECS       8             /* the cross-attention cache grows with it */
#define CHUNK          160           /* 10 ms per I2S read */
#define DEBOUNCE_MS    20
#define HEAD_DROP_MS   50            /* press transient: the engine normalises to
                                      * the clip peak, so one click would turn
                                      * every word down */
#define TAIL_DROP_MS   30            /* release transient */
#define FLOOR_WIN      320           /* 20 ms noise-floor windows */
#define FLOOR_HIST     250           /* 5 s of idle history */
#define GATE_DB        12.0f         /* speech must clear the floor by this */
#define READY_MSG      "Hold BOOT and speak"

static const char *const LANGS[] = { "en", "de", "fr", "es", "it", "nl", "pl" };

static nd_whistle s_w;

static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

/* ------------------------------------------------- second-core GEMV worker
 * As needle_demo: every matvec / batched matmul splits its output rows. */

static SemaphoreHandle_t s_go, s_done;
static nd_row_fn         s_fn;
static void             *s_ctx;
static uint32_t          s_r0, s_r1;

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        s_fn(s_ctx, s_r0, s_r1);
        xSemaphoreGive(s_done);
    }
}

static void rows_dual_core(nd_row_fn fn, void *ctx, uint32_t nrows)
{
    uint32_t half = nrows / 2;
    if (half < 2 || !s_go) {
        fn(ctx, 0, nrows);
        return;
    }
    s_fn  = fn;
    s_ctx = ctx;
    s_r0  = half;
    s_r1  = nrows;
    xSemaphoreGive(s_go);
    fn(ctx, 0, half);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

static void worker_start(void)
{
    s_go   = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    /* 16 KB: the Hadamard MLP's rows run here too, ~10 KB of scratch deep. */
    xTaskCreatePinnedToCore(worker_task, "nd_worker", 16384, NULL,
                            configMAX_PRIORITIES - 2, NULL, 1);
    nd_parallel_rows = rows_dual_core;
}

/* -------------------------------------------------------------- microphones */

static esp_codec_dev_handle_t s_mic;

/* ES7210 at 16 kHz, 16-bit, both mics interleaved - the format Waveshare's
 * 05_Spec_Analyzer example opens it in. */
static void mic_init(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE, .channel = 2, .bits_per_sample = 16,
    };
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        printf("ERR mic_init\n");
        abort();
    }
    ESP_ERROR_CHECK(esp_codec_dev_open(s_mic, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB));
}

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

/* Noise floor from the last 5 s of idle audio: the 20th-percentile 20 ms
 * window. The mic runs between presses, so this is known before anyone
 * speaks - better than guessing it from a clip that may be all speech. */
static float s_floor_hist[FLOOR_HIST];
static int   s_floor_n, s_floor_i;

static float noise_floor(void)
{
    static float tmp[FLOOR_HIST];
    int          i, j, n = s_floor_n;
    if (n < 25)
        return 0.0f;                       /* < 0.5 s seen: let the clip decide */
    memcpy(tmp, s_floor_hist, n * sizeof(float));
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && tmp[j - 1] > tmp[j]; j--) {
            float t = tmp[j]; tmp[j] = tmp[j - 1]; tmp[j - 1] = t;
        }
    return tmp[n / 5];
}

/* --------------------------------------------------------------- inference */

static void transcribe(float *x, uint32_t n)
{
    static const char *PN[ND_WP_COUNT] = { "mel", "stem", "encoder", "cross_kv", "decoder",
                                           "~matmul", "~mlp", "~attn", "~mhc" };
    uint32_t s0 = 0, s1 = n, T, ids[96], i;
    float    floor_rms = noise_floor(), peak = 0.0f, *enc;
    int      nt;
    char     text[512];
    int64_t  t0 = esp_timer_get_time(), t_enc, t_dec;

    for (i = 0; i < n; i++)
        if (fabsf(x[i]) > peak)
            peak = fabsf(x[i]);
    printf("EVT clip ms=%u peak_dbfs=%.1f floor_dbfs=%.1f\n", (unsigned)(n / 16),
           peak > 0 ? 20.0f * log10f(peak) : -120.0f,
           floor_rms > 0 ? 20.0f * log10f(floor_rms) : -120.0f);

    if (!nd_whistle_find_speech(x, n, floor_rms, GATE_DB, 300, &s0, &s1)) {
        printf("EVT no_speech (nothing %.0f dB above the noise floor)\n", GATE_DB);
        printf("TXT \n");
        fflush(stdout);
        ui_text("Didn't hear anything");
        return;
    }
    x += s0;
    n  = s1 - s0;
    T  = nd_whistle_frames(&s_w, n);
    printf("EVT speech ms=%u frames=%u\n", (unsigned)(n / 16), (unsigned)T);
    fflush(stdout);

    memset(nd_wprof, 0, sizeof(nd_wprof));
    enc = (float *)heap_caps_malloc((size_t)T * s_w.d * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!enc || nd_whistle_encode(&s_w, x, n, enc) != 0) {
        printf("ERR encode\n");
        heap_caps_free(enc);
        ui_text("Error (encode)");
        return;
    }
    t_enc = esp_timer_get_time();
    /* the language picked on screen, not detection: no wrong-language guesses */
    nt = nd_whistle_decode(&s_w, enc, T, ui_in_lang(), ids, sizeof(ids) / sizeof(ids[0]));
    t_dec = esp_timer_get_time();
    heap_caps_free(enc);
    if (nt < 0) {
        printf("ERR decode\n");
        ui_text("Error (decode)");
        return;
    }
    nd_whistle_detok(&s_w, ids, (uint32_t)nt, text, sizeof(text));

    printf("EVT lang %s\n", LANGS[s_w.lang]);
    printf("TXT %s\n", text);
    printf("EVT time total_ms=%.0f encode_ms=%.0f decode_ms=%.0f tokens=%d audio_ms=%u\n",
           (t_dec - t0) / 1000.0, (t_enc - t0) / 1000.0, (t_dec - t_enc) / 1000.0, nt,
           (unsigned)(n / 16));
    for (i = 0; i < ND_WP_COUNT; i++)
        printf("EVT prof %-9s %8.0f ms\n", PN[i], nd_wprof[i] / 1000.0);

    fflush(stdout);
    ui_text(nt == 0 || !text[0] ? "Didn't hear anything" : text);
}

/* -------------------------------------------------------------------- main */

static void whistle_task(void *arg)
{
    const uint32_t cap = SAMPLE_RATE * MAX_SECS;
    float         *clip = (float *)heap_caps_malloc(cap * sizeof(float), MALLOC_CAP_SPIRAM);
    int16_t        raw[CHUNK * 2];         /* two mics, interleaved */
    uint32_t       n = 0;
    int            recording = 0, stable = 1, last = 1;
    int64_t        changed_at = 0, t_start = 0;
    float          win_e = 0.0f;
    uint32_t       win_n = 0;
    (void)arg;

    if (!clip) {
        printf("ERR clip_alloc\n");
        vTaskDelete(NULL);
    }
#ifdef HAVE_SELFTEST
    {   /* Boot self-test: a known clip through the whole pipeline, so a build
         * proves itself (and prints its timings) before anyone speaks. */
        extern const uint8_t st_start[] asm("_binary_selftest_raw_start");
        extern const uint8_t st_end[] asm("_binary_selftest_raw_end");
        uint32_t k, ns = (uint32_t)(st_end - st_start) / 2;
        for (k = 0; k < ns && k < cap; k++)
            clip[k] = (float)(int16_t)(st_start[2 * k] | st_start[2 * k + 1] << 8) / 32768.0f;
        printf("EVT selftest clip (main/selftest.raw, in the input language on screen)\n");
        transcribe(clip, k);
        fflush(stdout);
    }
#endif
    printf("EVT READY - hold BOOT (GPIO%d) and speak\n", BUTTON_GPIO);
    fflush(stdout);
    ui_status(READY_MSG);

    for (;;) {
        size_t i, m = CHUNK;
        int    lvl;

        if (esp_codec_dev_read(s_mic, raw, sizeof(raw)) != ESP_CODEC_DEV_OK)
            continue;

        lvl = gpio_get_level(BUTTON_GPIO);
        if (lvl != last) {
            last = lvl;
            changed_at = esp_timer_get_time();
        } else if (lvl != stable && esp_timer_get_time() - changed_at >= DEBOUNCE_MS * 1000) {
            stable = lvl;
            if (stable == 0) {
                recording = 1;
                n = 0;
                t_start = esp_timer_get_time();
                ui_status("Listening…");
                ui_text("");
                printf("EVT recording\n");
                fflush(stdout);
            } else if (recording) {
                uint32_t head = HEAD_DROP_MS * 16, tail = TAIL_DROP_MS * 16;
                recording = 0;
                printf("EVT released held_ms=%u\n",
                       (unsigned)((esp_timer_get_time() - t_start) / 1000));
                if (n > head + tail + 1600) {
                    /* the button is not read until this returns, so say so */
                    ui_status("Thinking…");
                    transcribe(clip + head, n - head - tail);
                } else {
                    printf("EVT too_short\nTXT \n");
                    ui_text("Too short - hold BOOT while you speak");
                }
                ui_status(READY_MSG);
                fflush(stdout);
                /* Inference stalled the reads; restart the floor estimate on
                 * fresh audio rather than on whatever piled up in the DMA. */
                win_e = 0.0f;
                win_n = 0;
                continue;
            }
        }

        for (i = 0; i < m; i++) {
            /* The mean of the two mics, as Waveshare's example takes it. */
            float v = (float)(raw[2 * i] + raw[2 * i + 1]) / 65536.0f;
            if (recording) {
                if (n < cap)
                    clip[n++] = v;
            } else {
                win_e += v * v;
                if (++win_n == FLOOR_WIN) {
                    s_floor_hist[s_floor_i] = sqrtf(win_e / FLOOR_WIN);
                    s_floor_i = (s_floor_i + 1) % FLOOR_HIST;
                    if (s_floor_n < FLOOR_HIST)
                        s_floor_n++;
                    win_e = 0.0f;
                    win_n = 0;
                }
            }
        }
        if (recording == 1 && n >= cap) {
            printf("EVT clip_full secs=%d (release to transcribe)\n", MAX_SECS);
            fflush(stdout);
            ui_status("8 s max - let go of BOOT");
            recording = 2;
        }
    }
}

void app_main(void)
{
    const esp_partition_t      *part;
    esp_partition_mmap_handle_t handle;
    const void                 *blob = NULL;
    int                         rc;

    vTaskDelay(pdMS_TO_TICKS(500));
    ui_init();
    ui_status("Loading…");
    worker_start();
    button_init();
    mic_init();

    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "model");
    if (!part) {
        printf("ERR no_model_partition\n");
        ui_status("No model partition");
        return;
    }
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &blob, &handle) != ESP_OK) {
        printf("ERR mmap_failed size=%u\n", (unsigned)part->size);
        ui_status("Model mmap failed");
        return;
    }
    rc = nd_whistle_open(&s_w, blob, part->size);
    if (rc != 0) {
        /* -1..-5: no valid .cact at 0x210000; -6: a .cact that is not v4
         * (e.g. needle2.cact is still flashed there). */
        printf("ERR whistle_open rc=%d (flash whistle_d4.cact to 0x%06x)\n", rc,
               (unsigned)part->address);
        ui_status("No model - flash whistle_d4.cact");
        return;
    }
    {
        int64_t t = esp_timer_get_time();
        size_t  n = nd_whistle_cache_head(&s_w, psram_alloc);
        printf("EVT head_in_psram bytes=%u ms=%.0f\n", (unsigned)n,
               (esp_timer_get_time() - t) / 1000.0);
    }
    printf("EVT ready model=whistle depth=%u enc_layers=%u d_model=%u vocab=%u "
           "psram_free=%u internal_free=%u\n",
           (unsigned)s_w.depth, (unsigned)s_w.enc_layers, (unsigned)s_w.d, (unsigned)s_w.vocab,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    fflush(stdout);

    /* The decoder keeps a few KB of scratch per call level; give it room. */
    xTaskCreatePinnedToCore(whistle_task, "whistle", 48 * 1024, NULL, 5, NULL, 0);
}
