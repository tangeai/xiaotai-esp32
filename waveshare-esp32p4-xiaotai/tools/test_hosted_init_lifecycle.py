"""Check actual Hosted failure propagation and task-delete pairing with host stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1] / 'components/espressif__esp_hosted/host'
port = (root/'port/src/os_wrapper.c').read_text(encoding='utf-8')
transport = (root/'drivers/transport/transport_drv.c').read_text(encoding='utf-8')
sdio = (root/'drivers/transport/sdio/sdio_drv.c').read_text(encoding='utf-8')

def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start)+2]

reader = function(sdio, 'static void sdio_read_task(void const* pvParameters)\n{')
failure = reader[reader.index('res = g_h.funcs->_h_sdio_card_init'):reader.index('create_debugging_tasks();')]
assert 'transport_drv_report_init_error(res)' in failure
assert 'for (;;) vTaskSuspend(NULL);' in failure and 'return;' not in failure
api = (root/'api/src/esp_hosted_api.c').read_text(encoding='utf-8')
assert 'ESP_ERROR_CHECK' not in function(api, 'esp_err_t esp_wifi_remote_init(')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
typedef void *TaskHandle_t;
typedef int esp_err_t;
typedef struct {TaskHandle_t task;bool with_caps;} hosted_thread_t;
#define RET_OK 0
#define RET_INVALID -1
#define pdTRUE 1
#define ESP_OK 0
#define TRANSPORT_INACTIVE 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define H_TRANSPORT_IN_USE 1
#define H_TRANSPORT_SDIO 1
#define ESP_LOGE(...) ((void)0)
static int allocation_mode,handles,caps_deleted,normal_deleted,cleanups;
static void *hosted_malloc(size_t size) {
    if(allocation_mode==3) return NULL;
    ++handles; return malloc(size);
}
static void hosted_free(void *p) { assert(p); --handles; free(p); }
#define HOSTED_FREE(p) do {if(p) {hosted_free(p);p=NULL;}} while(0)
static int xTaskCreateWithCaps(void (*fn)(void*),char *name,uint32_t stack,void *arg,
                                uint32_t priority,TaskHandle_t *task,uint32_t caps) {
    (void)fn;(void)name;(void)stack;(void)arg;(void)priority; assert(caps==3);
    *task=allocation_mode==0?(void*)1:NULL; return allocation_mode==0;
}
static int xTaskCreate(void (*fn)(void*),char *name,uint32_t stack,void *arg,
                        uint32_t priority,TaskHandle_t *task) {
    (void)fn;(void)name;(void)stack;(void)arg;(void)priority;
    *task=allocation_mode==1?(void*)2:NULL; return allocation_mode==1;
}
static void vTaskDeleteWithCaps(TaskHandle_t task) {assert(task==(void*)1);++caps_deleted;}
static void vTaskDelete(TaskHandle_t task) {assert(task==(void*)2);++normal_deleted;}
static void routine(void const *arg) {(void)arg;}
static atomic_uchar transport_state;
static atomic_int transport_init_error;
static atomic_bool failed_init_cleaned;
static void transport_cleanup_failed_init(void) {++cleanups;}
'''
code += function(port, 'void *hosted_thread_create(')
code += function(port, 'int hosted_thread_cancel(')
code += function(transport, 'void transport_drv_report_init_error(')
code += function(transport, 'static esp_err_t transport_check_init_error(')
code += r'''
int main(void) {
    assert(hosted_thread_create("t",1,4096,NULL,NULL)==NULL && handles==0);
    for(allocation_mode=0;allocation_mode<4;allocation_mode++) {
        void *thread=hosted_thread_create("t",1,4096,routine,NULL);
        if(allocation_mode<2) {
            assert(thread && handles==1); assert(hosted_thread_cancel(thread)==RET_OK);
        } else assert(thread==NULL);
        assert(handles==0);
    }
    assert(caps_deleted==1 && normal_deleted==1);
    assert(hosted_thread_cancel(NULL)==RET_INVALID);
    assert(transport_check_init_error()==ESP_OK && cleanups==0);
    transport_drv_report_init_error(258);
    transport_drv_report_init_error(ESP_OK);
    transport_drv_report_init_error(-7);
    assert(transport_check_init_error()==258 && cleanups==1 && transport_state==0);
    assert(transport_check_init_error()==258 && cleanups==1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='p4-hosted-init-') as tmp:
    p=Path(tmp)
    (p/'test.c').write_text(code,encoding='utf-8')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: creation-kind cleanup, allocation failure, first error preserved and one-shot cleanup')
