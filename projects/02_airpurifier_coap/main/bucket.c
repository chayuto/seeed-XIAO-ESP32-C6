#include "bucket.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "bucket";

static purifier_state_t s_state;

typedef struct {
    int64_t end_ms; // 0 = empty
    int n;
    int pm25_min, pm25_max, iai_max;
    int64_t pm25_sum;
    int pm25_n;
} acc_t;

// The window being filled, and the one before it: a status can open the next window
// before the main loop gets round to closing the previous one.
static acc_t s_cur, s_prev;

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
    // rssi is negative, so -1 can't mean "missing" here; 0 does.
    const cJSON *rs = cJSON_GetObjectItemCaseSensitive(r, "rssi");
    st->dev_rssi = cJSON_IsNumber(rs) ? rs->valueint : 0;
    st->valid = st->purifier_id[0] != '\0';
    if (!st->valid) ESP_LOGW(TAG, "status without DeviceId - not bucketed");
    if (!st->valid || epoch_ms < 0) return;

    int64_t end = bucket_end_for(epoch_ms);
    if (end != s_cur.end_ms) {
        if (s_cur.end_ms) s_prev = s_cur;
        memset(&s_cur, 0, sizeof(s_cur));
        s_cur.end_ms = end;
    }
    if (st->pm25 >= 0) {
        if (s_cur.pm25_n == 0 || st->pm25 < s_cur.pm25_min) s_cur.pm25_min = st->pm25;
        if (s_cur.pm25_n == 0 || st->pm25 > s_cur.pm25_max) s_cur.pm25_max = st->pm25;
        s_cur.pm25_sum += st->pm25;
        s_cur.pm25_n++;
    }
    if (st->iai > s_cur.iai_max) s_cur.iai_max = st->iai;
    s_cur.n++;
}

const purifier_state_t *bucket_state(void)
{
    return &s_state;
}

static const acc_t *find(int64_t end_ms)
{
    if (s_cur.end_ms == end_ms && s_cur.n) return &s_cur;
    if (s_prev.end_ms == end_ms && s_prev.n) return &s_prev;
    return NULL;
}

int bucket_samples(int64_t end_ms)
{
    const acc_t *a = find(end_ms);
    return a ? a->n : 0;
}

bool bucket_close(int64_t end_ms, bool carry_forward, reading_row_t *out)
{
    const purifier_state_t *st = &s_state;
    const acc_t *a = find(end_ms);
    if (!st->valid || (!a && !carry_forward)) return false;

    memset(out, 0, sizeof(*out));
    out->end_ms = end_ms;
    strlcpy(out->purifier_id, st->purifier_id, sizeof(out->purifier_id));
    // Last-known values: what the purifier was doing when the window closed.
    out->pwr = st->pwr;
    out->mode = st->mode;
    out->fan = st->fan;
    out->err = st->err;
    out->prefilter_h = st->prefilter_h;
    out->hepa_h = st->hepa_h;
    out->dev_rssi = st->dev_rssi;
    if (a && a->pm25_n) {
        out->n_samples = a->n;
        out->pm25_mean = (float)a->pm25_sum / a->pm25_n;
        out->pm25_min = a->pm25_min;
        out->pm25_max = a->pm25_max;
        out->iai_max = a->iai_max;
    } else {
        out->n_samples = a ? a->n : 0;
        out->pm25_mean = st->pm25 >= 0 ? (float)st->pm25 : -1.0f;
        out->pm25_min = out->pm25_max = st->pm25;
        out->iai_max = st->iai;
    }
    return true;
}
