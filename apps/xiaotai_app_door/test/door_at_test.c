// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_native_preferences.py
// Replaces legacy: tests/test_at_diagnostics.py
#include "fakes.h"

#include "at_test_help.h"
#include "xiaotai/apps/door.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"
#include "xiaotai_console_host.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static xiaotai_signal_t done;
static char output[16384];
static uint32_t activity_id, maintenance_ticket;

static const char *command(const char *line)
{
    if (line) assert(xiaotai_console_host_feed(line, strlen(line)) == strlen(line));
    assert(xiaotai_at_service(20) == XIAOTAI_OK);
    const size_t size = xiaotai_console_host_take(output, sizeof(output) - 1);
    output[size] = '\0';
    return output;
}

typedef struct {
    unsigned control, accepted, peer_selected, save_pending, awake, setup, lock_configured;
    char page[32], role[16];
} status_t;

static status_t door_status(const char *line, unsigned control, unsigned accepted)
{
    const char *reply = command(line);
    status_t result = {0};
    int end = 0;
    const int parsed = sscanf(reply, "+DOOR: control=%u accepted=%u page=%31s role=%15s "
        "peer_selected=%u save_pending=%u awake=%u setup=%u lock_configured=%u%n",
        &result.control, &result.accepted, result.page, result.role, &result.peer_selected,
        &result.save_pending, &result.awake, &result.setup, &result.lock_configured, &end);
    if (parsed != 9 || result.control != control || result.accepted != accepted || strcmp(reply + end, "\r\nOK\r\n"))
        fprintf(stderr, "%s -> %s; expected control=%u accepted=%u and all status fields\n", line, reply, control, accepted);
    assert(parsed == 9 && !strcmp(reply + end, "\r\nOK\r\n"));
    assert(result.control == control && result.accepted == accepted);
    return result;
}

static void error(const char *line)
{
    const char *reply = command(line);
    assert(!strncmp(reply, "ERROR: ", 7) && !strstr(reply, "+DOOR:"));
}

static void mounted(const xiaotai_view_t *view, void *ctx)
{
    (void)ctx;
    if (view->layer == XIAOTAI_LAYER_PAGE) {
        snprintf(fake_ui.page, sizeof(fake_ui.page), "%s", view->id ? view->id : "");
        fake_ui.page_kind = view->kind;
    } else if (view->layer == XIAOTAI_LAYER_OVERLAY) {
        snprintf(fake_ui.overlay, sizeof(fake_ui.overlay), "%s",
                 view->id && !(view->flags & XIAOTAI_VIEW_DISMISS) ? view->id : "");
    }
}

static xiaotai_err_t setup(void)
{
    xiaotai_view_set_sink(mounted, NULL);
    assert(xiaotai_engine_at_init() == XIAOTAI_OK);
    return fake_store_init();
}

static void idle_display(bool awake, bool pending)
{
    fake_ui.awake = awake;
    fake_save_pending = pending;
}

static void change(void *ctx)
{
    switch (*(const char *)ctx) {
    case 'a': {
        static const xiaotai_activity_t activity = {.name = "fixture", .resources = XIAOTAI_RESOURCE_LINK};
        assert(xiaotai_activity_begin(&activity, &activity_id) == XIAOTAI_OK);
        break;
    }
    case 'i': xiaotai_activity_end(activity_id); break;
    case 'c': fake_call.session = 32; break;
    case 'e': fake_call.session = 0; break;
    case 'o': strcpy(fake_ui.overlay, "fixture.overlay"); break;
    case 'x': fake_ui.overlay[0] = '\0'; break;
    case 'h': fake_net.prepare.stage = XIAOTAI_NET_PREPARE_CHECKING; break;
    case 'j': fake_net.prepare.stage = XIAOTAI_NET_PREPARE_READY; break;
    case 'q': assert(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &maintenance_ticket) == XIAOTAI_OK); break;
    case 'r': xiaotai_quiesce_end(maintenance_ticket); break;
    case 'p':
        fake_provisioning.phase = XIAOTAI_PROVISIONING_WAITING;
        assert(xiaotai_event_emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, &(xiaotai_args_t){.value = XIAOTAI_PROVISIONING_WAITING},
                                 &fake_provisioning, sizeof(fake_provisioning)) == XIAOTAI_OK);
        break;
    case 'n':
        fake_provisioning.phase = XIAOTAI_PROVISIONING_NONE;
        assert(xiaotai_event_emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, &(xiaotai_args_t){0},
                                 &fake_provisioning, sizeof(fake_provisioning)) == XIAOTAI_OK);
        break;
    case 's':
        assert(xiaotai_setting_set_text(XIAOTAI_SETTING_APP_DOOR_PEER, "fixture-private-station") == XIAOTAI_OK);
        idle_display(false, true);
        break;
    default: assert(false);
    }
}

