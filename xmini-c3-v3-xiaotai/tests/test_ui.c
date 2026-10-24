// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "xmini_canvas.h"
#include "xmini_key.h"
#include "xmini_ui.h"

static void test_gestures(void)
{
    xmini_key_t key = {0};
    assert(xmini_key_poll(&key, false, 0, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, true, 10, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, true, 40, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 80, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 110, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 459, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 460, 1) == XMINI_KEY_NEXT);

    assert(xmini_key_poll(&key, true, 500, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, true, 530, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 570, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 600, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, true, 700, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, true, 730, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 770, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 800, 1) == XMINI_KEY_CONFIRM);
    assert(xmini_key_poll(&key, false, 1200, 1) == XMINI_KEY_NONE);

    assert(xmini_key_poll(&key, true, 1300, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, true, 1330, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, true, 2530, 1) == XMINI_KEY_BACK);
    assert(xmini_key_poll(&key, true, 2600, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 2700, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 2730, 1) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 3200, 1) == XMINI_KEY_NONE);
}

static void test_context_and_wrap(void)
{
    xmini_key_t key = {.epoch = 1};
    xmini_key_poll(&key, true, UINT32_MAX - 100, 1);
    xmini_key_poll(&key, true, UINT32_MAX - 70, 1);
    xmini_key_poll(&key, false, UINT32_MAX - 40, 1);
    xmini_key_poll(&key, false, UINT32_MAX - 10, 1);
    assert(xmini_key_poll(&key, false, 340, 1) == XMINI_KEY_NEXT);

    key = (xmini_key_t){.epoch = 1};
    xmini_key_poll(&key, true, 0, 1);
    xmini_key_poll(&key, true, 30, 1);
    // An incoming page or another owner must invalidate a held gesture.
    assert(xmini_key_poll(&key, true, 50, 2) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, true, 1300, 2) == XMINI_KEY_NONE);
    xmini_key_poll(&key, false, 1400, 2);
    assert(xmini_key_poll(&key, false, 1430, 2) == XMINI_KEY_NONE);
    assert(xmini_key_poll(&key, false, 1800, 2) == XMINI_KEY_NONE);
    // Bounce never becomes a click.
    xmini_key_poll(&key, true, 2000, 2);
    xmini_key_poll(&key, false, 2010, 2);
    assert(xmini_key_poll(&key, false, 2500, 2) == XMINI_KEY_NONE);
}

static void test_same_phase_session_identity(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.page = XMINI_PAGE_SESSION;
    ui.session_phase = XMINI_UI_RINGING;
    xmini_ui_session_identity(&ui, 2, 100, "caller-a");
    unsigned epoch = ui.epoch;
    xmini_key_t key = {.epoch = epoch};
    xmini_key_poll(&key, false, 0, ui.epoch);
    xmini_key_poll(&key, true, 10, ui.epoch);
    xmini_key_poll(&key, true, 40, ui.epoch);
    xmini_key_poll(&key, false, 80, ui.epoch);
    xmini_key_poll(&key, false, 110, ui.epoch);
    xmini_ui_session_identity(&ui, 2, 101, "caller-a");
    assert(ui.epoch != epoch);
    assert(xmini_key_poll(&key, false, 150, ui.epoch) == XMINI_KEY_NONE);
    xmini_key_poll(&key, true, 200, ui.epoch);
    xmini_key_poll(&key, true, 230, ui.epoch);
    xmini_key_poll(&key, false, 270, ui.epoch);
    assert(xmini_key_poll(&key, false, 300, ui.epoch) != XMINI_KEY_CONFIRM);
    epoch = ui.epoch;
    xmini_ui_session_identity(&ui, 2, 101, "caller-a");
    assert(ui.epoch == epoch);
    xmini_ui_session_identity(&ui, 3, 101, "caller-a");
    assert(ui.epoch != epoch);
    epoch = ui.epoch;
    xmini_ui_session_identity(&ui, 3, 101, "caller-b");
    assert(ui.epoch != epoch);
}

