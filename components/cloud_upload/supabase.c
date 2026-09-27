#include "supabase.h"
#include <stdio.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "supabase";

bool supabase_configured(void)
{
    return CONFIG_XIAO_SUPABASE_URL[0] && CONFIG_XIAO_SUPABASE_KEY[0];
}

const char *supa_result_str(supa_result_t r)
{
    switch (r) {
    case SUPA_OK: return "ok";
    case SUPA_DUPLICATE: return "duplicate";
    default: return "failed";
    }
}

typedef struct {
    char buf[160];
    int len;
} body_t;

// perform() consumes the response itself; the body (PostgREST's JSON reason for a 4xx)
// is only visible here.
static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b && b->len < (int)sizeof(b->buf) - 1) {
        int n = e->data_len;
        if (n > (int)sizeof(b->buf) - 1 - b->len) n = (int)sizeof(b->buf) - 1 - b->len;
        memcpy(b->buf + b->len, e->data, n);
        b->len += n;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

supa_result_t supabase_insert(const char *table, const char *json, int rows)
{
    body_t body = {0};
    if (!supabase_configured()) return SUPA_FAILED;
    char url[192];
    snprintf(url, sizeof(url), "%s/rest/v1/%s", CONFIG_XIAO_SUPABASE_URL, table);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http,
        .user_data = &body,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "client init failed");
        return SUPA_FAILED;
    }
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_XIAO_SUPABASE_KEY);
    esp_http_client_set_header(c, "apikey", CONFIG_XIAO_SUPABASE_KEY);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    // return=minimal: returning the inserted row would need SELECT, which this key lacks.
    esp_http_client_set_header(c, "Prefer", "return=minimal");
    esp_http_client_set_post_field(c, json, (int)strlen(json));

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    esp_http_client_cleanup(c);

    supa_result_t r;
    if (err != ESP_OK) r = SUPA_FAILED;
    else if (status == 201 || status == 200 || status == 204) r = SUPA_OK;
    else if (status == 409) r = SUPA_DUPLICATE;
    else r = SUPA_FAILED;
    if (r == SUPA_FAILED) {
        ESP_LOGW(TAG, "insert table=%s rows=%d http=%d err=%s ms=%d body=%s", table, rows, status,
                 esp_err_to_name(err), ms, body.buf);
    } else {
        ESP_LOGI(TAG, "insert table=%s rows=%d http=%d result=%s ms=%d%s%s", table, rows, status,
                 supa_result_str(r), ms, r == SUPA_DUPLICATE ? " body=" : "",
                 r == SUPA_DUPLICATE ? body.buf : "");
    }
    return r;
}
