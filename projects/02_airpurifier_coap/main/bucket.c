#include "bucket.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "uuid7.h"

static const char *TAG = "bucket";

static purifier_state_t s_state;

static struct {
    int64_t end_ms; // 0 = no bucket open
    int n;
    int pm25_min, pm25_max, iai_max;
    int64_t pm25_sum;
} s_b;

static int jint(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

static void jcopy(const cJSON *o, const char *k, char *dst, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v)) strlcpy(dst, v->valuestring, cap);
}

int64_t bucket_end_for(int64_t epoch_ms)
{
    return (epoch_ms / BUCKET_MS + 1) * BUCKET_MS;
}

void bucket_on_status(const cJSON *r, int64_t epoch_ms)
{
    // AC22xx "new2" keys (PhilipsAC22xx in kongo09/philips-airpurifier-coap).
    purifier_state_t *st = &s_state;
    jcopy(r, "DeviceId", st->purifier_id, sizeof(st->purifier_id));
    jcopy(r, "D01S03", st->name, sizeof(st->name));
    jcopy(r, "D01S05", st->model, sizeof(st->model));
    jcopy(r, "D01S12", st->fw, sizeof(st->fw));
    st->pwr = jint(r, "D03102");
    st->mode = jint(r, "D0310C");
    st->fan = jint(r, "D0310D");
    st->pm25 = jint(r, "D03221");
    st->iai = jint(r, "D03120");
    st->err = jint(r, "D03240");
    st->prefilter_h = jint(r, "D0520D");
    st->hepa_h = jint(r, "D0540E");
    st->dev_rssi = jint(r, "rssi");
    st->valid = st->purifier_id[0] != '\0';
    if (!st->valid) ESP_LOGW(TAG, "status without DeviceId - not bucketed");
    if (!st->valid || epoch_ms < 0) return;

    int64_t end = bucket_end_for(epoch_ms);
    if (end != s_b.end_ms) {
        if (s_b.end_ms) ESP_LOGD(TAG, "bucket closed end_ms=%lld n=%d", (long long)s_b.end_ms, s_b.n);
        memset(&s_b, 0, sizeof(s_b));
        s_b.end_ms = end;
    }
    if (st->pm25 >= 0) {
        if (s_b.n == 0 || st->pm25 < s_b.pm25_min) s_b.pm25_min = st->pm25;
        if (s_b.n == 0 || st->pm25 > s_b.pm25_max) s_b.pm25_max = st->pm25;
        s_b.pm25_sum += st->pm25;
    }
    if (st->iai > s_b.iai_max) s_b.iai_max = st->iai;
    s_b.n++;
}

const purifier_state_t *bucket_state(void)
{
    return &s_state;
}

// -1 (a key missing from this model's status) goes out as JSON null.
static const char *num(char *buf, size_t cap, int v)
{
    if (v < 0) return "null";
    snprintf(buf, cap, "%d", v);
    return buf;
}

bool bucket_row_json(int64_t end_ms, const char *device_id, char *out, size_t cap, int *n_samples)
{
    const purifier_state_t *st = &s_state;
    if (!st->valid) return false;
    bool have = s_b.end_ms == end_ms && s_b.n > 0;
    int n = have ? s_b.n : 0;
    int pmin = have ? s_b.pm25_min : st->pm25;
    int pmax = have ? s_b.pm25_max : st->pm25;
    int iai = have ? s_b.iai_max : st->iai;
    double mean = have ? (double)s_b.pm25_sum / s_b.n : st->pm25;

    char id[UUID7_STR_LEN];
    uuid7_deterministic(end_ms, device_id, st->purifier_id, strlen(st->purifier_id), id);
    time_t secs = (time_t)(end_ms / 1000);
    struct tm utc;
    gmtime_r(&secs, &utc);
    char ts[24];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);

    char a[9][12];
    char mean_s[16];
    if (st->pm25 < 0 && !have) strcpy(mean_s, "null");
    else snprintf(mean_s, sizeof(mean_s), "%.2f", mean);
    int len = snprintf(out, cap,
        "[{\"id\":\"%s\",\"ts\":\"%s\",\"device_id\":\"%s\",\"purifier_id\":\"%s\","
        "\"pm25_mean\":%s,\"pm25_min\":%s,\"pm25_max\":%s,\"iai_max\":%s,"
        "\"pwr\":%s,\"mode\":%s,\"fan\":%s,\"err\":%s,\"prefilter_h\":%s,\"hepa_h\":%s,"
        "\"dev_rssi\":%d,\"n_samples\":%d}]",
        id, ts, device_id, st->purifier_id, mean_s, num(a[0], 12, pmin), num(a[1], 12, pmax),
        num(a[2], 12, iai), num(a[3], 12, st->pwr), num(a[4], 12, st->mode), num(a[5], 12, st->fan),
        num(a[6], 12, st->err), num(a[7], 12, st->prefilter_h), num(a[8], 12, st->hepa_h),
        st->dev_rssi, n);
    if (len < 0 || (size_t)len >= cap) {
        ESP_LOGE(TAG, "row json truncated need=%d cap=%u", len, (unsigned)cap);
        return false;
    }
    if (n_samples) *n_samples = n;
    return true;
}
