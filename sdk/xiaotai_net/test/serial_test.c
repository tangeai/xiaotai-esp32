// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_command.c::command
#include "internal.h"
#include "portal.h"
#include "trace_internal.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_net_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define WAIT 3000U
static const char *input;
static size_t input_left, output_used;
static char output[10000];
static bool no_part, xmini_profile;
static uint32_t activity;
static xiaotai_signal_t owner, restart;
static atomic_uint readbacks, probes;
static unsigned serial_submissions;
static xiaotai_net_driver_t driver;

static size_t read_port(void *ctx, uint8_t *data, size_t capacity, uint32_t ms)
{
    (void)ctx;
    if (!input_left) { xiaotai_delay_ms(ms); return 0; }
    const size_t take = input_left < capacity ? input_left : capacity;
    memcpy(data, input, take);
    input += take;
    input_left -= take;
    return take;
}

static xiaotai_err_t write_port(void *ctx, const char *data, size_t size)
{
    (void)ctx;
    assert(output_used + size + 1 < sizeof(output));
    memcpy(output + output_used, data, size);
    output_used += size;
    output[output_used] = '\0';
    return XIAOTAI_OK;
}

static void expected(const char *line)
{
    if (!strstr(output, line)) fprintf(stderr, "missing [%s] in [%s]\n", line, output);
    assert(strstr(output, line));
}

static void run(const char *line, const char *end)
{
    output_used = 0;
    output[0] = '\0';
    input = line;
    input_left = strlen(line);
    for (unsigned i = 0; i < 8 && input_left; ++i) assert(xiaotai_at_service(15) == XIAOTAI_OK);
    assert(!input_left);
    expected(end);
}

static void give(void *ctx) { xiaotai_signal_give(ctx); }

static void begin_activity(void *unused)
{
    (void)unused;
    assert(xiaotai_activity_begin(&(const xiaotai_activity_t){.name = "fixture",
               .resources = XIAOTAI_RESOURCE_AUDIO_OUT}, &activity) == XIAOTAI_OK);
}

static void end_activity(void *unused) { (void)unused; xiaotai_activity_end(activity); }

static void unexpected_reply(void *ctx, const char *line)
{
    (void)ctx;
    (void)line;
    assert(false);
}

static void settle(void)
{
    assert(xiaotai_net_host_sync(WAIT) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(WAIT) == XIAOTAI_OK);
    assert(xiaotai_run(give, &owner) == XIAOTAI_OK && xiaotai_signal_take(&owner, WAIT));
}

const xiaotai_network_t *xiaotai_hal_network(size_t index)
{
    static const xiaotai_network_t parts[] = {
        {.desc = {.kind = XIAOTAI_NETWORK_WIFI, .provisioning = XIAOTAI_PROVISION_SERIAL}},
        {.desc = {.kind = XIAOTAI_NETWORK_WIFI, .provisioning = XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL}},
    };
    return index || no_part ? NULL : &parts[xmini_profile ? 1 : 0];
}

static void report(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    assert(net_on_worker() && !xiaotai_on_owner());
    atomic_fetch_add(&readbacks, 1);
    reply->write(reply->ctx, "radio ok");
}

xiaotai_err_t __wrap_xiaotai_restart_request(const char *reason, uint32_t ticket);
xiaotai_err_t __wrap_xiaotai_restart_request(const char *reason, uint32_t ticket)
{
    assert(xiaotai_on_owner() && !ticket && !strcmp(reason, "network"));
    xiaotai_signal_give(&restart);
    return XIAOTAI_OK;
}

xiaotai_err_t __wrap_net_probe_platform_start(uint32_t ipv4, net_probe_ready_fn_t ready, net_probe_done_fn_t done);
xiaotai_err_t __wrap_net_probe_platform_start(uint32_t ipv4, net_probe_ready_fn_t ready, net_probe_done_fn_t done)
{
    assert(ipv4 && ready && done);
    atomic_fetch_add(&probes, 1);
    ready();
    done(XIAOTAI_OK, &(const net_probe_result_t){.unique = 3, .duplicate = 1, .invalid = 2, .max_seq = 4});
    return XIAOTAI_OK;
}

xiaotai_err_t __real_net_provision_submit_checked(const xiaotai_net_submission_t *submission, uint32_t token,
                                                 bool (*active)(uint32_t), net_portal_submit_result_t *receipt);
