// SPDX-License-Identifier: MIT
/* Compile the actual native adapter against typed Hosted doubles, without
 * an ESP build, a radio, a reset or any flash operation. */
#include "coprocessor_link.h"
#include "hosted_fake.h"
#include "xiaotai/port/os.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int init_result, connect_result, wait_result, stop_result, calls;
static xiaotai_err_t version_result, app_result, write_result;
static bool alloc_fails, rpc_fails, synthetic_app;
static int rpc_reply_status, rpc_outstanding;
static uint32_t last_wait;
static const uint8_t bytes[] = {4, 0, 2, 7};

int eh_host_init(const eh_host_init_cfg_t *cfg) { assert(!cfg); calls = calls * 10 + 1; return init_result; }
int eh_host_connect_to_slave(void) { calls = calls * 10 + 2; return connect_result; }
int eh_host_deinit(void) { calls = calls * 10 + 3; return stop_result; }
int eh_host_wait_auto_init_ready(uint32_t timeout_ms) { last_wait = timeout_ms; return wait_result; }
uint8_t eh_host_mcu_transport_get_chip_id(void) { return 0x14; }
uint32_t eh_host_mcu_transport_get_fw_version(void) { return 0x102; }
uint8_t eh_host_mcu_transport_get_capabilities(void) { return 0x0d; }
uint32_t eh_host_mcu_transport_get_ext_capabilities(void) { return 0x104; }
bool eh_host_mcu_transport_peer_advertised_rpc_version(void) { return true; }
const struct eh_priv_sdio_buf_config *eh_host_mcu_transport_sdio_buf_config(void)
{
    return (const void *)&last_wait;
}

esp_err_t eh_host_sys_get_cp_fw_version(eh_host_coprocessor_fwver_t *info)
{
    *info = (eh_host_coprocessor_fwver_t){2, 12, 13, 16, 23, 99};
    return version_result;
}

esp_err_t eh_host_sys_get_cp_app_desc(esp_hosted_app_desc_t *info)
{
    if (!synthetic_app) memset(info->project_name, 'p', sizeof(info->project_name));
    memcpy(info->version, "fixture", sizeof("fixture"));
    memcpy(info->idf_ver, "idf", sizeof("idf"));
    info->app_elf_sha256[31] = 123;
    info->magic_word = 0xabcd5432;
    return app_result;
}

eh_rpc_ctrl_cmd_t *eh_rpc_ctrl_cmd_alloc(void)
{
    if (alloc_fails) return NULL;
    eh_rpc_ctrl_cmd_t *r = calloc(1, sizeof(*r));
    assert(r);
    ++rpc_outstanding;
    return r;
}

void eh_rpc_ctrl_cmd_free(eh_rpc_ctrl_cmd_t *r) { free(r); --rpc_outstanding; }

int eh_host_feat_rpc_request_sync(int32_t id, void *req, void **out)
{
    assert(id == 257 && ((const eh_rpc_ctrl_cmd_t *)req)->u.wifi_mac.mode == WIFI_IF_STA);
    eh_rpc_ctrl_cmd_free(req);
    if (rpc_fails) return -1;
    eh_rpc_ctrl_cmd_t *reply = eh_rpc_ctrl_cmd_alloc();
    assert(reply);
    reply->resp_event_status = rpc_reply_status;
    *out = reply;
    return 0;
}

esp_err_t eh_host_cp_ota_begin(void) { calls = calls * 10 + 1; return ESP_OK; }
esp_err_t eh_host_cp_ota_write(const uint8_t *data, uint32_t size)
{
    assert(data == bytes && size == sizeof(bytes));
    calls = calls * 10 + 2;
    return write_result;
}
esp_err_t eh_host_cp_ota_end(void) { calls = calls * 10 + 3; return ESP_OK; }
esp_err_t eh_host_cp_ota_activate(void) { calls = calls * 10 + 4; return ESP_OK; }

