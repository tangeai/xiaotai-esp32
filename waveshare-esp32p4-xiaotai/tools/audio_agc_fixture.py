"""Host-only AGC API fixture; no device I/O."""
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "starter_agc_stream.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_ARG 4
#define ESP_AGC_SUCCESS 0
#define AGC_MODE_2 2
#define AGC_MODE_3 3
#define EXT_RAM_BSS_ATTR
#define ESP_LOGI(...) ((void)0)
static struct { int mode, rate, gain, limiter, target; } state[2];
static int opened, closed, fail_allocation_at, internal_mode, fail_process_mode;
static void *esp_agc_open(int mode, int rate) {
    assert((mode==2 && rate==16000) || (mode==3 && rate==8000));
    ++opened;
    if (opened == fail_allocation_at) return NULL;
    state[mode-2].mode=mode; state[mode-2].rate=rate;
    return &state[mode-2];
}
static bool esp_ptr_external_ram(void *p) { return p != &state[internal_mode==3 ? 1 : 0] || internal_mode==0; }
static void esp_agc_close(void *p) { assert(p==&state[0] || p==&state[1]); ++closed; }
static void set_agc_config(void *p, int g, int l, int t) {
    int i=p==&state[0] ? 0 : 1;
    assert(p==&state[i]); state[i].gain=g; state[i].limiter=l; state[i].target=t;
}
static int esp_agc_process(void *p, int16_t *in, int16_t *out, int n, int rate) {
    int i=p==&state[0] ? 0 : 1;
    assert(p==&state[i] && rate==state[i].rate && n==rate/100 && in!=out);
    if (fail_process_mode==state[i].mode) return -1;
    for(int k=0;k<n;k++) out[k]=in[k];
    return 0;
}
void starter_agc_deinit(void);
'''
