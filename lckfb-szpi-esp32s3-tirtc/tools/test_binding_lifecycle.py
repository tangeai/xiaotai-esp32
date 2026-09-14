"""Host regression of real binding ownership/epoch code; no board credentials."""
from pathlib import Path
import subprocess
import tempfile
import re

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
#define PLATFORM_SERVICE_AI 1
#define PLATFORM_SERVICE_DEVICE 0
#define pdTRUE 1
typedef struct {
    int service; uint32_t epoch; char path[128]; bool post, metadata_only; char body[2048];
    unsigned timeout_ms; void (*callback)(const char*,void*); void *user_data;
    uint32_t queued_at_ms;
} platform_request_t;
typedef struct {
    uint32_t started_ms, prep_ms, conn_ms, head_ms, first_ms, done_ms;
    unsigned connections; bool reused, retained, redirected; int nodelay;
} http_timing_t;
static void http_log_result(const char *api,bool post,uint32_t req,uint32_t q,
        uint32_t total,int err,int status,const http_timing_t *t) {
    (void)api;(void)post;(void)req;(void)q;(void)total;(void)err;(void)status;(void)t;
}
typedef struct {void *client; char base[256]; uint32_t epoch;} http_session_t;
static http_session_t *observed_session;
static bool allow_retained;
static void http_session_close(http_session_t *s) {memset(s,0,sizeof(*s));}
static int64_t esp_timer_get_time(void) {return 1000000;}
static atomic_bool s_ready, s_binding_retry, s_rebind_quiesced;
static atomic_uint s_rebind_request, s_epoch;
static uint32_t s_response_epoch;
static char s_mqtt_token[1024]="old-token";
static platform_request_t pool[4], *s_request_pool=pool;
static void *s_request_ready_queue=(void*)1, *s_request_free_queue=(void*)2;
static unsigned queued, returned, online, http_calls, callbacks;
static unsigned uxQueueMessagesWaiting(void *q) {assert(q==(void*)1);return queued;}
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
static const char *service_base(int s) {assert(s==0 || s==1);return "http://local";}
bool platform_client_ready(void);
bool platform_client_request_rebind(bool);
uint32_t platform_client_epoch(void);
static int http_request_perform(const char *url,const char *body,const char *token,
    void *headers,void *values,unsigned count,char *out,size_t cap,unsigned timeout,int *status,
    http_session_t *session,const char *base,http_timing_t *timing) {
    (void)body;(void)token;(void)headers;(void)values;(void)count;
    session->client=(void*)3;timing->retained=true;assert(strcmp(base,"http://local")==0);
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
                  'static const char *http_api_name(', 'static void process_request(']:
    code += function(platform, signature) + '\n'
code += r'''
static void callback(const char *body,void *user) {
    assert(user==(void*)5 && (body!=NULL)==expected_body);
    assert((observed_session->client!=NULL)==allow_retained);
    assert(platform_client_response_epoch()==0); ++callbacks;
}
int main(void) {
    (void)esp_err_to_name;
    s_ready=true;
    platform_request_t request={.service=1,.epoch=0,.path="/test",.timeout_ms=1000,
        .callback=callback,.user_data=(void*)5};
    http_session_t session={0};
    observed_session=&session;
    char response[64]; expected_body=true;
    process_request(&request,response,sizeof(response),&session); assert(http_calls==1 && callbacks==1);
    unbind_during_http=true;expected_body=false;
    process_request(&request,response,sizeof(response),&session);
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
    process_request(&request,response,sizeof(response),&session);assert(http_calls==2 && callbacks==5);
    assert(platform_client_request_rebind(false) && !platform_client_known_unbound());
    assert(!platform_client_request_rebind(true) && platform_client_known_unbound());
    assert(platform_client_epoch()==2);
    platform_client_retry_binding();platform_client_retry_binding();
    assert(platform_client_take_binding_retry() && !platform_client_take_binding_retry());
    /* Metadata reuses only within a queued burst. Critical callbacks and an
     * empty queue must observe that the HTTP handle was already released. */
    atomic_store(&s_epoch,0);atomic_store(&s_rebind_request,0);
    unbind_during_http=false;expected_body=true;request.service=PLATFORM_SERVICE_DEVICE;
    queued=1;allow_retained=true;request.metadata_only=true;
    process_request(&request,response,sizeof(response),&session);assert(session.client);
    request.service=PLATFORM_SERVICE_AI;allow_retained=false;request.metadata_only=false;
    process_request(&request,response,sizeof(response),&session);assert(!session.client);
    request.service=PLATFORM_SERVICE_DEVICE;queued=0;request.metadata_only=true;
    process_request(&request,response,sizeof(response),&session);assert(!session.client);
    return 0;
}
'''
run(code)

