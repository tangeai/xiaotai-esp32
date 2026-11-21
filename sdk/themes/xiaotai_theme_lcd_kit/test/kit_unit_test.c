// SPDX-License-Identifier: MIT
/* theme_lcd_kit_unit: the kit's lifecycle unit. Its init succeeds on every
 * call and never reaches the port allocator (the kit holds no state of its
 * own; a theme's start allocates what the kit draws with), so selecting the
 * kit costs a colour product nothing at start-up and cannot fail it. */
#include <stdio.h>
#include <stdlib.h>
#include "xiaotai/theme_lcd.h"
#include "xiaotai_mem_host.h"

static int failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

int main(void)
{
    xiaotai_mem_host_clear();
    CHECK(xiaotai_engine_theme_lcd_kit_init() == XIAOTAI_OK);
    /* A framework restart runs init again: same answer, still nothing taken. */
    CHECK(xiaotai_engine_theme_lcd_kit_init() == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_allocations() == 0);
    CHECK(xiaotai_mem_host_outstanding() == 0);

    /* With the next port allocation set to fail, init still succeeds and the
     * injected failure stays pending: init does not allocate at all. */
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_engine_theme_lcd_kit_init() == XIAOTAI_OK);
    CHECK(xiaotai_mem_host_fail_pending());
    CHECK(xiaotai_mem_host_allocations() == 0);
    xiaotai_mem_host_clear();

    if (failures) {
        fprintf(stderr, "FAIL: theme_lcd_kit_unit: %d checks failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("PASS: theme_lcd_kit_unit\n");
    return EXIT_SUCCESS;
}
