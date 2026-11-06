// SPDX-License-Identifier: MIT
#include "xmini_ui.h"
#include "xmini_canvas.h"

#include <stdio.h>
#include <string.h>

static void page(xmini_ui_t *ui, xmini_page_t target)
{
    ui->page = target;
    ui->selected = 0;
    ++ui->epoch;
}

void xmini_ui_init(xmini_ui_t *ui)
{
    *ui = (xmini_ui_t){.epoch = 1, .volume = 40, .gain_db = 24, .guidance_enabled = true,
        .join_code = "000000", .group_password = "0000"};
}

void xmini_ui_session_identity(xmini_ui_t *ui, unsigned kind, uint32_t generation, const char *peer)
{
    if (!peer) peer = "";
    if (ui->session_kind != kind || ui->session_generation != generation || strcmp(ui->peer, peer))
        ++ui->epoch;
    ui->session_kind = kind;
    ui->session_generation = generation;
    if (peer != ui->peer) snprintf(ui->peer, sizeof(ui->peer), "%s", peer);
}

static xmini_intent_kind_t session_exit(const xmini_ui_t *ui)
{
    if (ui->session_phase == XMINI_UI_IDLE || ui->session_phase == XMINI_UI_ENDING)
        return XMINI_INTENT_NONE;
    if (ui->session_phase == XMINI_UI_ERROR && !ui->session_kind && !ui->session_generation)
        return XMINI_UI_DISMISS;
    return ui->session_phase == XMINI_UI_RINGING ? XMINI_UI_REJECT : XMINI_UI_HANGUP;
}