static void test_menu_and_audio_intents(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_AI);
    xmini_ui_key(&ui, XMINI_KEY_BACK);
    for (unsigned i = 0; i < 3; ++i) xmini_ui_key(&ui, XMINI_KEY_NEXT);
    xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(ui.page == XMINI_PAGE_SETTINGS);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    ui.audio_ready = true;
    ui.volume = 100;
    xmini_intent_t intent = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(intent.kind == XMINI_INTENT_VOLUME && intent.value == 100);
    for (unsigned i = 0; i < 4; ++i) xmini_ui_key(&ui, XMINI_KEY_NEXT);
    xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(ui.page == XMINI_PAGE_AUDIO);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_TONE);
    ui.audio_busy = true;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_STOP);
    assert(ui.page == XMINI_PAGE_SETTINGS);
    ui.page = XMINI_PAGE_AUDIO;
    ui.selected = 2;
    ui.audio_busy = false;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    ui.recording_ready = true;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_PLAY);
}

static void test_framebuffer(void)
{
    struct {
        uint8_t before[16];
        uint8_t frame[1024];
        uint8_t after[16];
    } memory;
    memset(&memory, 0xa5, sizeof(memory));
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.contact_count = ui.pending_count = 3;
    ui.session_kind = 1;
    ui.session_phase = XMINI_UI_ACTIVE;
    ui.pending_present = true;
    memset(ui.contact_name, 'C', sizeof(ui.contact_name) - 1);
    memset(ui.pending_peer, 'P', sizeof(ui.pending_peer) - 1);
    memset(ui.pending_name, 'N', sizeof(ui.pending_name) - 1);
    memset(ui.caption, 'X', sizeof(ui.caption) - 1);
    ui.contact_pages = ui.pending_pages = 3;
    ui.contact_page = ui.pending_page = 1;
    for (int p = XMINI_PAGE_HOME; p <= XMINI_PAGE_GROUP_PASSWORD; ++p) {
        if (p == XMINI_PAGE_GUIDE) continue;
        ui.page = (xmini_page_t)p;
        unsigned count = p == XMINI_PAGE_HOME ? 4 : p == XMINI_PAGE_SETTINGS ? 7 :
                         p == XMINI_PAGE_AUDIO ? 4 : p == XMINI_PAGE_DEVICE || p == XMINI_PAGE_GROUP ? 9 :
                         p == XMINI_PAGE_WECHAT || p == XMINI_PAGE_PENDING ? 6 :
                         p == XMINI_PAGE_CONTACT_DECISION || p == XMINI_PAGE_WIFI_CONFIRM ? 2 :
                         p == XMINI_PAGE_SESSION ? 3 : 1;
        for (ui.selected = 0; ui.selected < count; ++ui.selected) {
            xmini_ui_render(&ui, memory.frame);
            bool nonblank = false;
            for (unsigned i = 0; i < 1024; ++i) nonblank |= memory.frame[i] != 0;
            assert(nonblank);
            for (unsigned i = 0; i < 16; ++i)
                assert(memory.before[i] == 0xa5 && memory.after[i] == 0xa5);
            for (unsigned y = 14; y < 22; ++y)
                for (unsigned x = 0; x < 128; ++x)
                    assert(!(memory.frame[(y / 8) * 128 + x] & (1U << (y % 8))));
        }
    }
    xmini_canvas_text(memory.frame, -5, -3, "XIAOTAI", 3);
    xmini_canvas_text(memory.frame, 120, 62, "TEST", 3);
    for (unsigned i = 0; i < 16; ++i) {
        assert(memory.before[i] == 0xa5 && memory.after[i] == 0xa5);
    }
}

static void test_session_body_rows(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.page = XMINI_PAGE_SESSION;
    strcpy(ui.peer, "TIR_CONTACT_LONG_NAME");
    strcpy(ui.caption, "This is a long AI caption for horizontal scrolling");
    for (unsigned kind = 1; kind <= 5; ++kind) {
        ui.session_kind = kind;
        for (unsigned phase = XMINI_UI_CONNECTING; phase <= XMINI_UI_ERROR; ++phase) {
            ui.session_phase = (xmini_ui_session_phase_t)phase;
            for (unsigned selection = 0; selection < 3; ++selection) {
                ui.selected = selection;
                for (unsigned tick = 0; tick < 40; tick += 7) {
                    ui.scroll_tick = tick;
                    uint8_t frame[1024];
                    xmini_ui_render(&ui, frame);
                    for (unsigned y = 14; y < 22; ++y)
                        for (unsigned x = 0; x < 128; ++x)
                            assert(!(frame[(y / 8) * 128 + x] & (1U << (y % 8))));
                    for (unsigned row = 0; row < 3; ++row) {
                        bool nonblank = false;
                        for (unsigned y = 22 + row * 14; y < 34 + row * 14; ++y)
                            for (unsigned x = 0; x < 128; ++x)
                                nonblank |= (frame[(y / 8) * 128 + x] & (1U << (y % 8))) != 0;
                        assert(nonblank);
                    }
                    for (unsigned x = 0; x < 128; ++x)
                        assert(!(frame[7 * 128 + x] & 0xc0));
                }
            }
        }
    }
}