xiaotai_err_t __wrap_net_provision_submit_checked(const xiaotai_net_submission_t *submission, uint32_t token,
                                                 bool (*active)(uint32_t), net_portal_submit_result_t *receipt)
{
    assert(xiaotai_on_owner() && submission && submission->method == XIAOTAI_PROVISION_SERIAL);
    ++serial_submissions;
    return __real_net_provision_submit_checked(submission, token, active, receipt);
}

static xiaotai_err_t portal_start(uint32_t ipv4)
{
    assert(net_on_worker() && ipv4);
    return XIAOTAI_OK;
}

static xiaotai_err_t portal_stop(void) { return XIAOTAI_OK; }

typedef enum { BOOT_CONCURRENT, BOOT_EXCLUSIVE_SAVED, BOOT_XMINI_SETUP } boot_profile_t;

static void boot(boot_profile_t profile)
{
    assert(xiaotai_signal_init(&owner) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&restart) == XIAOTAI_OK);
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    xmini_profile = profile == BOOT_XMINI_SETUP;
    const bool exclusive = profile != BOOT_CONCURRENT;
    const xiaotai_net_config_t config = {
        .space = xmini_profile ? "xmini" : "wifi_cfg",
        .layout = exclusive ? XIAOTAI_NET_STORE_PLAIN : XIAOTAI_NET_STORE_RECORD,
        .provisioning = exclusive ? XIAOTAI_NET_PROVISIONING_EXCLUSIVE : XIAOTAI_NET_PROVISIONING_CONCURRENT,
        .serial_short_passwords = profile == BOOT_EXCLUSIVE_SAVED, .keep_running = !xmini_profile,
        .retry = {.first_ms = 30000, .max_ms = 30000},
    };
    assert(xiaotai_net_configure(&config) == XIAOTAI_OK);
    if (profile == BOOT_EXCLUSIVE_SAVED) {
        const xiaotai_net_credentials_t stored = {.ssid = "fixture", .password = "short"};
        assert(xiaotai_kv_set("wifi_cfg", "wifi", &stored, sizeof(stored), WAIT) == XIAOTAI_OK);
    }
    driver = *xiaotai_net_host_driver(XIAOTAI_NETWORK_WIFI);
    driver.report = report;
    assert(xiaotai_net_driver_register(&driver) == XIAOTAI_OK);
    if (xmini_profile) {
        static const net_provision_portal_io_t portal = {portal_start, portal_stop, portal_start, portal_stop};
        xiaotai_net_host_set_mac(XIAOTAI_OK, (const uint8_t[]){2, 3, 4, 5, 0xca, 0x88});
        assert(net_provision_portal_register(&portal) == XIAOTAI_OK);
    }
    static const xiaotai_at_io_t source = {.name = "net_test", .read = read_port, .write = write_port};
    assert(xiaotai_at_configure(&(const xiaotai_at_config_t){
        .task_stack_bytes = XIAOTAI_AT_NO_TASK, .no_platform_console = true,
    }) == XIAOTAI_OK);
    assert(xiaotai_at_attach(&source) == XIAOTAI_OK && xiaotai_at_open() == XIAOTAI_OK);
    static const xiaotai_module_t log = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const char *const at_depends[] = {"eventlog", NULL};
    static const xiaotai_module_t at = {.name = "at", .depends = at_depends,
        .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    static const xiaotai_module_t net = {.name = "net", .init = xiaotai_engine_net_init, .start = xiaotai_engine_net_start};
    const xiaotai_module_t *modules[] = {&log, &at, &net};
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 3}) == XIAOTAI_OK);
    settle();
}

static uint32_t token(xiaotai_net_host_call_t kind)
{
    xiaotai_net_host_record_t records[XIAOTAI_NET_HOST_RECORDS_MAX];
    const size_t count = xiaotai_net_host_records(records, XIAOTAI_NET_HOST_RECORDS_MAX);
    for (size_t i = count; i > 0; --i) if (records[i - 1].call == kind) return records[i - 1].token;
    return 0;
}

static void emit(const xiaotai_net_driver_event_t *event)
{
    assert(xiaotai_net_host_emit(event) == XIAOTAI_OK);
    settle();
}

static uint32_t connected(void)
{
    const uint32_t join = token(XIAOTAI_NET_HOST_JOIN);
    assert(join);
    emit(&(const xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_JOINED, .token = join, .ssid = "fixture", .channel = 11});
    uint32_t ip, gateway;
    memcpy(&ip, (const uint8_t[]){192, 168, 12, 44}, sizeof(ip));
    memcpy(&gateway, (const uint8_t[]){192, 168, 12, 1}, sizeof(gateway));
    emit(&(const xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = join, .ipv4 = ip, .gateway_ipv4 = gateway,
    });
    assert(xiaotai_net_generation());
    return join;
}

