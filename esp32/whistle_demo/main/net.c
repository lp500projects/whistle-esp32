/* Wi-Fi station and DeepL's v2 /translate (developers.deepl.com). The SSID,
 * password and API key are set in menuconfig (Kconfig.projbuild). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "net.h"

#define GOT_IP      BIT0
#define WAIT_IP_MS  8000   /* the boot self-test can finish before the first connect */

/* nd_whistle's language order: en de fr es it nl pl. DeepL takes plain EN as
 * a source but wants a variant as a target; the English flag is the USA's. */
static const char *const SRC[] = { "EN", "DE", "FR", "ES", "IT", "NL", "PL" };
static const char *const TGT[] = { "EN-US", "DE", "FR", "ES", "IT", "NL", "PL" };

static EventGroupHandle_t s_ev;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT) {
        printf("EVT wifi connected\n");
        xEventGroupSetBits(s_ev, GOT_IP);
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 201 = SSID not found (or 5 GHz only), 15/204 = wrong password */
        printf("EVT wifi disconnected reason=%d\n",
               ((wifi_event_sta_disconnected_t *)data)->reason);
        xEventGroupClearBits(s_ev, GOT_IP);
    }
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)
        esp_wifi_connect();
}

void net_start(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t      wc  = { 0 };
    esp_err_t          err;

    if (!CONFIG_WHISTLE_WIFI_SSID[0]) {
        printf("EVT wifi off (no SSID - set it in idf.py menuconfig)\n");
        return;
    }
    /* Wi-Fi keeps calibration in NVS; a layout left by other firmware on the
     * board (TamaPoke) is wiped. This touches only the nvs partition. */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    strlcpy((char *)wc.sta.ssid, CONFIG_WHISTLE_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_WHISTLE_WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* The screen fonts stop at Latin Extended-A, so DeepL's dashes, curly quotes
 * and French narrow no-break space (U+2013/4, U+2018-201E, U+202F; UTF-8
 * E2 80 xx) become ASCII rather than vanish. In place: it only shrinks. */
static void ascii_punct(char *s)
{
    char *w = s;
    for (; *s; s++) {
        unsigned char c;
        if ((unsigned char)s[0] == 0xE2 && (unsigned char)s[1] == 0x80 &&
            ((c = (unsigned char)s[2]) == 0x93 || c == 0x94 || c == 0xAF ||
             (c >= 0x98 && c <= 0x9E))) {
            *w++ = c == 0xAF ? ' ' : c <= 0x94 ? '-' : c <= 0x9B ? '\'' : '"';
            s += 2;
        } else
            *w++ = *s;
    }
    *w = 0;
}

/* {"translations":[{"detected_source_language":"ES","text":"..."}]} - 8 s of
 * speech translates to a few hundred bytes. */
static char s_resp[2048];
static int  s_len;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && s_len + e->data_len < (int)sizeof(s_resp)) {
        memcpy(s_resp + s_len, e->data, e->data_len);
        s_len += e->data_len;
    }
    return ESP_OK;
}

int net_translate(const char *text, int from, int to, char *out, size_t n)
{
    const char *key = CONFIG_WHISTLE_DEEPL_KEY;
    size_t      kl  = strlen(key);
    char        auth[128], *body;
    cJSON      *req, *res, *t;
    int         status, rc = -1;
    esp_err_t   err;
    int64_t     t0;
    esp_http_client_handle_t c;
    esp_http_client_config_t cfg = {
        /* Free-plan keys end in ":fx" and have their own host */
        .url = kl > 3 && !strcmp(key + kl - 3, ":fx") ? "https://api-free.deepl.com/v2/translate"
                                                      : "https://api.deepl.com/v2/translate",
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = on_http,
        .timeout_ms        = 10000,
        .buffer_size       = 2048,   /* the 512 B default is short for some response headers */
        .buffer_size_tx    = 2048,
    };

    if (!kl) {
        snprintf(out, n, "no DeepL key");
        return -1;
    }
    if (!s_ev || !(xEventGroupWaitBits(s_ev, GOT_IP, pdFALSE, pdTRUE,
                                       pdMS_TO_TICKS(WAIT_IP_MS)) & GOT_IP)) {
        snprintf(out, n, "no Wi-Fi");
        return -1;
    }

    req = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "text"), cJSON_CreateString(text));
    cJSON_AddStringToObject(req, "source_lang", SRC[from]);
    cJSON_AddStringToObject(req, "target_lang", TGT[to]);
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    snprintf(auth, sizeof(auth), "DeepL-Auth-Key %s", key);

    t0    = esp_timer_get_time();
    s_len = 0;
    c     = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, strlen(body));
    err    = esp_http_client_perform(c);
    status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(body);
    s_resp[s_len] = 0;
    printf("EVT deepl status=%d ms=%.0f internal_free=%u\n", status,
           (esp_timer_get_time() - t0) / 1000.0,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (err != ESP_OK)
        snprintf(out, n, "network error, %s", esp_err_to_name(err));
    else if (status == 403)
        snprintf(out, n, "DeepL rejected the key");
    else if (status == 456)
        snprintf(out, n, "DeepL quota used up");
    else if (status == 429)
        snprintf(out, n, "DeepL busy, try again");
    else if (status != 200)
        snprintf(out, n, "DeepL error %d", status);
    else {
        res = cJSON_Parse(s_resp);
        t   = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(res, "translations"), 0),
                                  "text");
        if (cJSON_IsString(t)) {
            snprintf(out, n, "%s", t->valuestring);
            ascii_punct(out);
            rc = 0;
        } else
            snprintf(out, n, "bad reply from DeepL");
        cJSON_Delete(res);
    }
    return rc;
}
