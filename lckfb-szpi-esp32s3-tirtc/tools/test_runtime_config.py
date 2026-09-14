#!/usr/bin/env python3
"""NVS adapter result/ownership/validation tests; no physical Flash access."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "runtime_config.h"
#include "nvs_worker.h"
static int transport_error, operation_error, operation, fail_at;
static unsigned writes, commits, closes;
static bool on_worker, optional_missing;
static const char *stored[] = {"test-id", "test-secret", "test-client"};
static int next(void) {return ++operation==fail_at ? operation_error : ESP_OK;}
static unsigned key_index(const char *key) {
    if(!strcmp(key,"device_id"))return 0;if(!strcmp(key,"secret"))return 1;
    assert(!strcmp(key,"client_id"));return 2;
}
static int nvs_open(const char *name,int mode,unsigned *handle) {
    assert(on_worker && !strcmp(name,"tirtc_cfg") && (mode==1 || mode==2));
    int err=next();if(!err)*handle=1;return err;
}
static int nvs_get_str(unsigned handle,const char *key,char *out,size_t *size) {
    assert(on_worker && handle==1);unsigned i=key_index(key);int err=next();if(err)return err;
    if(i==2 && optional_missing)return ESP_ERR_NVS_NOT_FOUND;
    assert(*size>strlen(stored[i]));strcpy(out,stored[i]);*size=strlen(out)+1;return ESP_OK;
}
static int nvs_set_str(unsigned handle,const char *key,const char *value) {
    assert(on_worker && handle==1 && !strcmp(value,stored[key_index(key)]));++writes;return next();
}
static int nvs_erase_key(unsigned handle,const char *key) {
    assert(on_worker && handle==1 && !strcmp(key,"endpoint"));return next();
}
static int nvs_erase_all(unsigned handle) {assert(on_worker && handle==1);return next();}
static int nvs_commit(unsigned handle) {assert(on_worker && handle==1);++commits;return next();}
static void nvs_close(unsigned handle) {assert(on_worker && handle==1);++closes;}
esp_err_t nvs_worker_call(nvs_worker_fn_t fn,void *data,size_t size,uint32_t timeout) {
    if(transport_error)return transport_error;
    _Alignas(max_align_t) uint8_t copy[NVS_WORKER_DATA_BYTES]={0};
    assert(size<=sizeof(copy) && timeout==NVS_WORKER_WAIT_MS);
    if(size)memcpy(copy,data,size);on_worker=true;int err=fn(copy,size);on_worker=false;
    if(size)memcpy(data,copy,size);return err;
}
#include "runtime_config.c"
static void reset(void) {transport_error=operation=fail_at=0;writes=commits=closes=0;optional_missing=false;operation_error=-99;}
int main(void) {
    runtime_tirtc_config_t config={"test-id","test-secret","test-client"};
    assert(runtime_config_tirtc_valid(&config,NULL,0));
    runtime_tirtc_config_t bad=config;memset(bad.device_id,'x',sizeof(bad.device_id));
    assert(!runtime_config_tirtc_valid(&bad,NULL,0));bad=config;memset(bad.device_secret,'x',sizeof(bad.device_secret));
    assert(!runtime_config_tirtc_valid(&bad,NULL,0));bad=config;memset(bad.client_id,'x',sizeof(bad.client_id));
    assert(!runtime_config_tirtc_valid(&bad,NULL,0));
    assert(runtime_config_save_tirtc(&bad)==ESP_ERR_INVALID_ARG && writes==0);
    assert(runtime_config_load_tirtc(NULL)==ESP_ERR_INVALID_ARG);
    reset();assert(runtime_config_save_tirtc(&config)==ESP_OK && writes==3 && commits==1 && closes==1);
    for(int i=1;i<=6;i++) {reset();fail_at=i;assert(runtime_config_save_tirtc(&config)==-99);assert(closes==(i>1));}
    reset();assert(runtime_config_load_tirtc(&bad)==ESP_OK && !memcmp(&bad,&config,sizeof(bad)));
    reset();optional_missing=true;assert(runtime_config_load_tirtc(&bad)==ESP_OK && !bad.client_id[0]);
    for(int i=1;i<=4;i++) {reset();fail_at=i;memset(&bad,0xff,sizeof(bad));assert(runtime_config_load_tirtc(&bad)==-99);
        for(size_t j=0;j<sizeof(bad);j++)assert(!((uint8_t*)&bad)[j]);assert(closes==(i>1));}
    reset();transport_error=-80;assert(runtime_config_save_tirtc(&config)==-80 && !writes);
    memset(&bad,0xff,sizeof(bad));assert(runtime_config_load_tirtc(&bad)==-80);
    for(size_t j=0;j<sizeof(bad);j++)assert(!((uint8_t*)&bad)[j]);
    reset();assert(runtime_config_clear_tirtc()==ESP_OK && commits==1 && closes==1);
    for(int i=1;i<=3;i++){reset();fail_at=i;assert(runtime_config_clear_tirtc()==-99 && closes==(i>1));}
    puts("PASS: binding NVS adapter isolation, bounded strings, required/optional fields, failure propagation and zeroed failed reads");
}
'''
with tempfile.TemporaryDirectory(prefix="s3-binding-nvs-") as directory:
    tmp = Path(directory)
    (tmp / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG -1\n#define ESP_ERR_INVALID_SIZE -2\n#define ESP_ERR_NVS_NOT_FOUND -3\n")
    (tmp / "nvs.h").write_text("typedef unsigned nvs_handle_t;\n#define NVS_READONLY 1\n#define NVS_READWRITE 2\n")
    (tmp / "test.c").write_text(HARNESS)
    exe = tmp / "test"
    subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
                    "-I", str(tmp), "-I", str(ROOT / "components/runtime_config/include"),
                    "-I", str(ROOT / "components/runtime_config/src"),
                    "-I", str(ROOT / "components/nvs_worker/include"), str(tmp / "test.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