# Exercise the actual HTTP collector and handle lifetime with IDF API doubles.
# No server credentials, real HTTP requests, or board access are involved.
trace_header = (root / 'components/platform_client/src/platform_http_trace.h').read_text(encoding='utf-8')
trace_type = re.search(r'typedef struct \{[^}]*\} platform_http_trace_t;', trace_header).group(0)
http_code = common + trace_type + r'''
#include <stdlib.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <stdarg.h>
#undef ESP_LOGI
static char captured_log[2048];
static void record_log(const char *format,...) __attribute__((format(printf,1,2)));
static void record_log(const char *format,...) {
    va_list args;va_start(args,format);vsnprintf(captured_log,sizeof(captured_log),format,args);va_end(args);
}
#define ESP_LOGI(tag,...) record_log(__VA_ARGS__)
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_NOT_FOUND 5
#define PLATFORM_DEFAULT_HTTP_TIMEOUT_MS 15000U
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
enum {HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT, HTTP_EVENT_ON_HEADER,
      HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED};
typedef struct {int event_id; void *user_data; void *client; const char *header_key;
                void *data; int data_len;} esp_http_client_event_t;
typedef struct {
    const char *url; int (*event_handler)(esp_http_client_event_t*); void *user_data;
    int timeout_ms; void (*crt_bundle_attach)(void); bool disable_auto_redirect;
} esp_http_client_config_t;
typedef struct {
    int (*handler)(esp_http_client_event_t*); void *user_data;
    char url[512], authorization[1100], content_type[64];
    const char *post; int post_len, method, timeout_ms, status;
    bool connected;
} mock_client_t;
typedef mock_client_t *esp_http_client_handle_t;
static unsigned creates, performs, destroys, opened, active;
static bool dirty_socket, fail_init, redirect_response, close_response;
static int perform_error, header_error, response_status=200;
static const char *response_body="{}";
static int64_t clock_us;
static unsigned trace_begins, trace_ends, nodelay_calls;
static bool fail_nodelay;
static void platform_http_trace_begin(platform_http_trace_t *t) {
    *t=(platform_http_trace_t){.enabled=true,.write_ms=UINT32_MAX,.first_rx_ms=UINT32_MAX};
    ++trace_begins;
}
static void platform_http_trace_end(platform_http_trace_t *t) {assert(t->enabled);++trace_ends;}
static int mock_setsockopt(int fd,int level,int option,const void *value,socklen_t size) {
    assert(fd==3 && level==IPPROTO_TCP && option==TCP_NODELAY && size==sizeof(int));
    assert(*(const int*)value==1);++nodelay_calls;errno=EINVAL;return fail_nodelay?-1:0;
}
#define setsockopt mock_setsockopt
static int64_t esp_timer_get_time(void) {return clock_us;}
static void esp_crt_bundle_attach(void) {}
static int mock_select(int nfds,fd_set *r,fd_set *w,fd_set *e,struct timeval *t) {
    assert(nfds==4 && r && !w && e && !t->tv_sec && !t->tv_usec);
    return dirty_socket ? 1 : 0;
}
#define select mock_select
static void emit(mock_client_t *c,int event,const char *key,void *data,int n) {
    esp_http_client_event_t e={.event_id=event,.user_data=c->user_data,.client=c,
        .header_key=key,.data=data,.data_len=n};
    c->handler(&e);
}
static int esp_http_client_set_url(mock_client_t *c,const char *url) {
    snprintf(c->url,sizeof(c->url),"%s",url);return 0;
}
static mock_client_t *esp_http_client_init(const esp_http_client_config_t *config) {
    if(fail_init)return NULL;
    mock_client_t *c=calloc(1,sizeof(*c));assert(c);++creates;++active;
    c->handler=config->event_handler;c->user_data=config->user_data;
    c->timeout_ms=config->timeout_ms;esp_http_client_set_url(c,config->url);return c;
}
static int esp_http_client_set_user_data(mock_client_t *c,void *p) {c->user_data=p;return 0;}
static int esp_http_client_set_timeout_ms(mock_client_t *c,int n) {c->timeout_ms=n;return 0;}
static int esp_http_client_reset_redirect_counter(mock_client_t *c) {(void)c;return 0;}
static int esp_http_client_set_method(mock_client_t *c,int method) {c->method=method;return 0;}
static int esp_http_client_set_header(mock_client_t *c,const char *key,const char *value) {
    if(header_error)return header_error;
    char *out=strcmp(key,"Authorization")==0?c->authorization:c->content_type;
    size_t cap=out==c->authorization?sizeof(c->authorization):sizeof(c->content_type);
    if(!value) {bool had=out[0]!=0;out[0]=0;return had?0:ESP_ERR_NOT_FOUND;}
    snprintf(out,cap,"%s",value);return 0;
}
static int esp_http_client_set_post_field(mock_client_t *c,const char *data,int n) {
    c->post=data;c->post_len=n;
    return data?0:esp_http_client_set_header(c,"Content-Type",NULL);
}
static int esp_http_client_get_status_code(mock_client_t *c) {return c->status;}
static int esp_http_client_get_socket(mock_client_t *c) {return c->connected?3:-1;}
static int esp_http_client_cleanup(mock_client_t *c) {
    assert(c && !c->user_data && !c->post);++destroys;--active;
    emit(c,HTTP_EVENT_DISCONNECTED,NULL,NULL,0);free(c);return 0;
}
static int esp_http_client_perform(mock_client_t *c) {
    ++performs;
    if(c->method==HTTP_METHOD_GET)assert(!c->post && !c->post_len && !c->content_type[0]);
    else assert(c->post && c->post_len==(int)strlen(c->post) && !strcmp(c->content_type,"application/json"));
    if(!c->connected) {clock_us+=10000;c->connected=true;++opened;emit(c,HTTP_EVENT_ON_CONNECTED,NULL,NULL,0);}
    clock_us+=2000;emit(c,HTTP_EVENT_HEADERS_SENT,NULL,NULL,0);
    if(perform_error)return perform_error;
    c->status=response_status;clock_us+=20000;
    emit(c,HTTP_EVENT_ON_HEADER,redirect_response?"Location":"Content-Length",NULL,0);
    emit(c,HTTP_EVENT_ON_DATA,NULL,(void*)response_body,strlen(response_body));
    clock_us+=1000;emit(c,HTTP_EVENT_ON_FINISH,NULL,NULL,0);
    if(close_response)c->connected=false;
    return 0;
}
'''
for name in ('http_timing_t', 'http_output_t', 'http_session_t'):
    http_code += re.search(r'typedef struct \{[^}]*\} ' + name + r';', platform).group(0) + '\n'
