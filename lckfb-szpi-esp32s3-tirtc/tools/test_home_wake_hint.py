"""Check the actual home status/hint writer without hardware or a server."""
from pathlib import Path
import subprocess
import tempfile

from test_captive_portal_contract import function_body

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_product/src/starter_product_s3_ui.inc").read_text(encoding="utf-8")
body = function_body(source, "s3_refresh_home_status")
assert body
refresh = function_body(source, "s3_refresh_ui")
assert "s3_refresh_home_status(runtime, product, ai_pending);" in refresh

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "starter_runtime.h"
#define LV_OBJ_FLAG_HIDDEN 1U
#define S3_ACTION_AI_STOP 40U
typedef struct { const char *text; bool hidden; } label_t;
static label_t header, hint, captions;
static label_t *s_state = &header;
static struct { label_t *wake_hint, *caption_panel; unsigned pending_action; }
    s3_ui = {&hint, &captions, 0};
static struct { bool microphone_muted; } s_preferences;
static char s_voice_feedback[80];
static const char *phase_text(starter_ai_ui_phase_t phase) {
    (void)phase; return "AI 回复中";
}
static void label_set_text_if_changed(label_t *label, const char *text) {
    assert(label && text); label->text = text;
}
static void lv_obj_clear_flag(label_t *label, unsigned flag) {
    assert(flag == LV_OBJ_FLAG_HIDDEN); label->hidden = false;
}
static void lv_obj_add_flag(label_t *label, unsigned flag) {
    assert(flag == LV_OBJ_FLAG_HIDDEN); label->hidden = true;
}
static void s3_refresh_home_status(starter_runtime_status_t runtime,
    const starter_runtime_product_snapshot_t *product, bool ai_pending) { BODY }

static void update(starter_runtime_state_t state, bool pending) {
    starter_runtime_status_t runtime = {.state = state};
    starter_runtime_product_snapshot_t product = {.ai_phase = STARTER_AI_UI_SPEAKING};
    s3_refresh_home_status(runtime, &product, pending);
}
int main(void) {
    const char *statuses[] = {
        "待唤醒", "远程查看中", "正在连接 AI", "AI 回复中",
        "语音来电", "正在建立通话", "通话中", "正在加入房间", "房间对讲中"
    };
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        for (int state = STARTER_RUNTIME_WAITING; state <= STARTER_RUNTIME_ROOM_ACTIVE; ++state) {
            /* Start from deliberately stale visibility, as after a page rebuild. */
            hint.hidden = true; captions.hidden = false;
            update(state, false);
            assert(!strcmp(header.text, statuses[state]));
            bool idle = state == STARTER_RUNTIME_WAITING;
            assert(hint.hidden == !idle && captions.hidden == idle);
            update(STARTER_RUNTIME_WAITING, false);
            assert(!hint.hidden && captions.hidden && !strcmp(header.text, "待唤醒"));
            assert(!strcmp(hint.text, "尝试说“你好小钛”\n或者点击表情唤醒对话"));
        }
    }
    /* A queued AI request suppresses wake guidance until it completes/expires. */
    update(STARTER_RUNTIME_WAITING, true);
    assert(hint.hidden && !strcmp(header.text, "正在连接 AI"));
    s3_ui.pending_action = S3_ACTION_AI_STOP;
    update(STARTER_RUNTIME_AI_ACTIVE, true);
    assert(hint.hidden && !strcmp(header.text, "结束对话中"));
    update(STARTER_RUNTIME_WAITING, false);
    assert(!hint.hidden && captions.hidden);
    s_preferences.microphone_muted = true;
    update(STARTER_RUNTIME_WAITING, false);
    assert(!strcmp(header.text, "麦克风关闭"));
    assert(!strcmp(hint.text, "麦克风已关闭\n点击表情唤醒对话"));
    assert(!hint.hidden && captions.hidden);
    strcpy(s_voice_feedback, "连接失败，请重新开始");
    update(STARTER_RUNTIME_WAITING, false);
    assert(!strcmp(header.text, s_voice_feedback) && !hint.hidden);
    update(STARTER_RUNTIME_ROOM_ACTIVE, false);
    assert(!strcmp(header.text, "房间对讲中") && hint.hidden);
    update((starter_runtime_state_t)99, false);
    assert(!strcmp(header.text, "状态同步中") && hint.hidden);
    puts("PASS: home status/hint agreement, all owners, pending, mute, feedback and idle restoration");
}
'''.replace("BODY", body)

with tempfile.TemporaryDirectory(prefix="s3-home-hint-") as directory:
    temp = Path(directory)
    (temp / "esp_err.h").write_text("typedef int esp_err_t;\n", encoding="utf-8")
    c = temp / "test.c"
    exe = temp / "test"
    c.write_text(harness, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-I", str(temp),
                    "-I", str(root / "components/starter_runtime/include"),
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
