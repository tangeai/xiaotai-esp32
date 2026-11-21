// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_platform_state.c::platform_state
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "binding_internal.h"
#include "fake_platform.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

static const char grant[] = "{\"type\":\"auth_grant\",\"payload\":{\"device_id\":\"fixture\",\"device_key\":\"fixture-key\"}}";

static xiaotai_binding_status_t status(void)
{
    xiaotai_binding_status_t value;
    xiaotai_binding_status(&value);
    return value;
}

static void emit(const char *name, const void *data, size_t size)
{
    const xiaotai_args_t args = {0};
    assert(xiaotai_event_emit(name, &args, data, size) == XIAOTAI_OK);
    fake_drain_events();
}

static int64_t start(void)
{
    static const xiaotai_module_t binding = {
        .name = "binding", .init = xiaotai_module_binding_init, .start = xiaotai_module_binding_start,
    };
    static const xiaotai_module_t *const modules[] = {&binding};
    static const xiaotai_framework_config_t framework = {.modules = modules, .module_count = 1};
    const xiaotai_binding_config_t config = {.wait_s = 5, .guest_subscribe_timeout_s = 20,
        .resume_unbound_window_on_reconnect = true};
    xiaotai_binding_reset();
    fake_reset();
    fake_clock_start();
    assert(xiaotai_binding_configure(&config) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    assert(fake_drain());
    emit(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, NULL, 0);
    assert(g_request.count == 1 && status().phase == XIAOTAI_BINDING_REPORTING);
    const int64_t issued = xiaotai_time_us();
    fake_reply(200, 200, fake_report_body("001234", "fixture-token", "guest-fixture"));
    fake_guest_ready();
    assert(status().phase == XIAOTAI_BINDING_WAITING && status().seconds_left == 5);
    return issued;
}

static void assert_hidden(void)
{
    const xiaotai_binding_status_t shown = status();
    assert(!shown.verification[0] && !shown.seconds_left);
}

static void lose_wifi(bool user)
{
    g_net.state = XIAOTAI_NET_WAITING;
    g_net.user_disconnected = user;
    g_platform.phase = XIAOTAI_PLATFORM_STOPPED;
    emit(XIAOTAI_EVENT_NET_DOWN, &g_net, sizeof(g_net));
    assert_hidden();
}

static void recover_wifi(void)
{
    assert(!g_net.user_disconnected);
    g_net.state = XIAOTAI_NET_UP;
    g_platform.phase = XIAOTAI_PLATFORM_NO_IDENTITY;
    emit(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, NULL, 0);
}

static void old_grant(void *ctx)
{
    const fake_guest_t *old = ctx;
    const xiaotai_platform_signal_t signal = {.type = "auth_grant", .json = grant,
        .size = sizeof(grant) - 1, .generation = 1, .command = true};
    old->guest.message(old->id, &signal, old->guest.ctx);
}

int main(void)
{
    int64_t issued = start();
    fake_guest_t old = g_guest;
    fake_guest_unready();
    assert_hidden();
    fake_clock_advance_to(issued + 1000000);
    fake_guest_ready();
    assert(!strcmp(status().verification, "001234") && status().seconds_left == 4);

    lose_wifi(false);
    recover_wifi(); /* The platform is back before the old guest's deferred close. */
    assert(g_guest.opens == 1 && g_request.count == 1);
    fake_clock_advance_to(issued + 2000000);
    fake_guest_closed(XIAOTAI_ERR_PLATFORM_OFFLINE);
    fake_drain_events();
    assert(g_guest.opens == 2 && g_request.count == 1 && g_guest.id != old.id);
    assert(g_guest.guest.initial_subscribe_deadline_us == issued + 5000000);
    assert_hidden();
    fake_guest_ready();
    assert(!strcmp(status().verification, "001234") && status().seconds_left == 3);
    fake_on_owner(old_grant, &old);
    assert(!g_identity.saves);
    fake_guest_message("auth_grant", grant);
    assert(status().phase == XIAOTAI_BINDING_STORING && g_identity.saves == 1);
    assert_hidden();
    xiaotai_framework_reset();

    issued = start();
    lose_wifi(false);
    fake_guest_closed(XIAOTAI_ERR_PLATFORM_OFFLINE);
    fake_clock_advance_to(issued + 5000000);
    fake_drain_events();
    assert(status().phase == XIAOTAI_BINDING_EXPIRED && g_guest.opens == 1);
    recover_wifi();
    assert(status().phase == XIAOTAI_BINDING_REPORTING && g_request.count == 2);
    assert_hidden();
    xiaotai_framework_reset();

    (void)start();
    lose_wifi(true);
    assert(status().phase == XIAOTAI_BINDING_FAILED && !g_guest.open && g_guest.closes == 1);
    xiaotai_framework_reset();
    xiaotai_binding_reset();
    puts("offline/retired guests preserve only the live first-binding window; grant/expiry/user disconnect hide the code");
    return 0;
}