for signature in ('static esp_err_t http_event(', 'static void http_session_close(',
                  'static bool http_session_socket_idle(', 'static esp_err_t http_request_perform(',
                  'static const char *http_api_name(', 'static void http_log_result(',
                  'static esp_err_t http_request('):
    http_code += function(platform, signature) + '\n'
http_code += r'''
int main(void) {
    (void)esp_err_to_name;
    http_session_t session={0};http_timing_t t;int status=-1;char out[64];
    #define REQUEST(base,path,body,bearer) http_request_perform(base path,body,bearer,NULL,NULL,0, \
        out,sizeof(out),800,&status,&session,base,&t)
    assert(REQUEST("http://local","/one",NULL,"one")==0);
    assert(active==1 && creates==1 && !t.reused && t.retained && t.connections==1);
    assert(t.nodelay==1 && nodelay_calls==1);
    assert(t.conn_ms==10 && t.head_ms==12 && t.first_ms==32 && t.done_ms==33);
    assert(!session.client->user_data && !session.client->post && !strcmp(out,"{}"));
    assert(REQUEST("http://local","/two?x=1","{\"x\":1}","two")==0);
    assert(creates==1 && t.reused && !t.connections && t.conn_ms==UINT32_MAX);
    assert(!strcmp(session.client->authorization,"Bearer two"));
    assert(REQUEST("http://local","/three",NULL,NULL)==0);
    assert(!session.client->authorization[0] && !strcmp(session.client->url,"http://local/three"));
    /* FIN/readable data is discarded before submission, not retried after it. */
    dirty_socket=true;
    assert(REQUEST("http://local","/four",NULL,"one")==0 && creates==2 && !t.reused);
    dirty_socket=false;
    assert(REQUEST("http://other","/one",NULL,"one")==0 && creates==3 && active==1);
    close_response=true;
    assert(REQUEST("http://other","/close",NULL,"one")==0 && !active && !t.retained);
    close_response=false;redirect_response=true;
    assert(REQUEST("http://local","/redirect",NULL,"one")==0 && !active && t.redirected);
    redirect_response=false;
    assert(REQUEST("https://local","/one",NULL,"one")==0 && !active);
    response_status=401;
    assert(REQUEST("http://local","/denied",NULL,"one")==0 && status==401 && !active);
    response_status=200;perform_error=17;unsigned before=performs;
    assert(REQUEST("http://local","/create","{}","one")==17 && !active && performs==before+1);
    perform_error=0;response_body="0123456789";
    assert(http_request_perform("http://local/large",NULL,"one",NULL,NULL,0,out,4,800,&status,
        &session,"http://local",&t)==ESP_ERR_INVALID_SIZE && !active && out[0]==0);
    response_body="{}";
    fail_init=true;
    assert(REQUEST("http://local","/oom",NULL,"one")==ESP_ERR_NO_MEM && status==0 && !active);
    fail_init=false;header_error=19;before=performs;
    assert(REQUEST("http://local","/header","{}","one")==19 && !active && performs==before);
    header_error=0;
    fail_nodelay=true;
    assert(REQUEST("http://local","/nodelay",NULL,"one")==0 && t.nodelay==0);
    http_session_close(&session);
    fail_nodelay=false;
    /* Discovery/binding one-shot contract and absent header deletion semantics. */
    assert(http_request("http://local/services",NULL,NULL,NULL,NULL,0,out,sizeof(out),0,&status)==0);
    assert(!active);
    assert(REQUEST("http://local","/final",NULL,"one")==0);
    http_session_close(&session);http_session_close(&session);
    assert(!active && creates==destroys && opened<=performs);
    assert(trace_begins==performs && trace_ends==performs && nodelay_calls==opened);
    t=(http_timing_t){.io={.enabled=true,.dns_ms=1054,.dns_calls=1,.connect_ms=1072,
        .connect_calls=1,.write_ms=1078,.first_rx_ms=1383}};
    http_log_result("/v1/ai/token",true,100,0,1400,0,200,&t);
    assert(strstr(captured_log,"api=/v1/ai/token method=POST"));
    assert(strstr(captured_log,"dns_ms=1054") && strstr(captured_log,"net_ms=18"));
    assert(strstr(captured_log,"wait_ms=305"));
    t.redirected=true;
    http_log_result("/v1/ai/token",true,100,0,1400,0,200,&t);
    assert(strstr(captured_log,"wait_ms=-1"));
    t.redirected=false;t.io.write_ms=1500; /* Auth replay after a first response. */
    http_log_result("/v1/ai/token",true,100,0,1600,0,200,&t);
    assert(strstr(captured_log,"wait_ms=-1"));
    return 0;
}
'''
run(http_code)

