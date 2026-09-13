"""Host regression of real binding ownership/epoch code; no board credentials."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
platform = (root / 'components/platform_client/src/platform_client.c').read_text(encoding='utf-8')
runtime = (root / 'components/starter_runtime/src/starter_runtime.c').read_text(encoding='utf-8')
app = (root / 'main/app_main.c').read_text(encoding='utf-8')
ui = (root / 'components/starter_product/src/starter_product_s3_ui.inc').read_text(encoding='utf-8')

# NVS presence is not server acceptance: do not briefly expose the home page
# before a cold-boot 6006 response returns an offline-unbound device to binding.
stored = app[app.index('"stored device binding found;'):app.index('(void)snprintf(s_station_mac')]
assert 'STARTER_BINDING_CHECKING' in stored
assert 'STARTER_BINDING_READY' not in stored


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for i in range(opening, len(source)):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if depth == 0:
                return source[start:i+1]
    raise AssertionError(signature)


def run(code):
    with tempfile.TemporaryDirectory(prefix='binding-lifecycle-') as temp:
        path = Path(temp)
        (path / 'test.c').write_text(code, encoding='utf-8')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(path/'test.c'), '-o', str(path/'test')], check=True)
        subprocess.run([str(path/'test')], check=True)


common = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_INVALID_RESPONSE 3
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static const char *esp_err_to_name(int x) {(void)x;return "error";}
'''

code = common + r'''
#define PLATFORM_REQUEST_QUEUE_DEPTH 4
#define pdTRUE 1
typedef struct {
    int service; uint32_t epoch; char path[128]; bool post; char body[2048];
    unsigned timeout_ms; void (*callback)(const char*,void*); void *user_data;
} platform_request_t;
static atomic_bool s_ready, s_binding_retry, s_rebind_quiesced;
static atomic_uint s_rebind_request, s_epoch;
static uint32_t s_response_epoch;
static char s_mqtt_token[1024]="old-token";
static platform_request_t pool[4], *s_request_pool=pool;
static void *s_request_ready_queue=(void*)1, *s_request_free_queue=(void*)2;
static unsigned queued, returned, online, http_calls, callbacks;
static bool stop_failed, unbind_during_http, expected_body;
static int stop_mqtt(void) {return stop_failed ? 9 : 0;}
static void mbedtls_platform_zeroize(void *p,size_t n) {memset(p,0,n);}
static int xQueueReceive(void *q,uint8_t *slot,unsigned timeout) {
    assert(q==(void*)1 && timeout==0);
    if(!queued)return 0;
    *slot=(uint8_t)--queued; return pdTRUE;
}
static int xQueueSend(void *q,const uint8_t *slot,unsigned timeout) {
    if(q==(void*)1) {assert(*slot==UINT8_MAX && timeout==0);return 1;}
    assert(q==(void*)2 && *slot<4 && timeout==0); ++returned; return pdTRUE;
}
static void notify_platform_online(void) {++online;}
static const char *service_base(int s) {assert(s==1);return "http://local";}
bool platform_client_ready(void);
bool platform_client_request_rebind(bool);
uint32_t platform_client_epoch(void);
static int http_request(const char *url,const char *body,const char *token,
    void *headers,void *values,unsigned count,char *out,size_t cap,unsigned timeout,int *status) {
    (void)body;(void)token;(void)headers;(void)values;(void)count;
    assert(strcmp(url,"http://local/test")==0 && cap==64 && timeout==1000);
    ++http_calls; strcpy(out,"old-owner-response"); *status=200;
    if(unbind_during_http)assert(platform_client_request_rebind(true));
    return ESP_OK;
}
'''
for signature in ['bool platform_client_ready(', 'uint32_t platform_client_epoch(',
                  'uint32_t platform_client_response_epoch(', 'void platform_client_retry_binding(',
                  'bool platform_client_take_binding_retry(', 'bool platform_client_request_rebind(',
                  'bool platform_client_known_unbound(', 'bool platform_client_reconciling(',
                  'void platform_client_rebind_quiesced(',
                  'esp_err_t platform_client_prepare_rebind(', 'void platform_client_complete_rebind(',
                  'static void process_request(']:
    code += function(platform, signature) + '\n'
code += r'''
static void callback(const char *body,void *user) {
    assert(user==(void*)5 && (body!=NULL)==expected_body);
    assert(platform_client_response_epoch()==0); ++callbacks;
}
int main(void) {
    (void)esp_err_to_name;
    s_ready=true;
    platform_request_t request={.service=1,.epoch=0,.path="/test",.timeout_ms=1000,
        .callback=callback,.user_data=(void*)5};
    char response[64]; expected_body=true;
    process_request(&request,response,sizeof(response)); assert(http_calls==1 && callbacks==1);
    unbind_during_http=true;expected_body=false;
    process_request(&request,response,sizeof(response));
    assert(!platform_client_ready() && http_calls==2 && callbacks==2 && platform_client_epoch()==1);
    assert(!platform_client_request_rebind(true) && platform_client_epoch()==1);
    assert(platform_client_prepare_rebind()==ESP_ERR_INVALID_STATE);
    platform_client_rebind_quiesced();
    stop_failed=true; assert(platform_client_prepare_rebind()==9 && s_mqtt_token[0]);
    stop_failed=false;
    pool[0]=pool[1]=request;queued=2;
    assert(platform_client_prepare_rebind()==0 && !s_mqtt_token[0] && returned==2 && callbacks==4);
    platform_client_complete_rebind();assert(platform_client_reconciling() && online==0);
    s_ready=true;platform_client_complete_rebind();assert(platform_client_ready() && online==1);
    /* Producer that acquired a slot just before unbind may enqueue late. */
    process_request(&request,response,sizeof(response));assert(http_calls==2 && callbacks==5);
    assert(platform_client_request_rebind(false) && !platform_client_known_unbound());
    assert(!platform_client_request_rebind(true) && platform_client_known_unbound());
    assert(platform_client_epoch()==2);
    platform_client_retry_binding();platform_client_retry_binding();
    assert(platform_client_take_binding_retry() && !platform_client_take_binding_retry());
    return 0;
}
'''
run(code)

code = common + r'''
#define STARTER_BINDING_REQUIRED 1
#define STARTER_BINDING_FAILED 2
#define STARTER_BINDING_READY 3
#define DISCOVERY_URL "http://local/services"
typedef struct {char device_id[65],device_secret[257],client_id[65];} runtime_tirtc_config_t;
typedef struct {char device_id[65],device_secret[257];} platform_provision_result_t;
typedef struct {const char *mac_address,*existing_device_id,*existing_device_secret,*discovery_url;
    unsigned timeout_seconds; int (*prompt_callback)(const int16_t*,size_t,void*);
    void *prompt_user_data;void (*prompt_cancel_callback)(void*);} platform_provision_config_t;
static runtime_tirtc_config_t s_tirtc_config={"device","key","client"};
static unsigned saves;static int ui_state,provision_error;static bool changed,internal_stack;
static void starter_product_set_binding_state(int s) {ui_state=s;}
static int play_verification_prompt(const int16_t *p,size_t n,void *u) {(void)p;(void)n;(void)u;return 0;}
static void cancel_verification_prompt(void *p) {(void)p;}
static int runtime_config_save_tirtc(const runtime_tirtc_config_t *p) {
    assert(internal_stack && p==&s_tirtc_config);++saves;return 0;
}
static int platform_client_provision(const platform_provision_config_t *config,platform_provision_result_t *r) {
    assert(config->timeout_seconds==190);
    strcpy(r->device_id,changed?"unexpected-device":"device");strcpy(r->device_secret,"key");
    return provision_error;
}
'''+function(app,'static esp_err_t provision_and_save(')+r'''
int main(void) {
    (void)esp_err_to_name;
    assert(provision_and_save("mac",true)==0 && saves==0);
    changed=true;
    assert(provision_and_save("mac",true)==ESP_ERR_INVALID_RESPONSE && saves==0);
    assert(strcmp(s_tirtc_config.device_id,"device")==0 && ui_state==STARTER_BINDING_FAILED);
    changed=false;provision_error=7;
    assert(provision_and_save("mac",true)==7 && saves==0);
    provision_error=0;internal_stack=true;
    assert(provision_and_save("mac",false)==0 && saves==1 && ui_state==STARTER_BINDING_READY);
    return 0;
}
'''
run(code)

# Layering/static guards complement the executed C functions above.
assert 'runtime_config_clear_tirtc(' not in runtime
assert 'esp_restart(' not in runtime
assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in app
startup = function(app, 'static void starter_start_task(')
assert startup.index('runtime_config_load_tirtc') < startup.index('while (!wifi_manager_connected())')
assert 'if (!connected || binding == STARTER_BINDING_CHECKING)' not in ui
assert 'S3_ACTION_BINDING_RETRY' in ui
assert 'binding transition' in app
print('PASS: live unbind, duplicate signals, in-flight/late HTTP isolation, failed MQTT teardown,')
print('      identity-preserving PSRAM rebind, internal-only first save, retry coalescing, early setup navigation')
