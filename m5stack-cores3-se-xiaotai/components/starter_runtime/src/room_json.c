/* A private instance of the existing parser keeps even tiny member/JSON nodes
 * in PSRAM. No new parser fork and no process-global hook mutation. */
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "room_json_prefix.h"

static void *room_json_allocate(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *room_json_resize(void *memory, size_t bytes)
{
    return heap_caps_realloc(memory, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

#define malloc room_json_allocate
#define realloc room_json_resize
/* The room schema is shallow; bound hostile recursion without changing the
 * SDK/platform parser's configuration or the runtime task's stack. */
#undef CJSON_NESTING_LIMIT
#define CJSON_NESTING_LIMIT 12
#include "cJSON.c"