static void test_menu_traversal(void)
{
    static const xmini_page_t targets[] = {
        XMINI_PAGE_AI, XMINI_PAGE_WECHAT, XMINI_PAGE_DEVICE, XMINI_PAGE_SETTINGS,
    };
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    for (unsigned selected = 0; selected < 4; ++selected) {
        for (unsigned i = 0; i < selected; ++i)
            assert(xmini_ui_key(&ui, XMINI_KEY_NEXT).kind == XMINI_INTENT_NONE);
        assert(ui.selected == selected);
        xmini_intent_t entered = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
        assert(entered.kind == (selected == 1 || selected == 2 ? XMINI_UI_CONTACT_PAGE : XMINI_INTENT_NONE));
        assert(entered.value == 0);
        assert(ui.page == targets[selected] && ui.selected == 0);
        assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE);
        assert(ui.page == XMINI_PAGE_HOME && ui.selected == 0);
    }
    static const struct { xmini_page_t page; unsigned count; } menus[] = {
        {XMINI_PAGE_HOME, 4}, {XMINI_PAGE_SETTINGS, 7}, {XMINI_PAGE_AUDIO, 4},
        {XMINI_PAGE_GROUP, 9}, {XMINI_PAGE_CONTACT_DECISION, 2}, {XMINI_PAGE_WIFI_CONFIRM, 2},
    };
    for (unsigned m = 0; m < sizeof(menus) / sizeof(menus[0]); ++m) {
        ui.page = menus[m].page;
        ui.selected = 0;
        for (unsigned i = 1; i <= menus[m].count; ++i) {
            assert(xmini_ui_key(&ui, XMINI_KEY_NEXT).kind == XMINI_INTENT_NONE);
            assert(ui.selected == i % menus[m].count);
        }
    }
    for (unsigned count = 0; count <= 3; ++count) {
        ui.contact_count = ui.pending_count = count;
        static const xmini_page_t pages[] = {XMINI_PAGE_WECHAT, XMINI_PAGE_DEVICE, XMINI_PAGE_PENDING};
        for (unsigned p = 0; p < sizeof(pages) / sizeof(pages[0]); ++p) {
            unsigned entries = count + (pages[p] == XMINI_PAGE_DEVICE ? 6 : 3);
            ui.page = pages[p];
            ui.selected = 0;
            for (unsigned i = 1; i <= entries; ++i) {
                assert(xmini_ui_key(&ui, XMINI_KEY_NEXT).kind == XMINI_INTENT_NONE);
                assert(ui.selected == i % entries);
            }
        }
    }
    ui.page = XMINI_PAGE_DEVICE;
    ui.selected = ui.contact_count + 3;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_CONTACT_REFRESH);
    assert(ui.page == XMINI_PAGE_PENDING && ui.selected == 0);
}

static void test_pending_contact_decisions(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.page = XMINI_PAGE_PENDING;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_PENDING_PAGE);
    assert(ui.page == XMINI_PAGE_PENDING);
    ui.pending_count = 2;
    strcpy(ui.pending_name, "TIR_PENDING_A");
    uint32_t epoch = ui.epoch;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_CONTACT_DECISION && ui.selected == 0);
    assert(ui.epoch != epoch && ui.pending_present && !strcmp(ui.pending_peer, "TIR_PENDING_A"));
    /* The selection is a fixed peer, not an index into a moving list. */
    strcpy(ui.pending_name, "TIR_PENDING_B");
    ui.pending_count = 1;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_CONTACT_ACCEPT);
    assert(!strcmp(ui.pending_peer, "TIR_PENDING_A"));
    assert(xmini_ui_key(&ui, XMINI_KEY_NEXT).kind == XMINI_INTENT_NONE);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_CONTACT_REJECT);
    assert(!strcmp(ui.pending_peer, "TIR_PENDING_A"));
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_PENDING && ui.selected == 0);
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_HOME);
    ui.page = XMINI_PAGE_PENDING;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(!strcmp(ui.pending_peer, "TIR_PENDING_B"));
    ui.pending_present = false;
    ui.pending_count = 0;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(xmini_ui_key(&ui, XMINI_KEY_NEXT).kind == XMINI_INTENT_NONE);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_CONTACT_DECISION && !strcmp(ui.pending_peer, "TIR_PENDING_B"));
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_PENDING);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_PENDING_PAGE);
    ui.pending_count = 1;
    ui.pending_name[0] = '\0';
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_PENDING);
}