static void lifecycle(void)
{
    assert(xiaotai_coprocessor_link_wait(200) == XIAOTAI_OK && last_wait == 200);
    wait_result = -1;
    assert(xiaotai_coprocessor_link_wait(77) == XIAOTAI_ERR_TIMEOUT && last_wait == 77);
    init_result = -ENOMEM;
    assert(xiaotai_coprocessor_link_start(100) == XIAOTAI_ERR_NO_MEM && calls == 1);
    init_result = 0;
    connect_result = -ETIMEDOUT;
    assert(xiaotai_coprocessor_link_start(100) == XIAOTAI_ERR_TIMEOUT && calls == 112);
    stop_result = -1;
    assert(xiaotai_coprocessor_link_stop() == XIAOTAI_FAIL && calls == 1123);
    stop_result = connect_result = 0;
    assert(xiaotai_coprocessor_link_stop() == XIAOTAI_OK);
    assert(xiaotai_coprocessor_link_start(100) == XIAOTAI_OK && calls == 1123312);
    assert(xiaotai_coprocessor_link_chip() == 0x14);
    xiaotai_coprocessor_link_facts_t facts;
    xiaotai_coprocessor_link_facts(&facts);
    assert(facts.capabilities == 0x0d && facts.ext_capabilities == 0x104 && facts.firmware == 0x102);
    assert(facts.rpc_version_advertised && facts.bus_buffer_config);
}

static void requests(void)
{
    xiaotai_coprocessor_version_t ver;
    assert(xiaotai_coprocessor_link_version(&ver) == XIAOTAI_OK && ver.major == 2 && ver.minor == 12 && ver.patch == 13);
    version_result = XIAOTAI_ERR_TIMEOUT;
    assert(xiaotai_coprocessor_link_version(&ver) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_coprocessor_app_t desc = {0};
    assert(xiaotai_coprocessor_link_app(&desc) == XIAOTAI_OK && desc.magic == 0xabcd5432 && desc.elf_sha256[31] == 123);
    assert(!memchr(desc.project, '\0', sizeof(desc.project))); /* no invented terminator */
    assert(!strcmp(desc.version, "fixture") && !strcmp(desc.platform, "idf"));
    synthetic_app = true;
    assert(xiaotai_coprocessor_link_app(&desc) == ESP_ERR_INVALID_RESPONSE);
    app_result = XIAOTAI_ERR_NOT_SUPPORTED;
    assert(xiaotai_coprocessor_link_app(&desc) == XIAOTAI_ERR_NOT_SUPPORTED);
    bool matched = true;
    alloc_fails = true;
    assert(xiaotai_coprocessor_link_probe_unversioned(&matched) == XIAOTAI_ERR_NO_MEM && !matched);
    alloc_fails = false;
    rpc_fails = true;
    assert(xiaotai_coprocessor_link_probe_unversioned(&matched) == XIAOTAI_FAIL && !matched && !rpc_outstanding);
    rpc_fails = false;
    assert(xiaotai_coprocessor_link_probe_unversioned(&matched) == XIAOTAI_OK && !matched && !rpc_outstanding);
    rpc_reply_status = ESP_ERR_WIFI_NOT_INIT;
    assert(xiaotai_coprocessor_link_probe_unversioned(&matched) == XIAOTAI_OK && matched && !rpc_outstanding);

    calls = 0;
    assert(xiaotai_coprocessor_link_ota_begin() == XIAOTAI_OK);
    assert(xiaotai_coprocessor_link_ota_write(NULL, 1) == XIAOTAI_ERR_INVALID_SIZE && calls == 1);
    assert(xiaotai_coprocessor_link_ota_write(bytes, 0) == XIAOTAI_ERR_INVALID_SIZE && calls == 1);
#if SIZE_MAX > UINT32_MAX
    assert(xiaotai_coprocessor_link_ota_write(bytes, UINT64_C(1) << 32) == XIAOTAI_ERR_INVALID_SIZE && calls == 1);
#endif
    write_result = XIAOTAI_ERR_TIMEOUT;
    assert(xiaotai_coprocessor_link_ota_write(bytes, sizeof(bytes)) == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_coprocessor_link_ota_end() == XIAOTAI_OK && xiaotai_coprocessor_link_ota_activate() == XIAOTAI_OK);
    assert(calls == 1234);
}

#if CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA_READY
enum { CHANNEL = 0x57545331 };
esp_event_base_t EH_HOST_EVENT = "hosted";
static esp_event_handler_t handler;
static unsigned registers, sends, waits, frees;
static int send_result, register_result;
static bool synchronous, deferred, trace;
static char *pending_free;
static const char *expected_word = "INFO";

esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t callback, void *ctx)
{
    assert(base == EH_HOST_EVENT && id == EH_HOST_EVENT_PEER_DATA_RX && !ctx);
    ++registers;
    if (register_result == ESP_OK) handler = callback;
    return register_result;
}