static void completed(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&done);
}

static void sync(void)
{
    assert(xiaotai_run(completed, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&done, 1000));
}

static void signal_change(char action)
{
    assert(xiaotai_run(change, &action) == XIAOTAI_OK);
    sync();
    sync();
}

static void intent(const char *name, int32_t value)
{
    assert(xiaotai_intent_post(name, &(xiaotai_args_t){.value = value}) == XIAOTAI_OK);
    sync();
}

static void show_other_home(bool clock)
{
    const xiaotai_view_t view = {.kind = clock ? XIAOTAI_VIEW_CLOCK : XIAOTAI_VIEW_HOME,
        .id = clock ? "fixture.clock" : "fixture.face"};
    xiaotai_view_show(&view);
}

static void other_foreground(bool active)
{
    if (active) show_other_home(xiaotai_app_entry() == XIAOTAI_APP_ENTRY_PREVIOUS);
}

static xiaotai_err_t other_step(int direction)
{
    if (direction == 1 && fake_ui.page_kind == XIAOTAI_VIEW_HOME) show_other_home(true);
    else if (direction == -1 && fake_ui.page_kind == XIAOTAI_VIEW_CLOCK) show_other_home(false);
    else return direction == 1 ? xiaotai_app_next() : xiaotai_app_previous();
    return XIAOTAI_OK;
}

static void step(const char *line, unsigned control, const char *page)
{
    const status_t state = door_status(line, control, 1);
    assert(!strcmp(state.page, page));
}

