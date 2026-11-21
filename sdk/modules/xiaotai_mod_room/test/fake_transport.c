// SPDX-License-Identifier: MIT
#include "fake_link.h"
#include "fake_platform.h"

#include <assert.h>
#include <string.h>
#include "xiaotai/framework.h"

fake_link_t fake_link;

void fake_step(char step)
{
    assert(fake_link.step_count + 1 < sizeof(fake_link.steps));
    fake_link.steps[fake_link.step_count++] = step;
}

xiaotai_transport_state_t xiaotai_transport_state(void)
{
    return fake_link.online ? XIAOTAI_TRANSPORT_ONLINE : XIAOTAI_TRANSPORT_DOWN;
}

void xiaotai_transport_status(xiaotai_transport_status_t *status)
{
    *status = (xiaotai_transport_status_t){.state = xiaotai_transport_state(),
                                           .connect_ready = fake_link.can_connect};
}

xiaotai_err_t xiaotai_link_connect(const xiaotai_link_request_t *request, const xiaotai_link_handlers_t *handlers,
                                  uint32_t *generation)
{
    assert(xiaotai_on_owner() && fake_link.online && fake_link.can_connect);
    ++fake_link.connects;
    fake_link.connecting_attempts_at_connect = fixture.connecting_attempts;
    fake_link.profile = *request;
    strcpy(fake_link.peer, request->peer);
    strcpy(fake_link.token, request->token);
    fake_link.profile.peer = fake_link.peer;
    fake_link.profile.token = fake_link.token;
    if (fake_link.connect_error != XIAOTAI_OK) return fake_link.connect_error;
    fake_link.handlers = *handlers;
    *generation = fake_link.generation = xiaotai_generation_next();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_command(uint32_t generation, uint32_t word, const void *data, size_t size)
{
    assert(generation == fake_link.generation && fake_link.opened && word == 0x2200U);
    assert(fake_link.commands_count < sizeof(fake_link.commands) / sizeof(fake_link.commands[0]) &&
           size < sizeof(fake_link.commands[0]));
    char *message = fake_link.commands[fake_link.commands_count++];
    memcpy(message, data, size);
    message[size] = '\0';
    fake_step(strstr(message, "join_room") ? 'J' : strstr(message, "set_mic_state") ? 'O' :
              strstr(message, "leave_room") ? 'L' : 'S');
    if (strstr(message, "set_mic_state") && fake_link.mic_command_error) return fake_link.mic_command_error;
    return fake_link.command_error;
}

xiaotai_err_t xiaotai_link_receive(uint32_t generation, xiaotai_link_media_kind_t kind,
                                  xiaotai_link_receive_fn_t receiver, void *ctx)
{
    assert(generation == fake_link.generation && kind == XIAOTAI_LINK_AUDIO && !receiver && !ctx);
    fake_step('R');
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_permit(uint32_t generation, xiaotai_link_media_kind_t kind, bool allowed)
{
    assert(generation == fake_link.generation && kind == XIAOTAI_LINK_AUDIO);
    if (allowed) assert(fake_link.opened);
    fake_link.permitted = allowed;
    fake_step(allowed ? 'P' : 'p');
    return XIAOTAI_OK;
}

void xiaotai_link_disconnect(uint32_t generation)
{
    assert(generation == fake_link.generation);
    ++fake_link.disconnects;
    fake_link.opened = false;
    fake_step('D');
}

void fake_open(void)
{
    assert(fake_link.generation && fake_link.handlers.opened);
    fake_link.opened = true;
    fake_link.handlers.opened(fake_link.handlers.ctx, fake_link.generation);
}

void fake_close(xiaotai_link_end_t why)
{
    assert(fake_link.generation && fake_link.handlers.closed);
    fake_link.opened = fake_link.permitted = false;
    fake_link.handlers.closed(fake_link.handlers.ctx, fake_link.generation, why, 0);
}

void fake_command(const char *data)
{
    fake_command_gen(fake_link.generation, data, false);
}

void fake_command_gen(uint32_t gen, const char *data, bool dropped)
{
    const xiaotai_link_command_t cmd = {.word = 0x2200U, .data = (const uint8_t *)data,
                                       .size = data ? strlen(data) : 0, .dropped = dropped};
    fake_link.handlers.command(fake_link.handlers.ctx, gen, &cmd);
}
