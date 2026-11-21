#include "xiaotai_product.h"
#include "xiaotai/at.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/port/os.h"

#if XIAOTAI_FRAMEWORK_OWNER_ON_MAIN
_Static_assert(CONFIG_ESP_MAIN_TASK_STACK_SIZE >= XIAOTAI_FRAMEWORK_OWNER_STACK_BYTES,
               "The main task stack is smaller than the selected framework owner stack");
#endif

void app_main(void)
{
    (void)xiaotai_start(&xiaotai_product_config);
    (void)xiaotai_at_service(XIAOTAI_WAIT_FOREVER);
}
