// SPDX-License-Identifier: MIT
/* The conversation's expression tool: what a user asks of her face while
 * talking to her ("make a happy face", "look surprised"). One action name the
 * service calls (xiaotai/modules/ai_talk.h, tools):
 *
 *   show_expression    expression: a name from the expression list (开心, 大笑,
 *                      害羞, ...), or one of the service's emotion words;
 *                      seconds: how long, 1..10, default 5 (optional)
 *
 * The face stands instead of the one the captions' emotion words give, in
 * this conversation only, and goes back by itself (companion_expression_show).
 * Registered only where the device shows a face. The answer is a result
 * object {"ok", "status", "message"} like the device tools', with
 * "expression" and "seconds" when the face is shown; `status` is ok,
 * invalid_params, not_shown (no face on the screen now) or failed. */
#include "ai_expression_tool.h"

#include <string.h>
#include "companion_internal.h"
#include "xiaotai/json.h"
#include "xiaotai/modules/ai_talk.h"

#define DEFAULT_SECONDS 5
#define MAX_SECONDS 10
#define WORD_BYTES 48U
#define REPLY_BYTES 384U

/* The face a word names: the expression list's Chinese names and theme names
 * and the service's emotion words (xiaotai_app_companion_emotion_face); NULL
 * for anything else. */
static const char *face_of(const char *word)
{
    const char *face = xiaotai_app_companion_emotion_face(word);
    if (!strcmp(face, XIAOTAI_EMOTION_NEUTRAL) && strcmp(word, "中性") && strcmp(word, "平静") &&
        strcmp(word, XIAOTAI_EMOTION_NEUTRAL)) return NULL;
    return face;
}

static void answer(const xiaotai_ai_talk_tool_call_t *call, const char *status, const char *message,
                   const char *label, int32_t seconds)
{
    char text[REPLY_BYTES];
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, text, sizeof(text));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_bool(&writer, "ok", label != NULL);
    xiaotai_json_put_string(&writer, "status", status);
    xiaotai_json_put_string(&writer, "message", message);
    if (label) {
        xiaotai_json_put_string(&writer, "expression", label);
        xiaotai_json_put_int(&writer, "seconds", seconds);
    }
    xiaotai_json_object_end(&writer);
    if (xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_OK)
        (void)xiaotai_ai_talk_tool_reply(call->session, call->request, text);
}

static void expression_tool(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    char word[WORD_BYTES];
    int32_t seconds = DEFAULT_SECONDS;
    const char *face = NULL;
    if (xiaotai_ai_talk_tool_arg_word(call, "expression", word, sizeof(word)) == XIAOTAI_OK) face = face_of(word);
    const xiaotai_err_t timed = xiaotai_ai_talk_tool_arg_int(call, "seconds", &seconds);
    if (!face || (timed != XIAOTAI_OK && timed != XIAOTAI_ERR_NOT_FOUND)) {
        answer(call, "invalid_params", "参数无效：expression 取表情列表里的名字，比如开心、大笑、害羞、惊讶、思考；"
               "seconds 取 1 到 10。", NULL, 0);
        return;
    }
    /* What a model asks for beyond the range is the nearest it can have. */
    if (seconds < 1) seconds = 1;
    if (seconds > MAX_SECONDS) seconds = MAX_SECONDS;
    const xiaotai_err_t err = companion_expression_show(call->session, face, (uint32_t)seconds * 1000U);
    if (err == XIAOTAI_ERR_INVALID_STATE) answer(call, "not_shown", "现在屏幕上没有显示小钛，做不了表情。", NULL, 0);
    else if (err != XIAOTAI_OK) answer(call, "failed", "表情没有显示出来。", NULL, 0);
    else answer(call, "ok", "表情已经做出来了。", word, seconds);
}

xiaotai_err_t companion_ai_expression_tool_init(void)
{
    return xiaotai_ai_talk_tool_register("show_expression", expression_tool, NULL);
}