assert 'user_data, false)' in function(platform, 'esp_err_t platform_client_request_timeout(')
assert 'callback, user_data, true)' in function(platform, 'esp_err_t platform_client_request_metadata(')
assert runtime.count('platform_client_request_metadata(') == 2
assert 'platform_client_request_metadata(' in function(runtime, 'static void refresh_contacts(')
assert 'platform_client_request_metadata(' in function(runtime, 'static void request_voip_profile(')
assert 'platform_client_request_metadata(' not in function(runtime, 'static void begin_ai_session(')

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
static unsigned saves;static int ui_state,provision_error;static bool changed;
static void starter_product_set_binding_state(int s) {ui_state=s;}
static int play_verification_prompt(const int16_t *p,size_t n,void *u) {(void)p;(void)n;(void)u;return 0;}
static void cancel_verification_prompt(void *p) {(void)p;}
static int runtime_config_save_tirtc(const runtime_tirtc_config_t *p) {
    assert(p==&s_tirtc_config);++saves;return 0;
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
    provision_error=0;
    assert(provision_and_save("mac",false)==0 && saves==1 && ui_state==STARTER_BINDING_READY);
    return 0;
}
'''
run(code)

# Layering/static guards complement the executed C functions above.
assert 'runtime_config_clear_tirtc(' not in runtime
assert 'esp_restart(' not in runtime
assert 'static EXT_RAM_BSS_ATTR StackType_t s_start_stack' in app
startup = function(app, 'static void starter_start_task(')
assert startup.index('runtime_config_load_tirtc') < startup.index('while (!wifi_manager_connected())')
assert 'if (!connected || binding == STARTER_BINDING_CHECKING)' not in ui
assert 'S3_ACTION_BINDING_RETRY' in ui
assert 'binding transition' in app
print('PASS: live unbind, duplicate signals, in-flight/late HTTP isolation, failed MQTT teardown,')
print('      identity-preserving PSRAM rebind, first-save result, retry coalescing, early setup navigation')