static void test_wifi_confirmation(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.page = XMINI_PAGE_SETTINGS;
    ui.selected = 5;
    uint32_t epoch = ui.epoch;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_WIFI_CONFIRM && ui.selected == 0 && ui.epoch != epoch);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_SETTINGS && ui.selected == 0);
    ui.selected = 5;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(xmini_ui_key(&ui, XMINI_KEY_NEXT).kind == XMINI_INTENT_NONE);
    assert(ui.selected == 1);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_WIFI_SETUP);
    assert(ui.page == XMINI_PAGE_WIFI_CONFIRM);
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_SETTINGS && ui.selected == 0);
    ui.selected = 5;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.selected == 0);
    assert(xmini_ui_key(&ui, XMINI_KEY_NEXT).kind == XMINI_INTENT_NONE);
    assert(xmini_ui_key(&ui, XMINI_KEY_NEXT).kind == XMINI_INTENT_NONE);
    assert(ui.selected == 0);
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_SETTINGS);
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_HOME);
    ui.page = XMINI_PAGE_WIFI_CONFIRM;
    ui.wifi_setup_pending = true;
    ui.selected = 1;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_UI_WIFI_CANCEL);
    assert(ui.page == XMINI_PAGE_WIFI_CONFIRM);
    ui.selected = 0; ui.wifi_setup_error = -1;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_WIFI_CANCEL);
    assert(ui.page == XMINI_PAGE_WIFI_CONFIRM);
}

static void test_business_intents(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.page = XMINI_PAGE_AI;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_AI_START);
    ui.page = XMINI_PAGE_SESSION;
    ui.session_kind = 3;
    ui.session_generation = 77;
    ui.session_phase = XMINI_UI_RINGING;
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_UI_REJECT);
    assert(ui.page == XMINI_PAGE_SESSION && ui.session_phase == XMINI_UI_RINGING);
    ui.page = XMINI_PAGE_SESSION;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_ACCEPT);
    xmini_ui_key(&ui, XMINI_KEY_NEXT);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_REJECT);
    ui.session_kind = 1;
    ui.session_phase = XMINI_UI_ACTIVE;
    ui.selected = 0;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_INTERRUPT);
    xmini_ui_key(&ui, XMINI_KEY_NEXT);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_SUBMIT);
    xmini_ui_key(&ui, XMINI_KEY_NEXT);
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_HANGUP);
    ui.page = XMINI_PAGE_GROUP;
    ui.selected = 2;
    xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(ui.page == XMINI_PAGE_GROUP_JOIN);
    for (unsigned i = 0; i < 6; ++i) {
        for (unsigned j = 0; j < i; ++j) xmini_ui_key(&ui, XMINI_KEY_NEXT);
        xmini_intent_t action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
        assert(action.kind == (i == 5 ? XMINI_UI_GROUP_JOIN : XMINI_INTENT_NONE));
    }
    assert(!strcmp(ui.join_code, "012345") && ui.page == XMINI_PAGE_GROUP);
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_UI_GROUP_DISCONNECT);
    ui.page = XMINI_PAGE_AI;
    xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(ui.page == XMINI_PAGE_SESSION);
}

