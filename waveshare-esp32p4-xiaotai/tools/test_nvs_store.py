#!/usr/bin/env python3
"""Failure/ownership tests of the real common NVS worker (host RTOS/NVS stubs)."""
from pathlib import Path
import re
from test_runtime_resources import COMMON, ROOT, function, run_case


def without_includes(source):
    return re.sub(r"(?m)^#(?:include|pragma).*\n", "", source)


def main():
    header = (ROOT / 'components/nvs_store/include/nvs_store.h').read_text(encoding='utf-8')
    source = (ROOT / 'components/nvs_store/src/nvs_store.c').read_text(encoding='utf-8')
    text = (ROOT / 'components/starter_product/src/starter_product.c').read_text(encoding='utf-8')
    harness = COMMON + r'''
#define ESP_ERR_INVALID_SIZE 260
#define ESP_ERR_NOT_FOUND 261
#define ESP_ERR_NOT_FINISHED 262
#define ESP_ERR_TIMEOUT 263
#define ESP_ERR_INVALID_RESPONSE 264
#define ESP_ERR_NVS_NOT_FOUND 4354
#define ESP_ERR_NVS_INVALID_LENGTH 4364
#define EXT_RAM_BSS_ATTR
#define pdMS_TO_TICKS(n) (n)
#define NVS_READWRITE 1
#define NVS_READONLY 0
typedef unsigned StaticSemaphore_t;
typedef int nvs_handle_t;
static size_t strnlen(const char *s,size_t n) { size_t i=0; while(i<n && s[i]) ++i; return i; }
static void *xSemaphoreCreateMutexStatic(void *p);
static int xSemaphoreTake(void *p,unsigned wait);
static void xSemaphoreGive(void *p);
static void vSemaphoreDelete(void *p);
static void *xQueueCreateStatic(unsigned count,unsigned bytes,void *data,void *storage);
static int xQueueReceive(void *p,void *out,unsigned wait);
static int xQueueSend(void *p,const void *in,unsigned wait);
static void vQueueDelete(void *p);
static void *xTaskCreateStaticPinnedToCore(void (*fn)(void *),const char *name,
    unsigned bytes,void *arg,int pri,void *stack,void *control,int core);
static void *xTaskGetCurrentTaskHandle(void);
static unsigned uxTaskGetStackHighWaterMark(void *task) { return 2048; }
static void vTaskDelay(unsigned ms);
static int64_t esp_timer_get_time(void);
static int nvs_open(const char *name,int mode,nvs_handle_t *out);
static int nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *value);
static int nvs_get_str(nvs_handle_t h,const char *key,char *value,size_t *size);
static int nvs_get_blob(nvs_handle_t h,const char *key,void *value,size_t *size);
static int nvs_set_u8(nvs_handle_t h,const char *key,unsigned value);
static int nvs_set_str(nvs_handle_t h,const char *key,const char *value);
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size);
static int nvs_erase_key(nvs_handle_t h,const char *key);
static int nvs_erase_all(nvs_handle_t h);
static int nvs_commit(nvs_handle_t h);
static void nvs_close(nvs_handle_t h);
''' + without_includes(header) + without_includes(source) + r'''
static unsigned queued, first, init_stage, fail_init, op_number, fail_op, closed, task_creates;
static uint8_t queue_data[8];
static bool locked, inside_worker, blocked_write;
static int64_t clock_us;
static jmp_buf drained;
static void (*during_write)(void);
static unsigned committed, values[8], write_index;
static unsigned latest_values[8];
static uint8_t blob[512];
static size_t blob_size;
static esp_err_t read_error;
static const char *read_failure_key;
static bool read_fails(const char *key) { return read_error && (!read_failure_key || !strcmp(key,read_failure_key)); }
static bool allocate_ok(void) { return ++init_stage != fail_init; }
static void *xSemaphoreCreateMutexStatic(void *p) { return allocate_ok() ? p : NULL; }
static int xSemaphoreTake(void *p,unsigned wait) {
    if (locked) { assert(wait==0); return 0; } locked=true; return pdTRUE;
}
static void xSemaphoreGive(void *p) { assert(locked); locked=false; }
static void vSemaphoreDelete(void *p) { assert(!locked); }
static void *xQueueCreateStatic(unsigned count,unsigned bytes,void *data,void *p) {
    assert(count==8 && bytes==1); return allocate_ok() ? p : NULL;
}
static int xQueueSend(void *p,const void *in,unsigned wait) {
    if (queued==8) return 0;
    queue_data[(first+queued++)%8]=*(const uint8_t *)in; return pdTRUE;
}
static int xQueueReceive(void *p,void *out,unsigned wait) {
    if (!queued) { assert(wait==portMAX_DELAY); longjmp(drained,1); }
    *(uint8_t *)out=queue_data[first++%8]; --queued; return pdTRUE;
}
static void vQueueDelete(void *p) { assert(!queued); }
static void *xTaskCreateStaticPinnedToCore(void (*fn)(void *),const char *name,
    unsigned bytes,void *arg,int pri,void *stack,void *p,int core) {
    assert(bytes==3072 && fn==store_task && stack==s_stack && core==0);
    if (!allocate_ok()) return NULL;
    ++task_creates; return p;
}
static void *xTaskGetCurrentTaskHandle(void) { return inside_worker ? s_task : NULL; }
static void drain(void) {
    inside_worker=true;
    if (setjmp(drained)==0) store_task(NULL);
    inside_worker=false;
}
static void vTaskDelay(unsigned ms) { clock_us+=ms*1000; if(!blocked_write) drain(); }
static int64_t esp_timer_get_time(void) { return clock_us; }
static int step(void) { return ++op_number==fail_op ? 777 : ESP_OK; }
static int nvs_open(const char *name,int mode,nvs_handle_t *out) {
    assert(inside_worker && !locked && name==s_active.namespace_name);
    write_index=0; *out=1; return step();
}
static int nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *value) {
    assert(inside_worker && value==s_active.data && !locked);
    if (read_fails(key)) return read_error;
    *value=41; return step();
}
static int nvs_get_str(nvs_handle_t h,const char *key,char *value,size_t *size) {
    assert(inside_worker && (uint8_t *)value==s_active.data && !locked);
    if (read_fails(key)) return read_error;
    size_t capacity=*size; *size=9;
    if(capacity<9) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(value,"snapshot",9); return step();
}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *value,size_t *size) {
    assert(inside_worker && value==s_active.data && !locked);
    if (read_fails(key)) return read_error;
    size_t capacity=*size;
    bool credentials=!strcmp(key,"credentials");
    *size=credentials ? blob_size : 96;
    if(capacity<*size) return ESP_ERR_NVS_INVALID_LENGTH;
    if(credentials) memcpy(value,blob,*size); else memset(value,0x5a,96);
    return step();
}
static int nvs_set_u8(nvs_handle_t h,const char *key,unsigned value) {
    if (during_write) { void (*fn)(void)=during_write; during_write=NULL; fn(); }
    values[write_index++]=value; return step();
}
static int nvs_set_str(nvs_handle_t h,const char *key,const char *value) {
    assert((const uint8_t *)value>=s_active.data && (const uint8_t *)value<s_active.data+512);
    return step();
}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size) {
    assert(value==s_active.data); memcpy(blob,value,size); blob_size=size; return step();
}
static int nvs_erase_key(nvs_handle_t h,const char *key) { return step()==ESP_OK ? ESP_ERR_NVS_NOT_FOUND : 777; }
static int nvs_erase_all(nvs_handle_t h) { return step(); }
static int nvs_commit(nvs_handle_t h) {
    int err=step(); if(!err) { ++committed; memcpy(latest_values,values,sizeof(values)); } return err;
}
static void nvs_close(nvs_handle_t h) { ++closed; }
static void assert_wiped(void) {
    const uint8_t *bytes=(const uint8_t *)s_jobs;
    for(size_t i=0;i<sizeof(s_jobs);++i) assert(bytes[i]==0);
    bytes=(const uint8_t *)&s_active;
    for(size_t i=0;i<sizeof(s_active);++i) assert(bytes[i]==0);
}
static void assert_free(void) { for(unsigned i=0;i<8;++i) assert(s_slots[i].state==SLOT_FREE); }
static nvs_store_ticket_t during_ticket;
static void abandon_running(void) {
    esp_err_t result;
    assert(nvs_store_take_result(during_ticket,&result)==ESP_ERR_NOT_FINISHED);
    assert(nvs_store_abandon(during_ticket)==ESP_OK);
    assert(s_slots[during_ticket&7].state==SLOT_RUNNING);
}
'''
    prefs = re.search(r'typedef struct \{\s+uint8_t volume;.*?} product_preferences_t;', text, re.S).group()
    harness += prefs + r'''
#define PRODUCT_NVS "product"
static product_preferences_t s_preferences;
static nvs_store_ticket_t s_preferences_ticket;
static bool s_preferences_pending;
static int64_t s_preferences_enqueue_until_ms;
static atomic_int s_preferences_error;
static int64_t monotonic_ms(void) { return clock_us/1000; }
'''
    harness += '\n'.join(function(text, name) for name in (
        'preferences_submit','preferences_poll','preferences_save'))
    config = (ROOT / 'components/runtime_config/src/runtime_config.c').read_text(encoding='utf-8')
    config_header = (ROOT / 'components/runtime_config/include/runtime_config.h').read_text(encoding='utf-8')
    wifi = (ROOT / 'components/wifi_manager/src/wifi_manager.c').read_text(encoding='utf-8')
    wifi_header = (ROOT / 'components/wifi_manager/include/wifi_manager.h').read_text(encoding='utf-8')
    harness += re.search(r'typedef struct \{.*?} runtime_tirtc_config_t;', config_header, re.S).group()
    harness += '\n#define TIRTC_NVS_NAMESPACE "tirtc_cfg"\n'
    harness += '\n'.join(function(config, name) for name in (
        'set_error', 'runtime_config_tirtc_valid', 'runtime_config_save_tirtc', 'runtime_config_clear_tirtc',
        'runtime_config_load_tirtc'))
    harness += '\n#define WIFI_MANAGER_SSID_MAX 32\n#define WIFI_MANAGER_PASSWORD_MAX 64\n'
    harness += '\n#define WIFI_NVS_NAMESPACE "wifi_sta"\n#define WIFI_NVS_CREDENTIALS "credentials"\n'
    harness += '\n#define WIFI_CREDENTIALS_VERSION 1U\n'
    harness += '\n#define WIFI_NVS_SSID "ssid"\n#define WIFI_NVS_PASSWORD "password"\n'
    harness += re.search(r'typedef struct \{.*?} wifi_manager_credentials_t;', wifi_header, re.S).group()
    harness += re.search(r'typedef struct \{\s+uint8_t version;.*?} wifi_credentials_record_t;', wifi, re.S).group()
    harness += '\n'.join(function(wifi, name) for name in (
        'wifi_manager_credentials_valid', 'wifi_manager_save_credentials', 'wifi_manager_forget_credentials',
        'wifi_manager_load_credentials'))
    harness += r'''
int main(void) {
    uint8_t v=7;
    nvs_store_op_t op={NVS_STORE_U8,"volume",&v,1};
    nvs_store_ticket_t ticket, tickets[8]; esp_err_t result=0;
    assert(nvs_store_submit("test",&op,1,&ticket)==ESP_ERR_INVALID_STATE);
    for(fail_init=1;fail_init<=3;++fail_init) {
        init_stage=0; assert(nvs_store_init()==ESP_ERR_NO_MEM && s_task==NULL);
        assert(s_lock==NULL && s_queue==NULL);
    }
    fail_init=0; init_stage=0;
    assert(nvs_store_init()==ESP_OK && nvs_store_init()==ESP_OK && task_creates==1);
    assert(nvs_store_submit("1234567890123456",&op,1,&ticket)==ESP_ERR_INVALID_ARG);
    nvs_store_op_t bad={NVS_STORE_STRING,"key","oops",4};
    assert(nvs_store_submit("test",&bad,1,&ticket)==ESP_ERR_INVALID_ARG);
    uint8_t original[512]; memset(original,0xab,sizeof(original));
    nvs_store_op_t big={NVS_STORE_BLOB,"record",original,513};
    assert(nvs_store_submit("test",&big,1,&ticket)==ESP_ERR_INVALID_SIZE);
    big.size=512;
    assert(nvs_store_submit("test",&big,1,&ticket)==ESP_OK);
    memset(original,0,sizeof(original)); drain();
    assert(blob_size==512 && blob[0]==0xab && blob[511]==0xab);
    assert(nvs_store_take_result(ticket,&result)==ESP_OK && result==ESP_OK);
    assert(nvs_store_take_result(ticket,&result)==ESP_ERR_NOT_FOUND);
    assert_wiped(); assert_free();
    for(unsigned i=0;i<8;++i) assert(nvs_store_submit("test",&op,1,&tickets[i])==ESP_OK);
    assert(nvs_store_submit("test",&op,1,&ticket)==ESP_ERR_NO_MEM);
    assert(nvs_store_abandon(tickets[0])==ESP_OK);
    assert(nvs_store_submit("test",&op,1,&ticket)==ESP_ERR_NO_MEM); /* not freed while queued */
    drain();
    assert(nvs_store_submit("test",&op,1,&ticket)==ESP_OK);
    assert(nvs_store_abandon(tickets[0])==ESP_ERR_NOT_FOUND); /* stale generation */
    drain(); assert(nvs_store_take_result(ticket,&result)==ESP_OK);
    for(unsigned i=1;i<8;++i) assert(nvs_store_take_result(tickets[i],&result)==ESP_OK);
    assert_free(); assert_wiped();
    assert(nvs_store_submit("test",&op,1,&during_ticket)==ESP_OK);
    during_write=abandon_running; drain(); assert_free(); assert_wiped();
    /* All supported types, first failing NVS op, and actual commit result. */
    const nvs_store_op_t batch[]={op,{NVS_STORE_STRING,"name","hi",3},
        {NVS_STORE_ERASE_KEY,"old",NULL,0},{NVS_STORE_ERASE_ALL,NULL,NULL,0}};
    for(fail_op=1;fail_op<=6;++fail_op) {
        op_number=closed=0;
        assert(nvs_store_execute("test",batch,4,5000)==777);
        assert(op_number==fail_op && closed==(fail_op!=1)); assert_free(); assert_wiped();
    }
    fail_op=0; assert(nvs_store_execute("test",batch,4,5000)==ESP_OK);
    blocked_write=true;
    assert(nvs_store_execute("test",&op,1,10)==ESP_ERR_TIMEOUT);
    blocked_write=false; drain(); assert_free(); assert_wiped();
    /* Product coalesces inputs without mutating a submitted job. */
    unsigned begin=committed;
    s_preferences=(product_preferences_t){.volume=1,.sleep_index=2,.emoji_index=3};
    preferences_save(); assert(s_preferences_ticket);
    for(unsigned i=0;i<10000;++i) { s_preferences.volume=i%11; preferences_save(); }
    unsigned last=s_preferences.volume;
    drain(); assert(committed==begin+1 && latest_values[0]==1);
    preferences_poll(); drain(); preferences_poll();
    assert(committed==begin+2 && latest_values[0]==last && latest_values[4]==2 && latest_values[5]==3);
    assert(!s_preferences_pending && !s_preferences_ticket && task_creates==1);
    for(unsigned i=0;i<1000;++i) {
        assert(nvs_store_submit("test",&op,1,&ticket)==ESP_OK);
        drain(); assert(nvs_store_take_result(ticket,&result)==ESP_OK && result==ESP_OK);
    }
    fail_op=op_number+2; preferences_save(); drain(); preferences_poll();
    assert(atomic_load(&s_preferences_error)==777); /* failed write is not silently retried */
    assert_free(); assert_wiped();
    fail_op=0;
    runtime_tirtc_config_t identity={.device_id="test-device",.device_secret="test-only",.client_id="test-client"};
    assert(runtime_config_save_tirtc(&identity)==ESP_OK);
    assert(runtime_config_clear_tirtc()==ESP_OK);
    memset(identity.client_id,'x',sizeof(identity.client_id));
    assert(runtime_config_save_tirtc(&identity)==ESP_ERR_INVALID_ARG);
    assert(wifi_manager_save_credentials("test-only","12345678")==ESP_OK);
    assert(blob_size==99 && blob[0]==1 && strcmp((char *)blob+1,"test-only")==0);
    assert(wifi_manager_forget_credentials()==ESP_OK);
    fail_op=op_number+3; /* open/blob/commit: caller must not report queueing as persistence */
    assert(wifi_manager_save_credentials("test-only","12345678")==777);
    fail_op=0; assert_free(); assert_wiped();
    /* Reads must stay on the internal worker too. Completion data lives in
     * the slot until consumed, never in a queued caller pointer. */
    unsigned before_reads=committed;
    uint8_t output[512]; memset(output,0xcc,sizeof(output));
    size_t size=sizeof(output);
    assert(nvs_store_read_async("test","name",NVS_STORE_GET_STRING,16,&ticket)==ESP_OK);
    assert(nvs_store_take_read_result(ticket,&result,output,&size)==ESP_ERR_NOT_FINISHED);
    drain(); assert(output[0]==0xcc);
    size=2;
    assert(nvs_store_take_read_result(ticket,&result,output,&size)==ESP_ERR_INVALID_SIZE && size==9);
    assert(output[0]==0xcc && s_slots[ticket&7].state==SLOT_DONE);
    size=sizeof(output);
    assert(nvs_store_take_read_result(ticket,&result,output,&size)==ESP_OK && result==ESP_OK);
    assert(size==9 && strcmp((char *)output,"snapshot")==0 && output[9]==0xcc);
    assert_free(); assert_wiped();
    size=sizeof(output);
    assert(nvs_store_read("test","data",NVS_STORE_GET_BLOB,output,&size,5000)==ESP_OK);
    assert(size==96 && output[0]==0x5a && output[95]==0x5a);
    size=1; assert(nvs_store_read("test","number",NVS_STORE_GET_U8,output,&size,5000)==ESP_OK && output[0]==41);
    size=2; output[0]=0xcc;
    assert(nvs_store_read("test","name",NVS_STORE_GET_STRING,output,&size,5000)==ESP_ERR_NVS_INVALID_LENGTH);
    assert(size==9 && output[0]==0xcc); assert_free(); assert_wiped();
    read_error=ESP_ERR_NVS_NOT_FOUND; size=1;
    assert(nvs_store_read("test","absent",NVS_STORE_GET_U8,output,&size,5000)==ESP_ERR_NVS_NOT_FOUND);
    assert(output[0]==0xcc); read_error=0;
    assert(nvs_store_read_async("test","data",NVS_STORE_GET_BLOB,513,&ticket)==ESP_ERR_INVALID_SIZE);
    assert(nvs_store_read_async("test","data",NVS_STORE_U8,1,&ticket)==ESP_ERR_INVALID_ARG);
    assert(nvs_store_read_async("test","data",NVS_STORE_GET_BLOB,96,&ticket)==ESP_OK);
    drain(); assert(nvs_store_abandon(ticket)==ESP_OK); assert_free(); assert_wiped();
    assert(nvs_store_read_async("test","data",NVS_STORE_GET_BLOB,96,&ticket)==ESP_OK);
    assert(nvs_store_abandon(ticket)==ESP_OK); drain(); assert_free(); assert_wiped();
    blocked_write=true; size=96;
    assert(nvs_store_read("test","data",NVS_STORE_GET_BLOB,output,&size,10)==ESP_ERR_TIMEOUT);
    blocked_write=false; drain(); assert_free(); assert_wiped();
    assert(committed==before_reads); /* read-only jobs never commit */
    assert(runtime_config_load_tirtc(&identity)==ESP_OK && !strcmp(identity.device_id,"snapshot"));
    read_error=ESP_ERR_NVS_NOT_FOUND; read_failure_key="client_id";
    assert(runtime_config_load_tirtc(&identity)==ESP_OK && identity.client_id[0]==0);
    read_failure_key="secret";
    assert(runtime_config_load_tirtc(&identity)==ESP_ERR_NVS_NOT_FOUND && identity.device_id[0]==0);
    read_error=0; read_failure_key=NULL;
    assert(wifi_manager_save_credentials("test-only","12345678")==ESP_OK);
    wifi_manager_credentials_t loaded;
    assert(wifi_manager_load_credentials(&loaded)==ESP_OK && !strcmp(loaded.ssid,"test-only"));
    blob[0]=99;
    assert(wifi_manager_load_credentials(&loaded)==ESP_ERR_INVALID_RESPONSE && loaded.ssid[0]==0);
    read_error=ESP_ERR_NVS_NOT_FOUND; read_failure_key="credentials";
    assert(wifi_manager_load_credentials(&loaded)==ESP_OK && !strcmp(loaded.ssid,"snapshot"));
    read_failure_key="password";
    blob[0]=1; /* current blob wins, so absent legacy password is irrelevant */
    assert(wifi_manager_load_credentials(&loaded)==ESP_OK && !strcmp(loaded.ssid,"test-only"));
    read_error=0; read_failure_key=NULL;
    assert_free(); assert_wiped();
    printf("shared NVS: init failures, copies, queue saturation, stale/abandoned/timeout tickets, "
           "6 write failures, async reads/size/error/timeout/wipe, 10000 UI changes, 1000 jobs OK; job=%zu pool=%zu\n",sizeof(s_active),sizeof(s_jobs));
}
'''
    run_case('common NVS owner + product', harness)
    for file in ['components/runtime_config/src/runtime_config.c',
                 'components/wifi_manager/src/wifi_manager.c',
                 'components/starter_product/src/starter_product.c']:
        body = (ROOT / file).read_text(encoding='utf-8')
        assert not re.search(r'\bnvs_(?:get_|set_|erase_|open\b|close\b|commit\b)', body), file
    print('PASS: all active application reads/writes routed to nvs_store')


if __name__ == '__main__':
    main()
