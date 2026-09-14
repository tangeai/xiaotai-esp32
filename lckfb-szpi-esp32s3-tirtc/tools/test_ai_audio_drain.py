#!/usr/bin/env python3
"""Exercise production EOS ownership, ingress race and nonblocking completion."""
from test_voice_reliability import ROOT, function, run
import re

media = "components/starter_media/src/starter_media.c"
runtime = "components/starter_runtime/src/starter_runtime.c"
code = next(line for line in (ROOT / "components/starter_media/include/starter_media.h").read_text().splitlines()
            if line.startswith("#define STARTER_MEDIA_DRAIN_TIMEOUT_MS")) + r'''
#include "starter_tirtc.h"
#include "starter_playout.h"
static atomic_bool s_active, s_speaker_muted;
static atomic_int s_mode;
static atomic_uint s_generation, s_drain_generation, s_drained_generation;
static atomic_uint s_room_ptt_generation;
static atomic_uint s_audio_rx_writers, s_audio_received, s_audio_dropped, s_audio_playback_blocked;
static struct { atomic_uint epoch, invalid, stale, overflow; } s_playout_diag;
#define AUDIO_RX_BYTES 1500U
#define AUDIO_RX_QUEUE_DEPTH 32U
typedef struct { uint64_t received_us; starter_tirtc_mode_t mode; uint32_t generation, epoch;
    starter_tirtc_frame_t frame; uint8_t payload[AUDIO_RX_BYTES]; } audio_rx_item_t;
static audio_rx_item_t pool[AUDIO_RX_QUEUE_DEPTH], *s_audio_rx_pool=pool;
static void *s_audio_rx_ready_queue=(void *)1, *s_audio_rx_free_queue=(void *)2;
static unsigned queued;
static bool close_during_copy;
static starter_playout_t q;
static uint64_t clock_us=1000000;
static int64_t esp_timer_get_time(void) {return clock_us;}
static unsigned uxQueueMessagesWaiting(void *queue) {assert(queue==(void *)1); return queued;}
static void release_audio_rx_slot(uint8_t slot, bool stale) {(void)slot;(void)stale;}
'''
code += re.search(r"EXT_RAM_BSS_ATTR static struct \{[^}]+\} s_room_audio_diag;",
                  (ROOT / media).read_text(), re.S)[0].replace("EXT_RAM_BSS_ATTR", "")