static void test_session_back_ends_owner(void)
{
    static const xmini_page_t owner_pages[] = {
        XMINI_PAGE_AI, XMINI_PAGE_DEVICE, XMINI_PAGE_WECHAT, XMINI_PAGE_LIVE,
    };
    for (unsigned owner = 1; owner <= 4; ++owner) {
        for (unsigned phase = XMINI_UI_CONNECTING; phase <= XMINI_UI_ERROR; ++phase) {
            for (unsigned landing = 0; landing < 2; ++landing) {
                xmini_ui_t ui;
                xmini_ui_init(&ui);
                ui.page = landing ? owner_pages[owner - 1] : XMINI_PAGE_SESSION;
                ui.session_kind = owner;
                ui.session_generation = 77;
                ui.session_phase = (xmini_ui_session_phase_t)phase;
                xmini_intent_kind_t expected = phase == XMINI_UI_RINGING ? XMINI_UI_REJECT :
                    phase == XMINI_UI_ENDING ? XMINI_INTENT_NONE : XMINI_UI_HANGUP;
                assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == expected);
                assert(ui.page == XMINI_PAGE_SESSION);
                assert(ui.session_kind == owner && ui.session_generation == 77);
                assert(ui.session_phase == (xmini_ui_session_phase_t)phase);
            }
        }
    }
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.page = XMINI_PAGE_SESSION;
    ui.session_phase = XMINI_UI_ERROR;
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_UI_DISMISS);
    assert(ui.page == XMINI_PAGE_SESSION && ui.session_phase == XMINI_UI_ERROR);
    ui.session_kind = 2;
    ui.session_generation = 99;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_HANGUP);
    assert(ui.session_generation == 99 && ui.session_phase == XMINI_UI_ERROR);
    ui.session_kind = ui.session_generation = 0;
    ui.session_phase = XMINI_UI_IDLE;
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_HOME);
    ui.page = XMINI_PAGE_AI;
    ui.session_kind = 3;
    ui.session_generation = 88;
    ui.session_phase = XMINI_UI_ACTIVE;
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_HOME && ui.session_kind == 3 && ui.session_generation == 88);
}

static void test_contact_paging(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.page = XMINI_PAGE_WECHAT;
    ui.contact_count = 8;
    ui.contact_pages = 3;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_CALL_CONTACT);
    ui.contacts_loading = true;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    ui.selected = 8;
    xmini_intent_t action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(action.kind == XMINI_UI_CONTACT_PAGE && action.value == 0);
    ui.selected = 9;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    ui.selected = 10;
    action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(action.kind == XMINI_UI_CONTACT_PAGE && action.value == 1);
    ui.contact_page = 1;
    ui.selected = 9;
    action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(action.kind == XMINI_UI_CONTACT_PAGE && action.value == 0);
    ui.contact_page = 2;
    ui.selected = 10;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    ui.page = XMINI_PAGE_DEVICE;
    ui.contact_page = 1;
    ui.selected = 12;
    action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(action.kind == XMINI_UI_CONTACT_PAGE && action.value == 0);
    ui.selected = 13;
    action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(action.kind == XMINI_UI_CONTACT_PAGE && action.value == 2);

    ui.page = XMINI_PAGE_PENDING;
    ui.pending_count = 4;
    ui.pending_page = 1;
    ui.pending_pages = 3;
    ui.selected = 0;
    strcpy(ui.pending_name, "pending-current-page");
    ui.pending_loading = true;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_PENDING);
    ui.pending_loading = false;
    ui.pending_name[0] = 0;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    ui.selected = 4;
    action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(action.kind == XMINI_UI_PENDING_PAGE && action.value == 1);
    ui.selected = 5;
    action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(action.kind == XMINI_UI_PENDING_PAGE && action.value == 0);
    ui.selected = 6;
    action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(action.kind == XMINI_UI_PENDING_PAGE && action.value == 2);
    ui.pending_page = 2;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    ui.pending_page = 0;
    ui.selected = 5;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
}

static void test_page_feedback(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.page = XMINI_PAGE_WECHAT;
    ui.platform_online = ui.rtc_online = true;
    uint8_t normal[1024], changed[1024];
    xmini_ui_render(&ui, normal);
    ui.contacts_error = 0x104;
    xmini_ui_render(&ui, changed);
    assert(memcmp(normal, changed, sizeof(normal)));
    ui.contacts_error = 0;
    ui.contacts_loading = true;
    xmini_ui_render(&ui, changed);
    assert(memcmp(normal, changed, sizeof(normal)));
    ui.page = XMINI_PAGE_PENDING;
    xmini_ui_render(&ui, normal);
    ui.pending_error = 0x107;
    xmini_ui_render(&ui, changed);
    assert(memcmp(normal, changed, sizeof(normal)));
    ui.pending_error = 0;
    ui.pending_loading = true;
    xmini_ui_render(&ui, changed);
    assert(memcmp(normal, changed, sizeof(normal)));
    ui.page = XMINI_PAGE_WIFI_CONFIRM;
    xmini_ui_render(&ui, normal);
    ui.wifi_setup_pending = true;
    xmini_ui_render(&ui, changed); assert(memcmp(normal, changed, sizeof(normal)));
    memcpy(normal, changed, sizeof(normal));
    ui.wifi_setup_error = -1;
    xmini_ui_render(&ui, changed); assert(memcmp(normal, changed, sizeof(normal)));
    ui.page = XMINI_PAGE_SETTINGS; ui.settings_saved = true;
    xmini_ui_render(&ui, normal);
    ui.settings_saved = false; ui.settings_pending = true;
    xmini_ui_render(&ui, changed); assert(memcmp(normal, changed, sizeof(normal)));
    memcpy(normal, changed, sizeof(normal)); ui.settings_error = -1;
    xmini_ui_render(&ui, changed); assert(memcmp(normal, changed, sizeof(normal)));
}