static void check_help(void)
{
    run("AT+HELP\r", "OK\r\n");
    expected("+HELP: AT+NET\r\n");
    for (const char *const *verb = (const char *const[]){
            "WIFI <ssid-hex> <password-hex|->", "STORE <ssid-hex> <password-hex|->", "RETRY", "DISCONNECT",
            "SCAN", "LIST", "DIAG", "PROBE", NULL,
        }; *verb; ++verb) {
        char line[100];
        (void)snprintf(line, sizeof(line), "+HELP: AT+NET %s\r\n", *verb);
        expected(line);
    }
#if XIAOTAI_NET_CONSOLE_FORGET
    expected("+HELP: AT+NET FORGET\r\n");
#else
    assert(!strstr(output, "AT+NET FORGET"));
#endif
}

static void serial(void)
{
    boot(BOOT_CONCURRENT);
    check_help();
    run("AT+NET\r", "+NET: state=unconfigured kind=wifi connected=0 provisioning=1 held=0 gen=0 ssid_hex=- ip=- gateway=- rssi=- channel=- mac=- error=0\r\n");
    const xiaotai_net_scan_record_t sample = {.ssid = "cafe", .signal_dbm = -50, .security = XIAOTAI_NET_SECURITY_WPA2};
    xiaotai_net_host_set_scan(&sample, 1, false);
    run("AT+NET SCAN\r", "OK\r\n");
    settle();
    run("AT+NET WIFI 66697874757265 -\r", "ERROR: BUSY\r\n");
    emit(&(const xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = token(XIAOTAI_NET_HOST_SCAN)});
    run("AT+NET LIST\r", "+NET-AP: ssid_hex=63616665 rssi=-50 secure=1 saved=0 available=1 supported=1\r\n");
    run("AT+NET STORE 00 -\r", "ERROR: INVALID_ARG\r\n");
    run("AT+NET STORE 1 -\r", "ERROR: INVALID_ARG\r\n");
    run("AT+NET STORE 66697874757265 3132333435\r", "ERROR: INVALID_ARG\r\n");
    const xiaotai_reply_t nonconsole = {.write = unexpected_reply};
    assert(xiaotai_command_run("AT+NET WIFI 66697874757265 -", &nonconsole, WAIT) == XIAOTAI_ERR_NOT_ALLOWED);
    assert(xiaotai_command_run("AT+NET STORE 66697874757265 -", &nonconsole, WAIT) == XIAOTAI_ERR_NOT_ALLOWED);
    assert(!xiaotai_net_has_saved_credentials());
    assert(xiaotai_run(begin_activity, NULL) == XIAOTAI_OK);
    settle();
    run("AT+NET WIFI 66697874757265 -\r", "ERROR: BUSY\r\n");
    run("AT+NET STORE 66697874757265 -\r", "ERROR: BUSY\r\n");
    assert(xiaotai_activity_active() && !xiaotai_net_has_saved_credentials());
    assert(xiaotai_run(end_activity, NULL) == XIAOTAI_OK);
    settle();
    uint32_t lease;
    assert(xiaotai_net_maintenance_begin(&lease) == XIAOTAI_OK);
    run("AT+NET STORE 66697874757265 -\r", "ERROR: INVALID_STATE MAINTENANCE\r\n");
    xiaotai_net_maintenance_end(lease);
    run("AT+NET STORE 66697874757265 -\r", "+NET-WIFI: accepted=1 verified=0 saved=1 restart=0\r\n");
    run("AT+NET RETRY\r", "OK\r\n");
    settle();
    const uint32_t join = connected();
    run("AT+NET\r", "ssid_hex=66697874757265 ip=192.168.12.44 gateway=192.168.12.1");
    expected("channel=11");
    run("AT+NET PROBE\r", "OK\r\n");
    assert(xiaotai_at_service(15) == XIAOTAI_OK);
    expected("+NET-PROBE: unique=3 duplicate=1 invalid=2 max_seq=4 phases=");
    assert(atomic_load(&probes) == 1);
    run("AT+NET DISCONNECT\r", "OK\r\n");
    emit(&(const xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_LOST, .token = join, .loss = XIAOTAI_NET_LOSS_REQUESTED});
    run("AT+NET WIFI 6E6577 -\r", "+NET-WIFI: accepted=1 verified=0 saved=0 restart=0\r\n");
    settle();
    assert(xiaotai_at_service(15) == XIAOTAI_OK);
    expected("+NET-WIFI: provision=verifying error=0\r\n");
}