void __real_free(void *ptr);
void __wrap_free(void *ptr)
{
    if (pending_free && ptr == pending_free) {
        pending_free = NULL;
        ++frees;
    }
    __real_free(ptr);
}

static void emit(uint32_t channel, const char *bytes_, size_t n)
{
    assert(handler && !pending_free);
    const unsigned before = frees;
    pending_free = malloc(n);
    assert(pending_free);
    memcpy(pending_free, bytes_, n);
    const eh_host_event_peer_data_t msg = {.msg_id = channel, .data = (const uint8_t *)pending_free, .len = n};
    handler(NULL, EH_HOST_EVENT, EH_HOST_EVENT_PEER_DATA_RX, (void *)&msg);
    assert(!pending_free && frees == before + 1);
}

esp_err_t eh_host_peer_data_send(uint32_t channel, const uint8_t *data, size_t size)
{
    assert(channel == CHANNEL && size == strlen(expected_word) && !memcmp(data, expected_word, size));
    ++sends;
    if (trace) emit(CHANNEL, "alloc_trace=1", 13);
    if (synchronous) {
        emit(CHANNEL + 1, "elf=ignored", 11);
        emit(CHANNEL, "elf=early", 9);
    }
    return send_result;
}

bool __real_xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout_ms);
bool __wrap_xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout_ms)
{
    if (timeout_ms) {
        assert(timeout_ms == 1000);
        ++waits;
        if (deferred) emit(CHANNEL, "elf=later", 9);
    }
    return __real_xiaotai_signal_take(signal, 0);
}

static void peer_data(void)
{
    char reply[40];
    assert(xiaotai_coprocessor_link_command(CHANNEL + 1, "INFO", true, reply, sizeof(reply)) ==
           XIAOTAI_ERR_NOT_SUPPORTED && !registers);
    register_result = XIAOTAI_ERR_NO_MEM;
    assert(xiaotai_coprocessor_link_command(CHANNEL, "INFO", true, reply, sizeof(reply)) ==
           XIAOTAI_ERR_NO_MEM && registers == 1 && sends == 0);
    register_result = 0;
    trace = synchronous = true;
    assert(xiaotai_coprocessor_link_command(CHANNEL, "INFO", true, reply, sizeof(reply)) == XIAOTAI_OK);
    assert(!strcmp(reply, "elf=early\nalloc_trace=1") && registers == 2 && frees == 3);
    assert(xiaotai_coprocessor_link_command(CHANNEL, "INFO", true, reply, 7) == XIAOTAI_OK);
    assert(!strcmp(reply, "elf=ea") && waits == 2 && sends == 2 && frees == 6);
    trace = synchronous = false;
    deferred = true;
    expected_word = "SINK0";
    assert(xiaotai_coprocessor_link_command(CHANNEL, "SINK0", true, reply, sizeof(reply)) == XIAOTAI_OK);
    assert(!strcmp(reply, "elf=later") && waits == 3 && frees == 7);
    deferred = false;
    trace = true;
    expected_word = "INFO";
    assert(xiaotai_coprocessor_link_command(CHANNEL, "INFO", true, reply, sizeof(reply)) == XIAOTAI_ERR_TIMEOUT);
    assert(!reply[0] && waits == 4 && sends == 4);
    emit(CHANNEL, "elf=too_late", 12);
    trace = false;
    assert(xiaotai_coprocessor_link_command(CHANNEL, "INFO", true, reply, sizeof(reply)) ==
           XIAOTAI_ERR_INVALID_STATE && sends == 4);

    expected_word = "ROLLBACK";
    send_result = XIAOTAI_ERR_TIMEOUT;
    assert(xiaotai_coprocessor_link_command(CHANNEL, "ROLLBACK", false, NULL, 0) == XIAOTAI_ERR_TIMEOUT && waits == 4);
    send_result = 0;
    assert(xiaotai_coprocessor_link_command(CHANNEL, "ROLLBACK", false, NULL, 0) == XIAOTAI_OK && waits == 4);
    emit(CHANNEL, "rollback=ERROR_NO_VERIFIED_PREVIOUS_IMAGE", strlen("rollback=ERROR_NO_VERIFIED_PREVIOUS_IMAGE"));
    assert(sends == 6 && !pending_free);
}
#else
static void peer_data(void)
{
    assert(xiaotai_coprocessor_link_command(1, "INFO", false, NULL, 0) == XIAOTAI_ERR_NOT_SUPPORTED);
}
#endif

int main(void)
{
    lifecycle();
    requests();
    peer_data();
    return 0;
}
