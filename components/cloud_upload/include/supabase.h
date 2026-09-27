#pragma once
// Insert-only POST to Supabase's REST API (PostgREST) with the publishable key.
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SUPA_OK,        // 201: inserted
    SUPA_DUPLICATE, // 409: a row with that id exists. With deterministic ids that means
                    // "already stored" for a single row - but for a BATCH, one duplicate
                    // rejects the whole statement, new rows included (verified on the
                    // Govee project). Callers batching rows must fall back to one by one.
    SUPA_FAILED,    // network, TLS, or any other HTTP status
} supa_result_t;

bool supabase_configured(void);

// POST `json` (an array of row objects) to /rest/v1/<table>. `rows` is for the log only.
// Blocks for up to ~15 s. Logs status and duration.
supa_result_t supabase_insert(const char *table, const char *json, int rows);

const char *supa_result_str(supa_result_t r);
