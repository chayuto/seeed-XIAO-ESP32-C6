// Host test, from components/cloud_upload/:
//   cc -Wall -Wextra -I include -o /tmp/t test/test_uuid7.c uuid7.c && /tmp/t
#include <stdio.h>
#include <string.h>
#include "uuid7.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    char a[UUID7_STR_LEN], b[UUID7_STR_LEN], c[UUID7_STR_LEN], d[UUID7_STR_LEN];
    const int64_t ms = 1790000000000LL; // 2026-09
    uuid7_deterministic(ms, "xiao-c6-90dd70", "purifier-a", 10, a);
    uuid7_deterministic(ms, "xiao-c6-90dd70", "purifier-a", 10, b);
    CHECK(strcmp(a, b) == 0);                       // deterministic
    CHECK(strlen(a) == 36 && a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-');
    CHECK(a[14] == '7');                            // version 7
    CHECK(strchr("89ab", a[19]) != NULL);           // RFC variant
    CHECK(uuid7_extract_ms(a) == ms);
    uuid7_deterministic(ms + 180000, "xiao-c6-90dd70", "purifier-a", 10, c);
    CHECK(strcmp(a, c) < 0);                        // later bucket sorts later
    uuid7_deterministic(ms, "xiao-c6-90dd70", "purifier-b", 10, c);
    CHECK(strcmp(a, c) != 0);                       // source matters
    uuid7_deterministic(ms, "xiao-c6-other", "purifier-a", 10, c);
    CHECK(strcmp(a, c) != 0);                       // device matters
    uuid7_deterministic(ms, "ab", "c", 1, c);
    uuid7_deterministic(ms, "a", "bc", 2, d);
    CHECK(strcmp(c, d) != 0);                       // separators prevent aliasing
    printf("%s (%d failures) e.g. %s\n", fails ? "FAILED" : "uuid7 ok", fails, a);
    return fails != 0;
}
