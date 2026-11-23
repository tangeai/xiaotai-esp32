// SPDX-License-Identifier: MIT
#include "fake_link.h"
#include <assert.h>
#include "xiaotai/framework.h"

const xiaotai_media_sink_t *xiaotai_link_audio_sink(void)
{
    static const xiaotai_media_sink_t sink = {0};
    return &sink;
}

xiaotai_err_t xiaotai_media_open(const xiaotai_media_session_t *session)
{
    assert(!fake_link.media_open && session->generation == fake_link.generation);
    fake_link.session = *session;
    fake_step('M');
    if (fake_link.media_error) return fake_link.media_error;
    fake_link.media_open = true;
    return XIAOTAI_OK;
}

void xiaotai_media_close(uint32_t generation)
{
    assert(fake_link.media_open && generation == fake_link.generation);
    fake_link.media_open = false;
    fake_step('X');
}

xiaotai_err_t xiaotai_media_uplink_gate(uint32_t generation, bool open, uint32_t tail_ms)
{
    assert(fake_link.media_open && generation == fake_link.generation);
    if (open) {
        assert(tail_ms == 0U && !fake_link.mic_muted);
        ++fake_link.gate_open_calls;
        if (fake_link.gate_error) return fake_link.gate_error;
    } else {
        ++fake_link.gate_close_calls;
        fake_link.gate_tail_ms = tail_ms;
    }
    fake_link.gate_open = open;
    fake_link.gate_on_owner = xiaotai_on_owner();
    fake_step(open ? 'g' : 'G');
    return XIAOTAI_OK;
}

void xiaotai_media_status(xiaotai_media_status_t *status)
{
    *status = (xiaotai_media_status_t){.mic_muted = fake_link.mic_muted};
}