xmini_intent_t xmini_ui_key(xmini_ui_t *ui, xmini_key_event_t key)
{
    xmini_intent_t action = {0};
    if (key == XMINI_KEY_BACK) {
        if (ui->page == XMINI_PAGE_AUDIO) {
            action.kind = XMINI_INTENT_STOP;
            page(ui, XMINI_PAGE_SETTINGS);
        } else if (ui->page == XMINI_PAGE_GROUP_JOIN || ui->page == XMINI_PAGE_GROUP_PASSWORD) {
            page(ui, XMINI_PAGE_GROUP);
        } else if (ui->page == XMINI_PAGE_CONTACT_DECISION) {
            page(ui, XMINI_PAGE_PENDING);
        } else if (ui->page == XMINI_PAGE_WIFI_CONFIRM) {
            if (ui->wifi_setup_pending) action.kind = XMINI_UI_WIFI_CANCEL;
            else page(ui, XMINI_PAGE_SETTINGS);
        } else if (ui->page == XMINI_PAGE_GROUP ||
                   (ui->page == XMINI_PAGE_SESSION && ui->session_kind == 5)) {
            action.kind = XMINI_UI_GROUP_DISCONNECT;
            page(ui, XMINI_PAGE_HOME);
        } else if (ui->page == XMINI_PAGE_SESSION ||
                   (ui->session_phase != XMINI_UI_IDLE &&
                    ((ui->page == XMINI_PAGE_AI && ui->session_kind == 1) ||
                     (ui->page == XMINI_PAGE_DEVICE && ui->session_kind == 2) ||
                     (ui->page == XMINI_PAGE_WECHAT && ui->session_kind == 3) ||
                     (ui->page == XMINI_PAGE_LIVE && ui->session_kind == 4)))) {
            action.kind = session_exit(ui);
            if (ui->session_phase == XMINI_UI_IDLE) page(ui, XMINI_PAGE_HOME);
            else if (ui->page != XMINI_PAGE_SESSION) page(ui, XMINI_PAGE_SESSION);
        } else if (ui->page != XMINI_PAGE_HOME) {
            page(ui, XMINI_PAGE_HOME);
        }
    } else if (key == XMINI_KEY_NEXT) {
        if (ui->page == XMINI_PAGE_GROUP_JOIN) {
            ui->join_code[ui->digit] = (char)('0' + (ui->join_code[ui->digit] - '0' + 1) % 10);
            return action;
        }
        if (ui->page == XMINI_PAGE_GROUP_PASSWORD) {
            ui->group_password[ui->digit] = (char)('0' + (ui->group_password[ui->digit] - '0' + 1) % 10);
            return action;
        }
        unsigned count = ui->page == XMINI_PAGE_HOME ? 4 :
                         ui->page == XMINI_PAGE_SETTINGS ? 7 :
                         ui->page == XMINI_PAGE_AUDIO ? 4 :
                         ui->page == XMINI_PAGE_WECHAT ? ui->contact_count + 3 :
                         ui->page == XMINI_PAGE_DEVICE ? ui->contact_count + 6 :
                         ui->page == XMINI_PAGE_PENDING ? ui->pending_count + 3 :
                         ui->page == XMINI_PAGE_CONTACT_DECISION || ui->page == XMINI_PAGE_WIFI_CONFIRM ? 2 :
                         ui->page == XMINI_PAGE_GROUP ? 9 :
                         ui->page == XMINI_PAGE_SESSION && ui->session_phase == XMINI_UI_RINGING ? 2 :
                         ui->page == XMINI_PAGE_SESSION && ui->session_phase == XMINI_UI_ACTIVE && ui->session_kind == 1 ? 3 : 1;
        ui->selected = (ui->selected + 1) % count;
    } else if (key == XMINI_KEY_CONFIRM) {
        if (ui->page == XMINI_PAGE_HOME) {
            static const xmini_page_t targets[] = {
                XMINI_PAGE_AI, XMINI_PAGE_WECHAT, XMINI_PAGE_DEVICE, XMINI_PAGE_SETTINGS,
            };
            page(ui, targets[ui->selected]);
            if (ui->page == XMINI_PAGE_WECHAT || ui->page == XMINI_PAGE_DEVICE)
                action.kind = XMINI_UI_CONTACT_PAGE;
        } else if (ui->page == XMINI_PAGE_SETTINGS) {
            if (ui->selected == 4) {
                page(ui, XMINI_PAGE_AUDIO);
            } else if (ui->selected == 5) {
                page(ui, XMINI_PAGE_WIFI_CONFIRM);
            } else if (ui->selected == 6) {
                action.kind = XMINI_UI_GUIDANCE;
                action.value = !ui->guidance_enabled;
            } else if (ui->audio_ready) {
                if (ui->selected < 2) {
                    action.kind = XMINI_INTENT_VOLUME;
                    action.value = ui->volume + (ui->selected == 0 ? 5 : -5);
                    if (action.value < 0) action.value = 0;
                    if (action.value > 100) action.value = 100;
                } else {
                    action.kind = XMINI_INTENT_GAIN;
                    action.value = ui->gain_db + (ui->selected == 2 ? 3 : -3);
                    if (action.value < 0) action.value = 0;
                    if (action.value > 36) action.value = 36;
                }
            }
        } else if (ui->page == XMINI_PAGE_AI) {
            if (ui->session_phase == XMINI_UI_IDLE) action.kind = XMINI_UI_AI_START;
            else page(ui, XMINI_PAGE_SESSION);
        } else if (ui->page == XMINI_PAGE_WECHAT || ui->page == XMINI_PAGE_DEVICE) {
            if (ui->session_phase != XMINI_UI_IDLE &&
                ui->session_kind == (ui->page == XMINI_PAGE_WECHAT ? 3U : 2U)) {
                page(ui, XMINI_PAGE_SESSION);
            } else if (ui->page == XMINI_PAGE_DEVICE && ui->selected == ui->contact_count + 1) {
                page(ui, XMINI_PAGE_GROUP);
                action.kind = XMINI_UI_GROUP_REFRESH;
            } else if (ui->page == XMINI_PAGE_DEVICE && ui->selected == ui->contact_count + 2) {
                page(ui, XMINI_PAGE_LIVE);
            } else if (ui->page == XMINI_PAGE_DEVICE && ui->selected == ui->contact_count + 3) {
                page(ui, XMINI_PAGE_PENDING);
                action.kind = XMINI_UI_CONTACT_REFRESH;
            } else if (ui->selected == ui->contact_count) {
                action.kind = XMINI_UI_CONTACT_PAGE;
                action.value = (int)ui->contact_page;
            } else if (ui->selected >= ui->contact_count + (ui->page == XMINI_PAGE_DEVICE ? 4U : 1U)) {
                bool previous = ui->selected == ui->contact_count + (ui->page == XMINI_PAGE_DEVICE ? 4U : 1U);
                if ((previous && ui->contact_page) || (!previous && ui->contact_page + 1 < ui->contact_pages)) {
                    action.kind = XMINI_UI_CONTACT_PAGE;
                    action.value = (int)(previous ? ui->contact_page - 1 : ui->contact_page + 1);
                }
            } else if (!ui->contacts_loading) {
                action.kind = ui->selected < ui->contact_count ? XMINI_UI_CALL_CONTACT : XMINI_UI_REFRESH_CONTACTS;
                action.value = (int)ui->selected;
            }
        } else if (ui->page == XMINI_PAGE_WIFI_CONFIRM) {
            if (ui->selected == 0 && ui->wifi_setup_pending) action.kind = XMINI_UI_WIFI_CANCEL;
            else if (ui->selected == 1 && !ui->wifi_setup_pending) action.kind = XMINI_UI_WIFI_SETUP;
            else if (ui->wifi_setup_pending) return action;
            else page(ui, XMINI_PAGE_SETTINGS);
        } else if (ui->page == XMINI_PAGE_PENDING) {
            if (ui->selected < ui->pending_count && ui->pending_name[0] && !ui->pending_loading) {
                memcpy(ui->pending_peer, ui->pending_name, sizeof(ui->pending_peer));
                ui->pending_present = true;
                page(ui, XMINI_PAGE_CONTACT_DECISION);
            } else if (ui->selected == ui->pending_count) {
                action.kind = XMINI_UI_PENDING_PAGE;
                action.value = (int)ui->pending_page;
            } else if (ui->selected >= ui->pending_count + 1) {
                bool previous = ui->selected == ui->pending_count + 1;
                if ((previous && ui->pending_page) || (!previous && ui->pending_page + 1 < ui->pending_pages)) {
                    action.kind = XMINI_UI_PENDING_PAGE;
                    action.value = (int)(previous ? ui->pending_page - 1 : ui->pending_page + 1);
                }
            }
        } else if (ui->page == XMINI_PAGE_CONTACT_DECISION) {
            if (ui->pending_present)
                action.kind = ui->selected == 0 ? XMINI_UI_CONTACT_ACCEPT : XMINI_UI_CONTACT_REJECT;
        } else if (ui->page == XMINI_PAGE_GROUP) {
            static const xmini_intent_kind_t actions[] = {XMINI_UI_GROUP_REFRESH, XMINI_UI_GROUP_CREATE,
                XMINI_INTENT_NONE, XMINI_UI_GROUP_CONNECT, XMINI_UI_GROUP_DISCONNECT, XMINI_UI_GROUP_LEAVE, XMINI_UI_GROUP_TALK};
            action.kind = ui->selected < 7 ? actions[ui->selected] : XMINI_INTENT_NONE;
            if (ui->selected == 1) ui->group_use_password = false;
            if (ui->selected == 2 || ui->selected == 7) {
                ui->group_use_password = ui->selected == 7;
                ui->group_create = false;
                ui->digit = 0;
                page(ui, XMINI_PAGE_GROUP_JOIN);
            } else if (ui->selected == 8) {
                ui->group_use_password = ui->group_create = true;
                ui->digit = 0;
                page(ui, XMINI_PAGE_GROUP_PASSWORD);
            }
        } else if (ui->page == XMINI_PAGE_GROUP_JOIN) {
            if (++ui->digit == 6) {
                if (ui->group_use_password) { ui->digit = 0; page(ui, XMINI_PAGE_GROUP_PASSWORD); }
                else { action.kind = XMINI_UI_GROUP_JOIN; page(ui, XMINI_PAGE_GROUP); }
            }
        } else if (ui->page == XMINI_PAGE_GROUP_PASSWORD) {
            if (++ui->digit == 4) {
                action.kind = ui->group_create ? XMINI_UI_GROUP_CREATE : XMINI_UI_GROUP_JOIN;
                page(ui, XMINI_PAGE_GROUP);
            }
        } else if (ui->page == XMINI_PAGE_LIVE && ui->session_phase != XMINI_UI_IDLE && ui->session_kind == 4) {
            page(ui, XMINI_PAGE_SESSION);
        } else if (ui->page == XMINI_PAGE_SESSION) {
            if (ui->session_phase == XMINI_UI_RINGING)
                action.kind = ui->selected == 0 ? XMINI_UI_ACCEPT : XMINI_UI_REJECT;
            else if (ui->session_phase == XMINI_UI_ACTIVE && ui->session_kind == 1) {
                static const xmini_intent_kind_t kinds[] = {XMINI_UI_INTERRUPT, XMINI_UI_SUBMIT, XMINI_UI_HANGUP};
                action.kind = kinds[ui->selected % 3];
            } else if (ui->session_phase == XMINI_UI_ERROR) action.kind = session_exit(ui);
            else if (ui->session_phase != XMINI_UI_IDLE && ui->session_phase != XMINI_UI_ENDING)
                action.kind = XMINI_UI_HANGUP;
        } else if (ui->page == XMINI_PAGE_AUDIO && ui->audio_ready) {
            static const xmini_intent_kind_t kinds[] = {
                XMINI_INTENT_TONE, XMINI_INTENT_RECORD, XMINI_INTENT_PLAY, XMINI_INTENT_STOP,
            };
            if (ui->selected == 3 || (!ui->audio_busy &&
                (ui->selected != 2 || ui->recording_ready))) {
                action.kind = kinds[ui->selected];
            }
        }
    }
    return action;
}

