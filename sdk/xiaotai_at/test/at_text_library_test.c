// SPDX-License-Identifier: MIT
/* The text helpers as every command author links them: through the xiaotai_at
 * library, real reply seam included. A reply that the console did not issue
 * (a debug page running a command directly) gets its lines, and a line that
 * is too long or broken is left out with no other effect. */
#include <stdint.h>

#include "at_test.h"
#include "xiaotai/at_command.h"

int main(void)
{
    at_test_sink_t sink;
    xiaotai_reply_t reply = at_test_reply(&sink);

    xiaotai_at_printf(&reply, "+STATE: started=%d app=%s", 1, "home");
    char long_value[XIAOTAI_AT_OUT_LINE_MAX + 1];
    memset(long_value, 'z', XIAOTAI_AT_OUT_LINE_MAX);
    long_value[XIAOTAI_AT_OUT_LINE_MAX] = '\0';
    xiaotai_at_printf(&reply, "+X: v=%s", long_value);
    xiaotai_at_printf(&reply, "+X: v=%s", "a\nb");
    xiaotai_at_printf(&reply, "+X: v=a%cb", 0);
    xiaotai_at_printf(&reply, "+STATE: uptime_ms=%u", 42U);
    CHECK(sink.writes == 2);
    CHECK(!strcmp(sink.text, "+STATE: started=1 app=home|+STATE: uptime_ms=42"));

    char scratch[12];
    CHECK(!strcmp(xiaotai_at_error_name(XIAOTAI_ERR_NOT_ALLOWED, scratch, sizeof(scratch)), "NOT_ALLOWED"));
    uint32_t value = 0;
    CHECK(xiaotai_at_parse_u32("65535", 0, UINT16_MAX, &value) && value == 65535);
    char text[8];
    CHECK(xiaotai_at_text_decode("412042", text, sizeof(text)) == XIAOTAI_OK && !strcmp(text, "A B"));
    return at_test_result("at_text_library");
}
