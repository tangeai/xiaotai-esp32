#!/usr/bin/env python3
"""Execute the vendored RPC routing with host semaphores, not a routing model."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--legacy", type=Path, help="prove FIFO mismatch in an unmodified core")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
path = root / "components/espressif__esp_hosted/host/drivers/rpc/core/rpc_core.c"
source = (args.legacy or path).read_text(encoding="utf-8")


def function(name):
    match = re.search(r"^(?:static )?(?:ctrl_cmd_t \*|int|void)\s*" + name
                      + r"\([^;]*?\)\n\{", source, re.M)
    assert match, name
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end]


types = re.search(r"typedef struct \{\n\s*uint32_t uid;.*?\} sync_rsp_t;", source, re.S)[0]
body = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <time.h>
#define SUCCESS 0
#define FAILURE -1
#define CALLBACK_SET_SUCCESS 0
#define CALLBACK_NOT_REGISTERED -3
#define MSG_ID_OUT_OF_ORDER -2
#define RET_FAIL_TIMEOUT -4
#define HOSTED_BLOCK_MAX -1
#define DEFAULT_RPC_RSP_TIMEOUT 5
#define RPC_TYPE__Req 1
#define RPC_ID__Req_Base 0x100
#define RPC_ID__Resp_Base 0x200
#define RPC_ID__Resp_Max 0x300
#define MAX_SYNC_RPC_TRANSACTIONS 5
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGV(...) ((void)0)
typedef struct ctrl_cmd {
    uint32_t uid, msg_id, msg_type;
    int rsp_timeout_sec, resp_event_status;
    void *rx_sem;
    void (*rpc_rsp_cb)(void *);
    void *app_free_buff_hdl;
    void (*app_free_buff_func)(void *);
} ctrl_cmd_t;
typedef struct { void *buf; int buf_len; } esp_queue_elem_t;
static atomic_int live_sem, live_req, live_resp;
static bool sem_oom, queue_fail;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static void *sync_rsp_mutex = &registry_mutex;
static int lock(void *m, int timeout) {(void)timeout; return pthread_mutex_lock(m);}
static int unlock(void *m) {return pthread_mutex_unlock(m);}
static void *create_sem(int count) {
    if (sem_oom) return NULL;
    sem_t *s=malloc(sizeof(*s)); assert(s); assert(!sem_init(s,0,count)); ++live_sem; return s;
}
static int destroy_sem(void *s) {assert(s); assert(!sem_destroy(s)); free(s); --live_sem; return 0;}
static int take(void *s, int sec) {
    assert(s);
    if (!sec) return sem_trywait(s) ? RET_FAIL_TIMEOUT : 0;
    struct timespec end; clock_gettime(CLOCK_REALTIME,&end);
    end.tv_nsec+=10000000;
    if (end.tv_nsec>=1000000000) {++end.tv_sec; end.tv_nsec-=1000000000;}
    return sem_timedwait(s,&end) ? RET_FAIL_TIMEOUT : 0;
}
static int post(void *s) {assert(s); return sem_post(s);}
static int queue_item(void *q, void *p, int t) {(void)q;(void)p;(void)t; return queue_fail ? -1 : 0;}
static esp_queue_elem_t fifo[8];
static int fifo_count;
static int dequeue(void *q, void *p, int t) {
    (void)q; (void)t; assert(fifo_count>0); *(esp_queue_elem_t*)p=fifo[0];
    for(int i=1;i<fifo_count;++i) fifo[i-1]=fifo[i]; --fifo_count; return 0;
}
static void free_cmd(void *p) {
    ctrl_cmd_t *c=p; if (!c) return;
    if(c->msg_type==RPC_TYPE__Req) --live_req; else --live_resp;
    free(c);
}
static const struct {
    int (*_h_lock_mutex)(void *,int), (*_h_unlock_mutex)(void *);
    void *(*_h_create_semaphore)(int);
    int (*_h_destroy_semaphore)(void *), (*_h_get_semaphore)(void *,int), (*_h_post_semaphore)(void *);
    int (*_h_queue_item)(void *,void *,int), (*_h_dequeue_item)(void *,void *,int);
    void (*_h_free)(void *);
} funcs={lock,unlock,create_sem,destroy_sem,take,post,queue_item,dequeue,free_cmd};
static struct { const __typeof__(funcs) *funcs; } g_h={&funcs};
#define HOSTED_FREE(p) do {if(p){g_h.funcs->_h_free(p);p=NULL;}}while(0)
#define H_FREE_PTR_WITH_FUNC(f,p) do {if(f && p){f(p);p=NULL;}}while(0)
#define CLEANUP_APP_MSG(p) do {if(p){H_FREE_PTR_WITH_FUNC(p->app_free_buff_func,p->app_free_buff_hdl);HOSTED_FREE(p);}}while(0)
static ctrl_cmd_t *new_req(uint32_t id) {
    ctrl_cmd_t *r=calloc(1,sizeof(*r)); assert(r); ++live_req;
    r->msg_type=RPC_TYPE__Req; r->msg_id=id; return r;
}
static ctrl_cmd_t *new_resp(ctrl_cmd_t *r) {
    ctrl_cmd_t *p=calloc(1,sizeof(*p)); assert(p); ++live_resp;
    p->uid=r->uid; p->msg_id=r->msg_id+0x100; p->msg_type=2; return p;
}
'''
body += types + "\nstatic sync_rsp_t sync_rsp_table[MAX_SYNC_RPC_TRANSACTIONS];\n"
body += function("wait_for_sync_response") + function("set_sync_resp_sem")
body += function("post_sync_resp_sem")
if args.legacy:
    body += "\nstatic void *rpc_rx_q;\n" + function("get_response")
    body += r'''
int main(void) {
    ctrl_cmd_t *a=new_req(0x104), *b=new_req(0x11a); a->uid=1; b->uid=2;
    assert(!set_sync_resp_sem(a)); assert(!set_sync_resp_sem(b));
    ctrl_cmd_t *ra=new_resp(a), *rb=new_resp(b);
    fifo[fifo_count++]=(esp_queue_elem_t){ra,sizeof(*ra)};
    fifo[fifo_count++]=(esp_queue_elem_t){rb,sizeof(*rb)};
    assert(!post_sync_resp_sem(ra)); assert(!post_sync_resp_sem(rb));
    int size=0; ctrl_cmd_t *got_b=get_response(&size,b);
    assert(got_b->uid!=b->uid && got_b->msg_id==a->msg_id+0x100);
    ctrl_cmd_t *got_a=get_response(&size,a); assert(got_a==rb);
    free_cmd(ra);free_cmd(rb);free_cmd(a);free_cmd(b);
    assert(!live_req && !live_resp && !live_sem);
    puts("REPRODUCED: AP-info waiter consumes SetMode success with another UID");
}
'''
else:
    assert "rpc_rx_q" not in source
    body += function("finish_sync_request") + function("get_response")
    body += function("rpc_wait_and_parse_sync_resp")
    body += "\nstatic uint32_t uid; static void *rpc_tx_q; static void rpc_tx_ind(void) {}\n"
    body += function("rpc_send_req")
    body += r'''
static void ready(ctrl_cmd_t *r) {assert(!rpc_send_req(r)); assert(r->uid);}
static void consume(ctrl_cmd_t *r, int status) {
    uint32_t u=r->uid, id=r->msg_id+0x100;
    ctrl_cmd_t *p=rpc_wait_and_parse_sync_resp(r);
    assert(p && p->uid==u && p->msg_id==id && p->resp_event_status==status);
    CLEANUP_APP_MSG(p);
}
static void empty(void) {
    assert(!live_sem && !live_req && !live_resp);
    for(int i=0;i<5;++i) assert(!sync_rsp_table[i].request);
}
static void *concurrent(void *arg) {
    for(int i=0;i<1000;++i) {
        ctrl_cmd_t *r=new_req(0x104+(uintptr_t)arg); ready(r);
        ctrl_cmd_t *p=new_resp(r); assert(!post_sync_resp_sem(p));
        finish_sync_request(r,true); consume(r,0);
    }
    return NULL;
}
int main(void) {
    for(int round=0;round<500;++round) {
        ctrl_cmd_t *r[5];
        for(int i=0;i<5;++i) {r[i]=new_req(0x104+i);ready(r[i]);}
        for(int i=0;i<5;++i) {ctrl_cmd_t *p=new_resp(r[i]);assert(!post_sync_resp_sem(p));finish_sync_request(r[i],true);}
        for(int i=4;i>=0;--i) consume(r[i],0);
        empty();
    }
    ctrl_cmd_t *r=new_req(0x104);ready(r);
    ctrl_cmd_t *bad=new_resp(r); ++bad->msg_id;
    assert(post_sync_resp_sem(bad)==MSG_ID_OUT_OF_ORDER);CLEANUP_APP_MSG(bad);
    ctrl_cmd_t *p=new_resp(r);p->resp_event_status=-99;assert(!post_sync_resp_sem(p));
    ctrl_cmd_t *dup=new_resp(r);assert(post_sync_resp_sem(dup)!=SUCCESS);CLEANUP_APP_MSG(dup);
    finish_sync_request(r,true);consume(r,-99);empty();
    r=new_req(0x104);ready(r);p=new_resp(r);
    assert(!rpc_wait_and_parse_sync_resp(r)); /* TX still owns request after timeout */
    assert(live_req==1 && !live_sem && r->msg_id==0x104);
    assert(post_sync_resp_sem(p)!=SUCCESS);CLEANUP_APP_MSG(p);
    finish_sync_request(r,true);empty();
    r=new_req(0x104);ready(r);p=new_resp(r);finish_sync_request(r,true);
    assert(!rpc_wait_and_parse_sync_resp(r));
    r=new_req(0x104);ready(r);assert(post_sync_resp_sem(p)!=SUCCESS);CLEANUP_APP_MSG(p);
    p=new_resp(r);assert(!post_sync_resp_sem(p));finish_sync_request(r,true);consume(r,0);empty();
    queue_fail=true;assert(rpc_send_req(new_req(0x104))==FAILURE);queue_fail=false;empty();
    sem_oom=true;assert(rpc_send_req(new_req(0x104))==FAILURE);sem_oom=false;empty();
    ctrl_cmd_t *all[5];for(int i=0;i<5;++i){all[i]=new_req(0x104+i);ready(all[i]);}
    assert(rpc_send_req(new_req(0x110))==FAILURE);
    for(int i=0;i<5;++i){p=new_resp(all[i]);assert(!post_sync_resp_sem(p));finish_sync_request(all[i],true);consume(all[i],0);}empty();
    uid=UINT32_MAX;r=new_req(0x104);ready(r);assert(r->uid==1);
    p=new_resp(r);assert(!post_sync_resp_sem(p));finish_sync_request(r,true);consume(r,0);empty();
    pthread_t threads[5];for(uintptr_t i=0;i<5;++i)assert(!pthread_create(&threads[i],NULL,concurrent,(void*)i));
    for(int i=0;i<5;++i)assert(!pthread_join(threads[i],NULL));empty();
    puts("PASS: 7500 routed transactions; concurrent/reverse wait/timeout/late/duplicate/mismatch/OOM/queue-full/UID-wrap; no leaks");
}
'''
    slave = (root / "components/espressif__esp_hosted/host/drivers/rpc/slaveif/rpc_slave_if.c").read_text()
    assert '"Failed to send control req 0x%x\\n", req->msg_id' not in slave
    tx = function("process_rpc_tx_msg")
    assert tx.count("app_resp->uid = app_req->uid;") == 2
    assert "finish_sync_request(app_req, true)" in function("rpc_tx_thread")
with tempfile.TemporaryDirectory(prefix="p4-rpc-") as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(body, encoding="utf-8")
    subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra",
                    "-Wno-unused-variable", "-Wno-misleading-indentation",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                    str(temp / "test.c"), "-o", str(temp / "test")], check=True)
    subprocess.run([str(temp / "test")], check=True)