int main(int argc, char **argv)
{
    xiaotai_console_host_pipe();
    const xiaotai_at_config_t at = {.task_stack_bytes = XIAOTAI_AT_NO_TASK, .reply_buffer_bytes = 4096};
    assert(xiaotai_at_configure(&at) == XIAOTAI_OK);
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_at_open() == XIAOTAI_OK);
    (void)command(NULL);
    assert(xiaotai_signal_init(&done) == XIAOTAI_OK);
    fake_saved_sender = true;
    static const xiaotai_module_t fixture = {.name = "at", .init = setup, .start = xiaotai_engine_at_start};
    static const xiaotai_application_t door = {
        .name = "door", .init = xiaotai_app_door_init, .start = xiaotai_app_door_start,
        .foreground = xiaotai_app_door_foreground, .page_step = xiaotai_app_door_page_step,
    };
    static const xiaotai_application_t other = {.name = "fixture", .foreground = other_foreground, .page_step = other_step};
    const xiaotai_module_t *const modules[] = {&fixture};
    const xiaotai_application_t *const apps[] = {&door, &other};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1, .applications = apps, .application_count = 2};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    sync();
    if (argc == 2 && !strcmp(argv[1], "--help-records")) {
        assert(at_test_help_write(stdout, command("AT+HELP\r\n")));
        goto out;
    }
    status_t status = door_status("AT+DOOR\r\n", 1, 1);
    assert(!strcmp(status.role, "sender") && !strcmp(status.page, "door") && status.awake &&
           !status.peer_selected && !status.lock_configured);
    assert(!fake_role_saves);
    status = door_status("AT+DOOR VIEWER\r\n", 5, 1);
    assert(!strcmp(status.role, "viewer") && !fake_saved_sender && fake_role_saves == 1);
    const char *denied = command("AT+INTENT door.role 1\r\n");
    assert(!strcmp(denied, "ERROR: NOT_ALLOWED\r\n") && !fake_saved_sender && fake_role_saves == 1);
    intent(XIAOTAI_INTENT_APP_DOOR_ROLE, -1);
    assert(!fake_saved_sender && fake_role_saves == 1);
    signal_change('q');
    (void)door_status("AT+DOOR SENDER\r\n", 6, 0);
    (void)door_status("AT+DOOR VIEWER\r\n", 5, 0);
    intent(XIAOTAI_INTENT_APP_DOOR_ROLE, 1);
    status = door_status("AT+DOOR\r\n", 1, 1);
    assert(!strcmp(status.page, "door") && !strcmp(status.role, "viewer") && !status.save_pending);
    const unsigned refreshes = fake_refreshes;
    step("AT+DOOR DEVICES\r\n", 7, "devices");
    assert(!strcmp(fake_ui.page, "door.devices") && fake_refreshes == refreshes + 1);
    step("AT+DOOR BACK\r\n", 8, "door");
    assert(!strcmp(fake_ui.page, "door.home"));
    assert(!fake_saved_sender && fake_role_saves == 1);
    assert(!fake_saved_peer[0] && !fake_mic_saves && !fake_volume_saves);
    signal_change('r');
    status = door_status("AT+DOOR SENDER\r\n", 6, 1);
    assert(!strcmp(status.role, "sender") && fake_saved_sender && fake_role_saves == 2);

    assert(!strcmp(fake_ui.page, "door.home"));
    (void)door_status("AT+DOOR BACK\r\n", 8, 0);
    status = door_status("AT+DOOR DEVICES\r\n", 7, 1);
    assert(!strcmp(status.page, "devices") && fake_refreshes == refreshes + 2);
    (void)door_status("AT+DOOR DEVICES\r\n", 7, 0);
    (void)door_status("AT+DOOR PREVIOUS\r\n", 4, 0);
    (void)door_status("AT+DOOR NEXT\r\n", 3, 0);
    (void)door_status("AT+DOOR VIEWER\r\n", 5, 0);
    status = door_status("AT+DOOR BACK\r\n", 8, 1);
    assert(!strcmp(status.page, "door"));
    intent(XIAOTAI_INTENT_APP_DOOR_MONITOR, 0);
    assert(!strcmp(fake_ui.page, "door.monitor"));
    status = door_status("AT+DOOR HOME\r\n", 2, 1);
    assert(!strcmp(status.page, "door"));
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN, 0);
    assert(fake_ui.overlay[0]);
    (void)door_status("AT+DOOR\r\n", 1, 1);
    (void)door_status("AT+DOOR HOME\r\n", 2, 0);
    (void)door_status("AT+DOOR BACK\r\n", 8, 0);
    assert(fake_ui.overlay[0]);
    intent(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, 0);
    assert(!fake_ui.overlay[0]);
    signal_change('p');
    status = door_status("AT+DOOR\r\n", 1, 1);
    assert(status.setup);
    (void)door_status("AT+DOOR DEVICES\r\n", 7, 0);
    signal_change('n');
    assert(!door_status("AT+DOOR\r\n", 1, 1).setup);
    signal_change('a');
    (void)door_status("AT+DOOR\r\n", 1, 1);
    (void)door_status("AT+DOOR HOME\r\n", 2, 0);
    (void)door_status("AT+DOOR VIEWER\r\n", 5, 0);
    signal_change('i');
    signal_change('c');
    (void)door_status("AT+DOOR HOME\r\n", 2, 0);
    signal_change('e');
    signal_change('o');
    (void)door_status("AT+DOOR HOME\r\n", 2, 0);
    signal_change('x');
    signal_change('h');
    assert(door_status("AT+DOOR\r\n", 1, 1).setup);
    (void)door_status("AT+DOOR HOME\r\n", 2, 0);
    signal_change('j');

    step("AT+DOOR NEXT\r\n", 3, "face");
    (void)door_status("AT+DOOR DEVICES\r\n", 7, 0);
    step("AT+DOOR NEXT\r\n", 3, "clock");
    step("AT+DOOR NEXT\r\n", 3, "door");
    step("AT+DOOR PREVIOUS\r\n", 4, "clock");
    step("AT+DOOR PREVIOUS\r\n", 4, "face");
    step("AT+DOOR PREVIOUS\r\n", 4, "door");
    step("AT+DOOR NEXT\r\n", 3, "face");
    step("AT+DOOR HOME\r\n", 2, "door");

    signal_change('s');
    status = door_status("AT+DOOR\r\n", 1, 1);
    assert(status.peer_selected && status.save_pending && !status.awake);
    const char *diag = command("AT+STATUS app.door\r\n");
    assert(strstr(diag, "+APP-DOOR:") && strstr(diag, "OK\r\n") && !strstr(diag, "fixture-private-station"));
    assert(!strstr(command("AT+DOOR\r\n"), "fixture-private-station"));
    error("AT+DOOR HOME extra\r\n");
    error("AT+DOOR ACCEPT\r\n");
    error("AT+DOOR DIAL\r\n");
    error("AT+DOOR UNLOCK\r\n");
    assert(!fake_calls && !fake_lock_calls && fake_saved_sender && fake_role_saves == 2);
out:
    xiaotai_framework_reset();
    xiaotai_app_door_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&done);
    return 0;
}