static void check_not_saved(void)
{
    xiaotai_net_credentials_t saved;
    size_t bytes = sizeof(saved);
    assert(xiaotai_kv_get_blob_strict("xmini", "wifi", &saved, &bytes, WAIT) == XIAOTAI_ERR_NOT_FOUND);
    assert(!xiaotai_signal_take(&restart, 0));
}

static uint32_t submitted(const char *command, const char *ssid, const char *password, size_t joins)
{
    const unsigned before = serial_submissions;
    run(command, "+NET-WIFI: accepted=1 verified=0 saved=0 restart=1\r\nOK\r\n");
    assert(serial_submissions == before + 1U);
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_JOIN, joins, WAIT));
    settle();
    xiaotai_net_host_record_t records[XIAOTAI_NET_HOST_RECORDS_MAX];
    const size_t count = xiaotai_net_host_records(records, XIAOTAI_NET_HOST_RECORDS_MAX);
    const xiaotai_net_host_record_t *joined = NULL;
    for (size_t i = 0; i < count; ++i)
        if (records[i].call == XIAOTAI_NET_HOST_JOIN) joined = &records[i];
    assert(joined && joined->token && joined->has_credentials);
    assert(!strcmp(joined->credentials.ssid, ssid) && !strcmp(joined->credentials.password, password));
    xiaotai_net_provision_status_t provision;
    xiaotai_net_provision_status(&provision);
    assert(provision.state == XIAOTAI_NET_PROVISION_VERIFYING && !strcmp(provision.ssid, ssid));
    check_not_saved();
    return joined->token;
}

static void xmini_serial(void)
{
    boot(BOOT_XMINI_SETUP);
    assert(xiaotai_net_provisioning_boot() && xiaotai_net_provisioning_exclusive());
    xiaotai_net_provision_status_t provision;
    xiaotai_net_provision_status(&provision);
    assert(provision.state == XIAOTAI_NET_PROVISION_OPEN &&
           provision.methods == (XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL));
    assert(!strcmp(provision.url, "http://192.168.6.1"));

    char line[224];
    const size_t start = (size_t)snprintf(line, sizeof(line), "AT+NET WIFI ");
    memset(line + start, 'a', 64U);
    line[start + 64U] = ' ';
    memset(line + start + 65U, 'b', 130U);
    memcpy(line + start + 195U, "\r", 2U);
    run(line, "ERROR: INVALID_ARG\r\n"); /* 65-byte password. */
    static const char *const invalid[] = {
        "AT+NET WIFI - -\r", "AT+NET WIFI 00 -\r", "AT+NET WIFI 5 -\r", "AT+NET WIFI 5g -\r",
        "AT+NET WIFI 5469 00\r", "AT+NET WIFI 5469 31323334\r", "AT+NET WIFI 5469 - -\r",
        "AT+NET WIFI 5469 3132333435363738 trailing\r",
        "AT+NET WIFI aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa -\r", /* 33-byte SSID. */
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) run(invalid[i], "ERROR: INVALID_ARG");
    settle();
    xiaotai_net_diag_t diag;
    xiaotai_net_diag(&diag);
    assert(!serial_submissions && !diag.submissions && !net_provision_submission_pending() && !token(XIAOTAI_NET_HOST_JOIN));
    check_not_saved();

    const uint32_t first = submitted("AT+NET WIFI 5469 3132333435363738\r", "Ti", "12345678", 1);
    emit(&(const xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_LOST, .token = first,
         .loss = XIAOTAI_NET_LOSS_REFUSED});

    memcpy(line + start + 193U, "\r", 2U);
    char ssid[33], password[65];
    memset(ssid, 0xaa, sizeof(ssid) - 1U); ssid[32] = 0;
    memset(password, 0xbb, sizeof(password) - 1U); password[64] = 0;
    const uint32_t maximum = submitted(line, ssid, password, 2);
    emit(&(const xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_LOST, .token = maximum,
         .loss = XIAOTAI_NET_LOSS_REFUSED});

    const char chinese[] = "\xe5\xb0\x8f\xe9\x92\x9b";
    const uint32_t open = submitted("AT+NET WIFI e5b08fe9929b -\r", chinese, "", 3);
    xiaotai_net_driver_event_t joined = {.kind = XIAOTAI_NET_DRIVER_JOINED, .token = open};
    memcpy(joined.ssid, chinese, sizeof(chinese));
    emit(&joined);
    emit(&(const xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = open, .ipv4 = 42});
    assert(xiaotai_signal_take(&restart, WAIT));
    xiaotai_net_provision_status(&provision);
    assert(provision.state == XIAOTAI_NET_PROVISION_DONE && provision.restart_required);
    xiaotai_net_credentials_t saved;
    size_t bytes = sizeof(saved);
    assert(xiaotai_kv_get_blob_strict("xmini", "wifi", &saved, &bytes, WAIT) == XIAOTAI_OK && bytes == sizeof(saved));
    assert(!strcmp(saved.ssid, chinese) && !saved.password[0]);
    printf("xmini Wi-Fi AT: 32/64 accepted; 33/65 and eight malformed inputs rejected before submit; Chinese open network verified and saved\n");
}