static void test_group_password(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    ui.page = XMINI_PAGE_GROUP;
    ui.selected = 7;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_GROUP_JOIN && ui.group_use_password && !ui.group_create);
    for (unsigned i = 0; i < 6; ++i) {
        for (unsigned j = 0; j < i; ++j) xmini_ui_key(&ui, XMINI_KEY_NEXT);
        assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    }
    assert(!strcmp(ui.join_code, "012345") && ui.page == XMINI_PAGE_GROUP_PASSWORD && ui.digit == 0);
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < 9 - i; ++j) xmini_ui_key(&ui, XMINI_KEY_NEXT);
        xmini_intent_t action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
        assert(action.kind == (i == 3 ? XMINI_UI_GROUP_JOIN : XMINI_INTENT_NONE));
    }
    assert(!strcmp(ui.group_password, "9876") && ui.page == XMINI_PAGE_GROUP);
    ui.selected = 8;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_GROUP_PASSWORD && ui.group_use_password && ui.group_create);
    xmini_ui_key(&ui, XMINI_KEY_NEXT);
    assert(ui.group_password[0] == '0');
    for (unsigned i = 0; i < 4; ++i) {
        xmini_intent_t action = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
        assert(action.kind == (i == 3 ? XMINI_UI_GROUP_CREATE : XMINI_INTENT_NONE));
    }
    assert(ui.page == XMINI_PAGE_GROUP);
    ui.selected = 8;
    xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE && ui.page == XMINI_PAGE_GROUP);
    ui.selected = 1;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_UI_GROUP_CREATE && !ui.group_use_password);
    ui.selected = 2;
    assert(xmini_ui_key(&ui, XMINI_KEY_CONFIRM).kind == XMINI_INTENT_NONE);
    assert(ui.page == XMINI_PAGE_GROUP_JOIN && !ui.group_use_password);
    assert(xmini_ui_key(&ui, XMINI_KEY_BACK).kind == XMINI_INTENT_NONE && ui.page == XMINI_PAGE_GROUP);
}

static void test_guidance_setting(void)
{
    xmini_ui_t ui;
    xmini_ui_init(&ui);
    assert(ui.guidance_enabled);
    ui.page = XMINI_PAGE_SETTINGS;
    for (unsigned i = 0; i < 6; ++i) xmini_ui_key(&ui, XMINI_KEY_NEXT);
    assert(ui.selected == 6);
    xmini_intent_t intent = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(intent.kind == XMINI_UI_GUIDANCE && intent.value == 0 && ui.page == XMINI_PAGE_SETTINGS);
    assert(ui.volume == 40 && ui.gain_db == 24 && ui.guidance_enabled);
    uint8_t on[1024], off[1024];
    xmini_ui_render(&ui, on);
    ui.guidance_enabled = false;
    xmini_ui_render(&ui, off);
    assert(memcmp(on, off, sizeof(on)));
    intent = xmini_ui_key(&ui, XMINI_KEY_CONFIRM);
    assert(intent.kind == XMINI_UI_GUIDANCE && intent.value == 1);
    xmini_ui_key(&ui, XMINI_KEY_NEXT);
    assert(ui.selected == 0);
}

int main(void)
{
    test_gestures();
    test_context_and_wrap();
    test_same_phase_session_identity();
    test_menu_and_audio_intents();
    test_framebuffer();
    test_session_body_rows();
    test_business_intents();
    test_session_back_ends_owner();
    test_menu_traversal();
    test_pending_contact_decisions();
    test_wifi_confirmation();
    test_contact_paging();
    test_page_feedback();
    test_group_password();
    test_guidance_setting();
    return 0;
}
