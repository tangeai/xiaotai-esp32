#!/usr/bin/env python3
"""Run the real NVS executor with pthread RTOS doubles and ASan/UBSan.

Tests ownership, bounded queues, timeout races and adapters, not physical Flash.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHIM = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_SIZE -3
#define ESP_ERR_INVALID_STATE -4
#define ESP_ERR_TIMEOUT -5
#define ESP_ERR_NO_MEM -6
#define ESP_ERR_NVS_NOT_FOUND -7
#define EXT_RAM_BSS_ATTR
#define DRAM_ATTR
#define ESP_LOGE(...) ((void)0)
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
typedef uint32_t StackType_t, EventBits_t;
typedef struct {pthread_mutex_t mutex; pthread_cond_t cond; unsigned bits;} StaticEventGroup_t;
typedef struct {pthread_mutex_t mutex;} StaticSemaphore_t;
typedef struct {pthread_mutex_t mutex; pthread_cond_t cond; unsigned head, count, limit; uint8_t *bytes;} StaticQueue_t;
typedef struct {pthread_t thread; void (*fn)(void *); void *arg;} StaticTask_t;
typedef StaticEventGroup_t *EventGroupHandle_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef StaticQueue_t *QueueHandle_t;
typedef StaticTask_t *TaskHandle_t;
static _Thread_local TaskHandle_t current_task;
static _Thread_local bool isr;
static int fail_create, creates;
static struct timespec deadline(unsigned ms) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms/1000; t.tv_nsec += (ms%1000)*1000000L;
    t.tv_sec += t.tv_nsec/1000000000L; t.tv_nsec %= 1000000000L; return t;
}
static int wait_cond(pthread_cond_t *c, pthread_mutex_t *m, unsigned ms, const struct timespec *t) {
    return ms==portMAX_DELAY ? pthread_cond_wait(c,m) : pthread_cond_timedwait(c,m,t);
}
static bool create_ok(void) { return ++creates != fail_create; }
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) {
    if (!create_ok()) return NULL; pthread_mutex_init(&s->mutex,NULL); return s;
}
static int xSemaphoreTake(SemaphoreHandle_t s,unsigned ms) {
    if(ms==portMAX_DELAY)return pthread_mutex_lock(&s->mutex)==0;
    struct timespec t=deadline(ms);return pthread_mutex_timedlock(&s->mutex,&t)==0;
}
static void xSemaphoreGive(SemaphoreHandle_t s) {pthread_mutex_unlock(&s->mutex);}
static void vSemaphoreDelete(SemaphoreHandle_t s) {pthread_mutex_destroy(&s->mutex);}
static EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *s) {
    if(!create_ok())return NULL;pthread_mutex_init(&s->mutex,NULL);pthread_cond_init(&s->cond,NULL);s->bits=0;return s;
}
static void vEventGroupDelete(EventGroupHandle_t s) {pthread_mutex_destroy(&s->mutex);pthread_cond_destroy(&s->cond);}
static void xEventGroupClearBits(EventGroupHandle_t s,unsigned b) {
    pthread_mutex_lock(&s->mutex);s->bits&=~b;pthread_mutex_unlock(&s->mutex);
}
static void xEventGroupSetBits(EventGroupHandle_t s,unsigned b) {
    pthread_mutex_lock(&s->mutex);s->bits|=b;pthread_cond_broadcast(&s->cond);pthread_mutex_unlock(&s->mutex);
}
static unsigned xEventGroupWaitBits(EventGroupHandle_t s,unsigned b,int clear,int all,unsigned ms) {
    assert(all);struct timespec t=deadline(ms);pthread_mutex_lock(&s->mutex);
    while((s->bits&b)!=b)if(wait_cond(&s->cond,&s->mutex,ms,&t)==ETIMEDOUT)break;
    unsigned result=s->bits;if(clear && (result&b)==b)s->bits&=~b;
    pthread_mutex_unlock(&s->mutex);return result;
}
static QueueHandle_t xQueueCreateStatic(unsigned n,unsigned width,uint8_t *bytes,StaticQueue_t *q) {
    assert(width==1);if(!create_ok())return NULL;
    pthread_mutex_init(&q->mutex,NULL);pthread_cond_init(&q->cond,NULL);
    q->bytes=bytes;q->limit=n;q->head=q->count=0;return q;
}
static void vQueueDelete(QueueHandle_t q) {pthread_mutex_destroy(&q->mutex);pthread_cond_destroy(&q->cond);}
static int xQueueSend(QueueHandle_t q,const void *v,unsigned ms) {
    assert(ms==0);pthread_mutex_lock(&q->mutex);int ok=q->count<q->limit;
    if(ok){q->bytes[(q->head+q->count++)%q->limit]=*(const uint8_t*)v;pthread_cond_signal(&q->cond);}
    pthread_mutex_unlock(&q->mutex);return ok;
}
static int xQueueReceive(QueueHandle_t q,void *v,unsigned ms) {
    assert(ms==portMAX_DELAY);pthread_mutex_lock(&q->mutex);
    while(!q->count)pthread_cond_wait(&q->cond,&q->mutex);
    *(uint8_t*)v=q->bytes[q->head];q->head=(q->head+1)%q->limit;--q->count;
    pthread_mutex_unlock(&q->mutex);return 1;
}
static void *thread_entry(void *p) {current_task=p;current_task->fn(current_task->arg);return NULL;}
static TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void*),const char *name,unsigned bytes,
        void *arg,unsigned priority,StackType_t *stack,StaticTask_t *tcb,int core) {
    assert(!strcmp(name,"nvs_worker") && bytes==4096 && priority==1 && stack && core==0);
    if(!create_ok())return NULL;tcb->fn=fn;tcb->arg=arg;assert(!pthread_create(&tcb->thread,NULL,thread_entry,tcb));return tcb;
}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return current_task;}
static bool xPortInIsrContext(void) {return isr;}
static unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) {(void)t;return 0;} /* Not a host measurement. */
'''

TEST = r'''
#include "nvs_worker.c"
#include <stdlib.h>
static pthread_mutex_t gate_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static bool gate_entered,gate_open;
static unsigned calls, observed;
static esp_err_t echo(void *p,size_t size) {
    assert(size==sizeof(unsigned));unsigned value=*(unsigned*)p;
    pthread_mutex_lock(&gate_mutex);++calls;observed=value;pthread_mutex_unlock(&gate_mutex);
    *(unsigned*)p=value+1;return value==99 ? ESP_FAIL : ESP_OK;
}
static esp_err_t block(void *p,size_t size) {
    unsigned before=*(unsigned*)p;
    pthread_mutex_lock(&gate_mutex);gate_entered=true;pthread_cond_broadcast(&gate_cond);
    while(!gate_open)pthread_cond_wait(&gate_cond,&gate_mutex);
    pthread_mutex_unlock(&gate_mutex);assert(*(unsigned*)p==before);return echo(p,size);
}
static esp_err_t other(void *p,size_t size) {return echo(p,size);}
static esp_err_t nested(void *p,size_t size) {
    assert(nvs_worker_call(echo,p,size,20)==ESP_ERR_INVALID_STATE);return ESP_OK;
}
static void gate(bool open) {
    pthread_mutex_lock(&gate_mutex);gate_open=open;if(!open)gate_entered=false;
    pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate_mutex);
}
static void entered(void) {
    pthread_mutex_lock(&gate_mutex);while(!gate_entered)pthread_cond_wait(&gate_cond,&gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
}
static void idle(void) {
    for(unsigned i=0;i<2000;i++) {
        nvs_worker_status_t s;assert(nvs_worker_get_status(&s)==ESP_OK);
        if(!s.in_use)return;usleep(1000);
    }assert(!"worker stuck");
}
static void *producer(void *p) {
    (void)p;unsigned accepted=0,rejected=0;
    /* Host scheduling may exhaust the API's 5 ms mutex budget. Check those
     * explicit failures and unchanged caller data; never count them as writes. */
    for(unsigned attempt=0;accepted<500 && attempt<1000;attempt++) {
        unsigned v=1;int err=nvs_worker_call(echo,&v,sizeof(v),1000);
        if(err==ESP_OK){assert(v==2);++accepted;}
        else {fprintf(stderr,"bounded submit result=%d value=%u\n",err,v);
            assert((err==ESP_ERR_TIMEOUT || err==ESP_ERR_NO_MEM) && v==1);++rejected;usleep(1000);}
    }
    assert(accepted==500);if(rejected)fprintf(stderr,"producer completed=500 rejected=%u\n",rejected);
    return NULL;
}
int main(int argc,char **argv) {
    unsigned value=7;
    assert(nvs_worker_call(echo,&value,sizeof(value),20)==ESP_ERR_INVALID_STATE);
    if(argc>1){fail_create=atoi(argv[1]);assert(nvs_worker_init()==ESP_ERR_NO_MEM);fail_create=0;}
    assert(nvs_worker_init()==ESP_OK && nvs_worker_init()==ESP_OK);
    assert(nvs_worker_call(NULL,&value,4,20)==ESP_ERR_INVALID_ARG);
    assert(nvs_worker_call(echo,NULL,4,20)==ESP_ERR_INVALID_ARG);
    assert(nvs_worker_call(echo,&value,513,20)==ESP_ERR_INVALID_SIZE);
    assert(nvs_worker_call(echo,&value,4,0)==ESP_ERR_INVALID_ARG);
    isr=true;assert(nvs_worker_submit_latest(echo,&value,4)==ESP_ERR_INVALID_STATE);isr=false;
    assert(nvs_worker_call(echo,&value,4,1000)==ESP_OK && value==8);
    value=99;assert(nvs_worker_call(echo,&value,4,1000)==ESP_FAIL && value==100);
    assert(nvs_worker_call(nested,&value,4,1000)==ESP_OK);

    gate(false);value=10;assert(nvs_worker_submit_latest(block,&value,4)==ESP_OK);entered();
    value=20;assert(nvs_worker_submit_latest(echo,&value,4)==ESP_OK);
    value=21;assert(nvs_worker_submit_latest(echo,&value,4)==ESP_OK);value=999;
    assert(nvs_worker_submit_latest(other,&value,4)==ESP_OK);
    /* Queued timeout is skipped; its slot cannot be reused before dequeuing. */
    assert(nvs_worker_call(echo,&value,4,20)==ESP_ERR_TIMEOUT && value==999);
    assert(nvs_worker_submit_latest(nested,&value,4)==ESP_ERR_NO_MEM);
    gate(true);idle();assert(calls==5 && observed==999);

    /* Timeout while running must keep the service's copy alive. */
    gate(false);value=42;assert(nvs_worker_call(block,&value,4,20)==ESP_ERR_TIMEOUT);
    entered();value=888;gate(true);idle();assert(observed==42);
    value=17;assert(nvs_worker_call(echo,&value,4,1000)==ESP_OK && value==18);
    pthread_t threads[3];for(int i=0;i<3;i++)assert(!pthread_create(&threads[i],NULL,producer,NULL));
    for(int i=0;i<3;i++)pthread_join(threads[i],NULL);idle();
    nvs_worker_status_t stats;assert(nvs_worker_get_status(&stats)==ESP_OK);
    assert(stats.coalesced==1 && stats.timeouts>=2 && stats.full>=1 && stats.failures==1 && stats.in_use==0);
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    for(unsigned i=0;i<NVS_WORKER_SLOTS;i++)for(unsigned j=0;j<sizeof(s_jobs[i]);j++)assert(!((uint8_t*)&s_jobs[i])[j]);
    xSemaphoreGive(s_mutex);
    puts("PASS: NVS owned snapshots, coalescing, full queue, queued/running timeout, errors, nested/ISR rejection, 1500 concurrent writes");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="s3-nvs-test-") as directory:
    tmp = Path(directory)
    (tmp / "freertos").mkdir()
    (tmp / "shim.h").write_text(SHIM)
    for name in ("esp_attr.h", "esp_log.h", "esp_err.h", "freertos/FreeRTOS.h",
                 "freertos/event_groups.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h"):
        (tmp / name).write_text('#include "shim.h"\n')
    (tmp / "test.c").write_text(TEST)
    exe = tmp / "test"
    subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-pthread", "-fno-pie", "-no-pie",
                    "-fsanitize=address,undefined", "-I", str(tmp),
                    "-I", str(ROOT / "components/nvs_worker/include"),
                    "-I", str(ROOT / "components/nvs_worker/src"), str(tmp / "test.c"), "-o", str(exe)], check=True)
    for fail in range(5):
        subprocess.run([str(exe)] + ([str(fail)] if fail else []), check=True, timeout=30)