void xmini_ui_render(const xmini_ui_t *ui, uint8_t frame[1024])
{
    static const char *const home[] = {"AI", "WX", "DEV", "SET"};
    static const char *const settings[] = {"音量 +", "音量 -", "麦克风 +", "麦克风 -", "音频诊断", "重新配网"};
    static const char *const audio[] = {"提示音", "录音", "回放", "停止"};
    char text[24];
    memset(frame, 0, 1024);
    // Reserve rows 14..21 below the header; all body text starts at row 22 or later.
    if (ui->page == XMINI_PAGE_HOME) {
        xmini_canvas_utf8_center(frame, 2, "小钛 XMINI-C3");
        for (unsigned i = 0; i < 4; ++i) {
            snprintf(text, sizeof(text), "%c%s", i == ui->selected ? '>' : ' ', home[i]);
            xmini_canvas_text(frame, (i % 2) * 68, i < 2 ? 22 : 44, text, 2);
        }
    } else if (ui->page == XMINI_PAGE_SETTINGS) {
        xmini_canvas_utf8_center(frame, 2, "设置");
        if (ui->selected == 6) {
            snprintf(text, sizeof(text), "语音引导 %s", ui->guidance_enabled ? "开" : "关");
            xmini_canvas_utf8_center(frame, 25, text);
        } else xmini_canvas_utf8_center(frame, 25, settings[ui->selected]);
        xmini_canvas_utf8_center(frame, 40, ui->settings_error ? "保存失败" :
            ui->settings_pending ? "正在保存" : ui->settings_saved ? "已保存" : "设置不可用");
        snprintf(text, sizeof(text), "V:%d MIC:%d DB", ui->volume, ui->gain_db);
        xmini_canvas_center(frame, 54, ui->audio_ready ? text : "AUDIO ERROR", 1);
    } else if (ui->page == XMINI_PAGE_AUDIO) {
        xmini_canvas_utf8_center(frame, 2, "音频诊断 PCMA");
        xmini_canvas_utf8_center(frame, 24, audio[ui->selected]);
        snprintf(text, sizeof(text), "F:%u PEAK:%lu", ui->frames, (unsigned long)ui->peak);
        xmini_canvas_center(frame, 45, text, 1);
        const char *state = !ui->audio_ready || ui->audio_error ? "AUDIO ERROR" :
                            ui->audio_busy ? "BUSY" :
                            ui->recording_ready ? "RECORDED" : "IDLE";
        xmini_canvas_center(frame, 56, state, 1);
    } else if (ui->page == XMINI_PAGE_WIFI_CONFIRM) {
        xmini_canvas_utf8_center(frame, 0, "重新配网");
        xmini_canvas_utf8_center(frame, 22, ui->wifi_setup_error ? "准备未完成" :
            ui->wifi_setup_pending ? "正在准备" : "保留设备绑定");
        if (ui->wifi_setup_error) {
            snprintf(text, sizeof(text), "ERR %04X", (unsigned)ui->wifi_setup_error);
            xmini_canvas_center(frame, 34, text, 1);
        }
        xmini_canvas_utf8_center(frame, 48, ui->selected == 0 ? "> 取消" :
            ui->wifi_setup_pending ? "请等待或取消" : "> 确认重新配网");
    } else if (ui->page == XMINI_PAGE_PENDING || ui->page == XMINI_PAGE_CONTACT_DECISION) {
        xmini_canvas_utf8_center(frame, 0, "联系人申请");
        if (ui->page == XMINI_PAGE_CONTACT_DECISION) {
            xmini_canvas_utf8_window(frame, 0, 22, ui->pending_peer, 128, ui->scroll_tick);
            xmini_canvas_utf8_center(frame, 48, !ui->pending_present ? "申请状态已更新" :
                ui->selected == 0 ? "> 接受申请" : "> 拒绝申请");
        } else if (ui->selected < ui->pending_count) {
            xmini_canvas_utf8_window(frame, 0, 22, ui->pending_name, 128, ui->scroll_tick);
            xmini_canvas_utf8_center(frame, 48, "> 查看申请");
        } else if (ui->selected == ui->pending_count) {
            xmini_canvas_utf8_center(frame, 22, ui->pending_loading ? "正在加载" : ui->pending_error ? "加载失败" :
                ui->pending_count ? "申请列表" : "暂无申请");
            xmini_canvas_utf8_center(frame, 48, "> 刷新申请");
        } else {
            bool previous = ui->selected == ui->pending_count + 1;
            snprintf(text, sizeof(text), "%u / %u", ui->pending_page + 1, ui->pending_pages ? ui->pending_pages : 1);
            xmini_canvas_center(frame, 22, text, 2);
            xmini_canvas_utf8_center(frame, 48, previous ? (ui->pending_page ? "> 上一页" : "已是首页") :
                (ui->pending_page + 1 < ui->pending_pages ? "> 下一页" : "已是末页"));
        }
    } else if (ui->page == XMINI_PAGE_SESSION) {
        const char *title = ui->session_kind == 1 ? "人工智能" : ui->session_kind == 2 ? "设备呼叫" :
                            ui->session_kind == 3 ? "微信通话" : ui->session_kind == 5 ? "多人对讲" : "实时语音";
        xmini_canvas_utf8_center(frame, 0, title);
        const char *phase = ui->session_phase == XMINI_UI_RINGING ? "来电" :
                            ui->session_phase == XMINI_UI_ACTIVE ? "通话中" :
                            ui->session_phase == XMINI_UI_ENDING ? "正在结束" :
                            ui->session_phase == XMINI_UI_ERROR ? "通话失败" : "正在连接";
        xmini_canvas_utf8_center(frame, 22, phase);
        xmini_canvas_utf8_window(frame, 0, 36, ui->caption[0] ? ui->caption : ui->peer, 128, ui->scroll_tick);
        const char *action = ui->session_phase == XMINI_UI_RINGING ? (ui->selected == 0 ? "> 接听" : "> 拒绝") :
                             ui->session_phase == XMINI_UI_ACTIVE && ui->session_kind == 1 ?
                                (ui->selected == 0 ? "> 打断" : ui->selected == 1 ? "> 发送语音" : "> 结束对话") :
                             ui->session_phase == XMINI_UI_ACTIVE ? "> 挂断" :
                             ui->session_phase == XMINI_UI_ENDING ? "结束中" :
                             ui->session_phase == XMINI_UI_ERROR ?
                                (ui->session_kind == 2 ? "> 重试结束" :
                                 ui->session_kind || ui->session_generation ? "结束未完成" : "> 返回") : "> 取消通话";
        xmini_canvas_utf8_center(frame, 50, action);
    } else if (ui->page == XMINI_PAGE_GROUP) {
        static const char *const actions[] = {"刷新房间", "创建房间", "加入房间", "连接房间", "断开连接", "退出房间", "开始发言", "加入密码房间", "创建密码房间"};
        xmini_canvas_utf8_center(frame, 0, "多人对讲");
        xmini_canvas_center(frame, 22, ui->group_assigned ? ui->room_code : "NO ROOM", 2);
        const char *action = ui->selected == 6 && ui->group_speaking ? "停止发言" : actions[ui->selected % 9];
        xmini_canvas_utf8_center(frame, 48, action);
    } else if (ui->page == XMINI_PAGE_GROUP_JOIN) {
        xmini_canvas_utf8_center(frame, 0, "输入房间号");
        xmini_canvas_center(frame, 22, ui->join_code, 2);
        char cursor[7] = "      ";
        cursor[ui->digit % 6] = '+';
        xmini_canvas_center(frame, 41, cursor, 2);
    } else if (ui->page == XMINI_PAGE_GROUP_PASSWORD) {
        xmini_canvas_utf8_center(frame, 0, "输入房间密码");
        char password[5] = "****", cursor[5] = "    ";
        password[ui->digit % 4] = ui->group_password[ui->digit % 4];
        cursor[ui->digit % 4] = '+';
        xmini_canvas_center(frame, 22, password, 2);
        xmini_canvas_center(frame, 41, cursor, 2);
    } else if (ui->page == XMINI_PAGE_LIVE) {
        xmini_canvas_utf8_center(frame, 0, "实时语音");
        xmini_canvas_utf8_center(frame, 25, ui->rtc_online ? "等待连接" : "正在连接");
        xmini_canvas_center(frame, 53, "PCMA 8K MONO", 1);
    } else {
        const char *title = ui->page == XMINI_PAGE_AI ? "人工智能" :
                            ui->page == XMINI_PAGE_WECHAT ? "微信通话" : "设备呼叫";
        xmini_canvas_utf8_center(frame, 2, title);
        if (ui->page == XMINI_PAGE_AI) {
            xmini_canvas_utf8_center(frame, 27, ui->rtc_online ? "> 开始对话" : "正在连接");
        } else if (ui->selected < ui->contact_count) {
            xmini_canvas_utf8_window(frame, 0, 27, ui->contact_name, 128, ui->scroll_tick);
        } else if (ui->page == XMINI_PAGE_DEVICE && ui->selected == ui->contact_count + 1)
            xmini_canvas_utf8_center(frame, 27, "> 多人对讲");
        else if (ui->page == XMINI_PAGE_DEVICE && ui->selected == ui->contact_count + 2)
            xmini_canvas_utf8_center(frame, 27, "> 实时语音");
        else if (ui->page == XMINI_PAGE_DEVICE && ui->selected == ui->contact_count + 3)
            xmini_canvas_utf8_center(frame, 27, "> 联系人申请");
        else if (ui->selected >= ui->contact_count + (ui->page == XMINI_PAGE_DEVICE ? 4U : 1U)) {
            bool previous = ui->selected == ui->contact_count + (ui->page == XMINI_PAGE_DEVICE ? 4U : 1U);
            xmini_canvas_utf8_center(frame, 27, previous ? (ui->contact_page ? "> 上一页" : "已是首页") :
                (ui->contact_page + 1 < ui->contact_pages ? "> 下一页" : "已是末页"));
        }
        else xmini_canvas_utf8_center(frame, 27, "> 刷新联系人");
        if (ui->contacts_error && ui->page != XMINI_PAGE_AI) snprintf(text, sizeof(text), "ERROR:%d", ui->contacts_error);
        xmini_canvas_center(frame, 53, ui->maintenance ? "MAINTENANCE" : !ui->platform_online ? "PLATFORM OFFLINE" :
            !ui->rtc_online ? "RTC CONNECTING" : ui->contacts_loading && ui->page != XMINI_PAGE_AI ? "LOADING" :
            ui->contacts_error && ui->page != XMINI_PAGE_AI ? text : "PCMA 8K MONO", 1);
    }
}
