#!/usr/bin/env python3
"""Guard PSRAM placement and exercise the actual captive-DNS task lifecycle."""
import re
from test_runtime_resources import COMMON, ROOT, function, run_case


def placement():
    product = (ROOT / "components/starter_product/src/starter_product.c").read_text(encoding="utf-8")
    wifi = (ROOT / "components/wifi_manager/src/wifi_manager.c").read_text(encoding="utf-8")
    assert "static EXT_RAM_BSS_ATTR StackType_t s_call_ring_stack[" in product
    assert "static DRAM_ATTR StaticTask_t s_call_ring_storage;" in product
    assert "#define PRODUCT_RING_STACK_BYTES 8192U" in product
    assert 'xTaskCreateStatic(call_ring_task, "call_ring"' in product
    assert 'xTaskCreate(call_ring_task' not in product
    portal = function(wifi, "start_http_server")
    assert "config.stack_size = 8192;" in portal
    assert "config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;" in portal
    assert "esp_restart()" in function(wifi, "signal_task")
    assert 'xTaskCreate(signal_task, "wifi_signal", 3072' in wifi
    print("PASS: ring/portal PSRAM placement, internal reboot owner preserved", flush=True)


def dns_lifecycle():
    source = (ROOT / "components/wifi_manager/src/captive_dns.c").read_text(encoding="utf-8")
    source = re.sub(r'^#include "[^"\n]+"\s*$', '', source, flags=re.M)
    harness = COMMON + r'''
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 263
#define pdMS_TO_TICKS(ms) (ms)
typedef unsigned TickType_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t;
typedef int esp_netif_t;
static unsigned ticks;
static int fail_socket, fail_bind, fail_option, fail_task, fail_ip;
static int socket_live, task_live, close_count, delete_count, blocked;
static int receive_step, sends, ip_changed;
static jmp_buf park;
static void dns_task(void *argument);
static esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out) {
    if (fail_ip) return ESP_FAIL;
    out->ip.addr=inet_addr(ip_changed ? "192.168.8.1" : "192.168.4.1"); return ESP_OK;
}
static int fake_socket(int family,int type,int protocol) {
    if (fail_socket) return -1;
    assert(!socket_live); socket_live=1; return 10;
}
static int fake_setsockopt(int fd,int level,int opt,const void *value,socklen_t len) {
    assert(fd==10 && socket_live);
    return fail_option && opt==SO_RCVTIMEO ? -1 : 0;
}
static int fake_bind(int fd,const struct sockaddr *addr,socklen_t len) {
    assert(fd==10 && socket_live); return fail_bind ? -1 : 0;
}
static int fake_close(int fd) {
    assert(fd==10 && socket_live); socket_live=0; ++close_count; return 0;
}
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned size,
    void *arg,int priority,TaskHandle_t *out,unsigned caps) {
    assert(fn==dns_task && size==8192 && priority==4);
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    assert(!task_live);
    if (fail_task) return 0;
    *out=malloc(size); assert(*out); ++task_live; return pdPASS;
}
static void vTaskDeleteWithCaps(TaskHandle_t task) {
    assert(task && task_live && !socket_live);
    free(task); --task_live; ++delete_count;
}
static void vTaskSuspend(TaskHandle_t task) { assert(task==NULL); longjmp(park,1); }
static TickType_t xTaskGetTickCount(void) { return ticks; }
static void run_worker(void) { if (!setjmp(park)) dns_task(NULL); }
static void vTaskDelay(unsigned ms) { ticks+=ms; if (!blocked) run_worker(); }
static ssize_t fake_recvfrom(int fd,void *buf,size_t size,int flags,
    struct sockaddr *addr,socklen_t *len);
static ssize_t fake_sendto(int fd,const void *buf,size_t size,int flags,
    const struct sockaddr *addr,socklen_t len);
#define socket fake_socket
#define setsockopt fake_setsockopt
#define bind fake_bind
#define close fake_close
#define recvfrom fake_recvfrom
#define sendto fake_sendto
'''
    harness += source
    harness += r'''
static const uint8_t query[]={0x12,0x34,0x01,0,0,1,0,0,0,0,0,0,1,'a',0,0,1,0,1};
static ssize_t fake_recvfrom(int fd,void *buf,size_t size,int flags,
    struct sockaddr *addr,socklen_t *len) {
    assert(fd==10 && socket_live && size>=sizeof(query));
    if (receive_step++==0) { memcpy(buf,query,sizeof(query)); return sizeof(query); }
    atomic_store(&s_dns_stop,true); errno=EAGAIN; return -1;
}
static ssize_t fake_sendto(int fd,const void *buf,size_t size,int flags,
    const struct sockaddr *addr,socklen_t len) {
    assert(fd==10 && socket_live && size==sizeof(query)+16);
    const uint8_t *reply=buf;
    assert(read_be16(reply)==0x1234 && read_be16(reply+6)==1);
    assert(!memcmp(reply+size-4,&s_portal_ip.addr,4)); ++sends; return size;
}
int main(void) {
    esp_netif_t netif=1;
    assert(captive_dns_start(NULL)==ESP_ERR_INVALID_ARG);
    assert(captive_dns_stop()==ESP_OK);
    int *faults[]={&fail_ip,&fail_socket,&fail_option,&fail_bind,&fail_task};
    for (unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);++i) {
        *faults[i]=1; assert(captive_dns_start(&netif)!=ESP_OK); *faults[i]=0;
        assert(!s_dns_task && !task_live && !socket_live);
        assert(captive_dns_stop()==ESP_OK);
    }
    for (int round=0;round<1000;++round) {
        ip_changed=round%2;
        assert(captive_dns_start(&netif)==ESP_OK);
        assert(captive_dns_start(&netif)==ESP_OK);
        TaskHandle_t owned=s_dns_task;
        if (round%3==0) {
            blocked=1; unsigned begin=ticks;
            assert(captive_dns_stop()==ESP_ERR_TIMEOUT && ticks-begin>=500);
            assert(s_dns_task==owned && task_live==1 && socket_live);
            assert(captive_dns_start(&netif)==ESP_ERR_INVALID_STATE); blocked=0;
        } else {
            receive_step=0; run_worker();
            assert(!atomic_load(&s_dns_running) && !socket_live && task_live==1);
        }
        assert(captive_dns_stop()==ESP_OK);
        assert(captive_dns_stop()==ESP_OK);
        assert(!s_dns_task && !task_live && !socket_live);
    }
    assert(delete_count==1000 && sends==666);
    assert(close_count==1003); /* option, bind and task-create failures */
    uint8_t reply[512], bad[sizeof(query)];
    memcpy(bad,query,sizeof(query)); bad[12]=0xc0;
    assert(make_reply(bad,sizeof(bad),reply,sizeof(reply))==0);
    assert(make_reply(query,sizeof(query),reply,16)==0);
    assert(make_reply(query,5,reply,sizeof(reply))==0);
    memcpy(bad,query,sizeof(query)); bad[16]=28; /* AAAA: no IPv4 answer */
    assert(make_reply(bad,sizeof(bad),reply,sizeof(reply))==sizeof(query));
    assert(read_be16(reply+6)==0);
    puts("1000 cycles: socket/task reclamation, stop timeout, creation failures, DNS replies OK");
    return 0;
}
'''
    run_case("captive DNS PSRAM owner lifecycle", harness)


if __name__ == "__main__":
    placement()
    dns_lifecycle()