code += function(media, "update_max_counter")
code += function(media, "same_session")
code += function(media, "uplink_session_current")
code += function(media, "complete_audio_drain")
code += function(media, "starter_media_begin_audio_drain")
code += function(media, "starter_media_audio_drained")
code += r'''
static int xQueueReceive(void *queue, uint8_t *slot, int wait) {
    assert(queue==(void *)2 && wait==0); *slot=0;
    if(close_during_copy) {
        assert(starter_media_begin_audio_drain(7));
        complete_audio_drain(&q,0,clock_us);
        assert(!starter_media_audio_drained(7)); /* still in ingress */
    }
    return pdTRUE;
}
static int xQueueSend(void *queue, const uint8_t *slot, int wait) {
    assert(queue==(void *)1 && *slot==0 && wait==0); queued++; return pdTRUE;
}
'''
code += function(media, "submit_audio")
code += function(media, "starter_media_submit_audio")
code += r'''
static uint32_t s_connection_generation=7, s_ai_drain_started_ms;
static int64_t s_deadline_ms;
static unsigned finishes, disconnects;
static int error;
#define ESP_ERR_TIMEOUT 263
static int64_t now_ms(void) {return clock_us/1000;}
int starter_tirtc_disconnect(void) {disconnects++;return 0;}
static void finish_session(int err) {
    error=err; finishes++; s_ai_drain_started_ms=0;
    s_active=false; s_generation=0; s_drain_generation=0; s_drained_generation=0;
}
'''
code += function(runtime, "begin_ai_audio_drain")
code += function(runtime, "poll_ai_audio_drain")
code += r'''
int main(void) {
    s_active=true; s_mode=STARTER_TIRTC_CALL; s_generation=7;
    assert(!starter_media_begin_audio_drain(7)); /* never alter calls/H5 */
    s_mode=STARTER_TIRTC_AI;
    assert(uplink_session_current(STARTER_TIRTC_AI,7));
    assert(!starter_media_begin_audio_drain(0));
    assert(!starter_media_begin_audio_drain(6));
    starter_playout_init(&q);
    uint8_t packet[320]={0}; starter_tirtc_frame_t frame={.length=sizeof(packet)};
    close_during_copy=true;
    starter_media_submit_audio(STARTER_TIRTC_AI,7,&frame,packet);
    assert(queued==1 && s_audio_received==1 && s_audio_rx_writers==0);
    assert(same_session(STARTER_TIRTC_AI,7) && !uplink_session_current(STARTER_TIRTC_AI,7));
    complete_audio_drain(&q,0,clock_us); assert(!starter_media_audio_drained(7));
    starter_media_submit_audio(STARTER_TIRTC_AI,7,&frame,packet);
    assert(queued==1 && s_playout_diag.stale==1 && s_audio_rx_writers==0);
    begin_ai_audio_drain(); assert(disconnects==1 && finishes==0);
    clock_us+=50000;
    begin_ai_audio_drain(); assert(disconnects==1 && s_ai_drain_started_ms==1000);
    poll_ai_audio_drain(1050); assert(finishes==0);
    queued=0; q.count=1;
    complete_audio_drain(&q,0,clock_us); assert(!starter_media_audio_drained(7));
    q.count=0;
    complete_audio_drain(&q,160,clock_us); assert(!starter_media_audio_drained(7));
    starter_playout_output_written(&q,clock_us,160);
    complete_audio_drain(&q,0,clock_us+89999); assert(!starter_media_audio_drained(7));
    complete_audio_drain(&q,0,clock_us+90000); assert(starter_media_audio_drained(7));
    assert(!starter_media_audio_drained(8));
    poll_ai_audio_drain(1140); assert(finishes==1 && error==0);
    poll_ai_audio_drain(9000); assert(finishes==1);
    /* Explicit cancellation/new owner invalidates even a stale sink ACK. */
    s_drained_generation=7; s_generation=8; s_active=true;
    assert(!starter_media_audio_drained(7) && !starter_media_audio_drained(8));
    s_connection_generation=8; clock_us=10000000;
    begin_ai_audio_drain(); assert(disconnects==2);
    poll_ai_audio_drain(16999); assert(finishes==1);
    poll_ai_audio_drain(17000); assert(finishes==2 && error==ESP_ERR_TIMEOUT);
    begin_ai_audio_drain(); assert(finishes==3); /* no active PCM: immediate end */
    /* Unsigned deadline arithmetic remains valid across uptime rollover. */
    s_active=true; s_generation=8; clock_us=(uint64_t)(UINT32_MAX-100U)*1000U;
    begin_ai_audio_drain(); poll_ai_audio_drain(6898); assert(finishes==3);
    poll_ai_audio_drain(6899); assert(finishes==4 && error==ESP_ERR_TIMEOUT);
    s_active=true; s_generation=8; clock_us=0;
    begin_ai_audio_drain(); poll_ai_audio_drain(0); assert(finishes==4);
    poll_ai_audio_drain(6999); assert(finishes==5 && error==ESP_ERR_TIMEOUT);
    /* Room timing observes arrival, including discontinuous/constant PTS.
     * It must not affect AI counters, queue admission, payload or ordering. */
    assert(s_room_audio_diag.rx_frames==0);
    s_active=true;s_mode=STARTER_TIRTC_ROOM;s_generation=10;s_drain_generation=0;
    close_during_copy=false;queued=0;clock_us=1000000;
    frame.length=160;frame.timestamp_ms=1000;
    starter_media_submit_audio(STARTER_TIRTC_ROOM,10,&frame,packet);
    clock_us+=100000;frame.timestamp_ms+=20;
    starter_media_submit_audio(STARTER_TIRTC_ROOM,10,&frame,packet);
    clock_us+=500;frame.timestamp_ms+=20;
    starter_media_submit_audio(STARTER_TIRTC_ROOM,10,&frame,packet);
    assert(queued==3&&s_room_audio_diag.rx_frames==3&&s_room_audio_diag.rx_bytes==480);
    assert(s_room_audio_diag.rx_pairs==2&&s_room_audio_diag.rx_gaps==1&&s_room_audio_diag.rx_burst==1);
    assert(s_room_audio_diag.rx_gap_max_us==100000&&s_room_audio_diag.rx_forward_gap==0);
    clock_us+=20000; /* Repeated PTS is reported, never sorted or dropped. */
    starter_media_submit_audio(STARTER_TIRTC_ROOM,10,&frame,packet);
    clock_us+=20000;frame.timestamp_ms+=60;
    starter_media_submit_audio(STARTER_TIRTC_ROOM,10,&frame,packet);
    clock_us+=20000;frame.timestamp_ms-=40;
    starter_media_submit_audio(STARTER_TIRTC_ROOM,10,&frame,packet);
    assert(queued==6&&s_room_audio_diag.rx_repeat_ts==1&&s_room_audio_diag.rx_forward_gap==1&&s_room_audio_diag.rx_back_ts==1);
    assert(pool[0].frame.timestamp_ms==frame.timestamp_ms&&!memcmp(pool[0].payload,packet,160));
    unsigned pairs=s_room_audio_diag.rx_pairs;
    s_generation=11;clock_us+=1000000;
    starter_media_submit_audio(STARTER_TIRTC_ROOM,11,&frame,packet);
    assert(s_room_audio_diag.rx_pairs==pairs); /* Do not measure across calls. */
    s_room_audio_diag.rx_observer_busy=1;clock_us+=20000;
    starter_media_submit_audio(STARTER_TIRTC_ROOM,11,&frame,packet);
    assert(queued==8&&s_room_audio_diag.rx_observer_skipped==1);
    assert(s_room_audio_diag.rx_pairs==pairs); /* Never block media on diagnostics. */
    s_room_audio_diag.rx_observer_busy=0;
    clock_us=s_room_audio_diag.rx_last_us-100;
    starter_media_submit_audio(STARTER_TIRTC_ROOM,11,&frame,packet);
    assert(queued==9&&s_room_audio_diag.rx_observer_skipped==2);
    assert(s_room_audio_diag.rx_pairs==pairs&&s_room_audio_diag.rx_gap_max_us==100000);
    assert(s_audio_rx_writers==0&&s_room_audio_diag.rx_observer_busy==0);
    return 0;
}
'''
run("AI EOS: ingress race, queued/decoded/DMA tail, stale ACK, cancel, timeout", code,
    ["components/starter_media/src/starter_playout.c"])

source = (ROOT / runtime).read_text()
assert 'begin_ai_audio_drain();' in function(runtime, "handle_ai_command")
assert 'poll_ai_audio_drain((uint32_t)current_ms);' in source
assert 's_ai_drain_started_ms = 0;' in function(runtime, "finish_session")
assert 'finish_session(0);' in function(runtime, "end_ai_session")
assert 'complete_audio_drain(' in function(media, "audio_sink_task")
assert 'atomic_store(&s_drain_generation, 0);' in function(media, "stop_media")