static void diagnostics(void)
{
    boot(BOOT_EXCLUSIVE_SAVED);
    (void)connected();
    for (unsigned i = 0; i < 10; ++i) {
        emit(&(const xiaotai_net_driver_event_t){
            .kind = XIAOTAI_NET_DRIVER_LOST, .token = token(XIAOTAI_NET_HOST_JOIN),
            .loss = XIAOTAI_NET_LOSS_SIGNAL, .code = (int32_t)(100 + i), .signal_valid = true, .signal_dbm = -70,
        });
        assert(xiaotai_net_retry() == XIAOTAI_OK);
        settle();
    }
    run("AT+NET DIAG\r", "OK\r\n");
    expected("losses=10 signal_losses=10");
    expected("loss=signal code=100 rssi=-70\r\n");
    expected("type=loss index=0");
    expected("loss=signal code=102 rssi=-70\r\n");
    expected("loss=signal code=109 rssi=-70\r\n");
    assert(strstr(output, "code=102") < strstr(output, "code=109") && !strstr(output, "code=101"));
    expected("+NET-DIAG: type=driver driver_hex=726164696F206F6B\r\n");
    assert(atomic_load(&readbacks) == 1);
}

int main(int argc, char **argv)
{
    assert(argc <= 2);
    const char *mode = argc == 2 ? argv[1] : "serial";
    if (!strcmp(mode, "serial")) serial();
    else if (!strcmp(mode, "xmini")) xmini_serial();
    else if (!strcmp(mode, "diag")) diagnostics();
    else if (!strcmp(mode, "blocked")) {
        no_part = true;
        boot(BOOT_CONCURRENT);
        run("AT+NET STORE 00 -\r", "ERROR: INVALID_STATE NOT_STARTED\r\n");
    } else if (!strcmp(mode, "exclusive")) {
        boot(BOOT_EXCLUSIVE_SAVED);
        run("AT+NET WIFI 66697874757265 3132333435\r", "ERROR: INVALID_STATE NOT_PROVISIONING\r\n");
        run("AT+NET STORE 66697874757265 3132333435\r", "+NET-WIFI: accepted=1 verified=0 saved=1 restart=0\r\n");
    } else if (!strcmp(mode, "forget")) {
        boot(BOOT_EXCLUSIVE_SAVED);
#if XIAOTAI_NET_CONSOLE_FORGET
        assert(xiaotai_net_has_saved_credentials());
        const xiaotai_reply_t nonconsole = {.write = unexpected_reply};
        assert(xiaotai_command_run("AT+NET FORGET", &nonconsole, WAIT) == XIAOTAI_ERR_NOT_ALLOWED);
        assert(xiaotai_net_has_saved_credentials() && !xiaotai_signal_take(&restart, 0));
        run("AT+NET FORGET\r", "OK\r\n");
        assert(xiaotai_signal_take(&restart, WAIT));
        xiaotai_net_provision_status_t status;
        xiaotai_net_provision_status(&status);
        assert(status.restart_required && !xiaotai_net_has_saved_credentials());
#else
        run("AT+NET FORGET\r", "ERROR: INVALID_ARG UNKNOWN_VERB\r\n");
        assert(xiaotai_net_has_saved_credentials() && !xiaotai_signal_take(&restart, 0));
#endif
    } else assert(false);
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&owner);
    xiaotai_signal_deinit(&restart);
    return 0;
}
