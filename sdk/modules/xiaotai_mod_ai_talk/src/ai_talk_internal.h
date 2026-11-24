// SPDX-License-Identifier: MIT
#pragma once
/* Internal interfaces between the sources of the AI talk module. Everything
 * here runs on the framework owner task.
 *
 * ai_talk.c and ai_talk_protocol.c own the conversation: state machine,
 * activity, deadlines, token, link, session handshake and every end. Two
 * seams lead to the caption (ai_talk_caption.c) and to the tools and the
 * console (ai_talk_tools.c, ai_talk_at.c), which implement the functions
 * declared under "caption" and "tools" below; the conversation offers them
 * the functions under "conversation". */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/json_view.h"
#include "xiaotai/modules/ai_talk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- protocol facts */

#define AI_TALK_COMMAND 0x2100U
/* Both directions of the session's audio; also its link stream. */
#define AI_TALK_STREAM 1U
#define AI_TALK_RATE_HZ 8000U
/* What deployed firmware asks the platform for and waits for the token. */
#define AI_TALK_TOKEN_PATH "/v1/ai/token"
#define AI_TALK_TOKEN_HTTP_TIMEOUT_MS 15000U
#define AI_TALK_TOKEN_RESPONSE_BYTES 2560U
/* Credential sizes with the terminator (both under XIAOTAI_LINK_TOKEN_MAX). */
#define AI_TALK_CREDENTIAL_MAX 1024U
#define AI_TALK_ROLE_MAX 65U
#define AI_TALK_REQUEST_ID_MAX 17U   /* 16 hexadecimal digits */
/* The session request: identifiers of at most 64 bytes, escaped. */
#define AI_TALK_REQUEST_TEXT_MAX 768U

/* Notifications this side sends, byte for byte as deployed firmware does. */
#define AI_TALK_END_SESSION "{\"jsonrpc\":\"2.0\",\"method\":\"end_session\"}"
#define AI_TALK_INTERRUPT "{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}"
#define AI_TALK_SUBMIT "{\"jsonrpc\":\"2.0\",\"method\":\"submit_speech\"}"

/* Decoded credentials of a token reply; XIAOTAI_MEM_BULK, wiped and freed
 * right after the link request copied them. */
typedef struct {
    char peer[AI_TALK_CREDENTIAL_MAX];
    char token[AI_TALK_CREDENTIAL_MAX];
    char role[AI_TALK_ROLE_MAX];
} ai_talk_credentials_t;

/* XIAOTAI_OK: HTTP 200, one JSON object (blanks around it allowed) whose
 * "code" is the number 200, "data.peer_id" and "data.token" non-empty strings
 * shorter than AI_TALK_CREDENTIAL_MAX bytes, "data.role_id" a string shorter
 * than AI_TALK_ROLE_MAX bytes or absent (role_required: present and not
 * empty). XIAOTAI_ERR_INVALID_ARG: not JSON or not an object;
 * XIAOTAI_FAIL: anything else refused. */
xiaotai_err_t ai_talk_token_parse(int http_status, const char *body, size_t size, bool role_required,
                                  size_t capacity, ai_talk_credentials_t *out);

/* start_session with the given request id, device id and role. */
xiaotai_err_t ai_talk_session_request(char *out, size_t capacity, const char *request_id, const char *device_id,
                                      const char *role, size_t *length);

typedef enum {
    AI_TALK_MESSAGE_INVALID = 0,   /* not one JSON object, or "method" is not a string */
    AI_TALK_MESSAGE_REPLY,         /* no "method": the answer to a request of this side */
    AI_TALK_MESSAGE_METHOD,        /* a notification or a request of the service */
} ai_talk_message_kind_t;

typedef struct {
    ai_talk_message_kind_t kind;
    xiaotai_json_view_t id, method, params, result;
    bool has_error;
} ai_talk_message_t;

/* Views borrow the command payload until the transport callback returns.
 * Ignore blanks and NUL bytes around the value, as deployed firmware did. */
void ai_talk_message_parse(const char *text, size_t size, size_t capacity,
                           ai_talk_message_t *out);
/* True for the method name, compared exactly. */
bool ai_talk_message_is(const ai_talk_message_t *message, const char *method);
/* The service has used both integer and decimal/exponent JSON spellings. */
bool ai_talk_number(const xiaotai_json_view_t *value, double *out);
/* Collect the first occurrence of each requested member in one object pass. */
void ai_talk_fields(const xiaotai_json_view_t *object, const char *const *names,
                    xiaotai_json_view_t *fields, size_t count);

typedef enum {
    AI_TALK_HANDSHAKE_NONE = 0,     /* not the reply to request_id */
    AI_TALK_HANDSHAKE_ACCEPTED,
    AI_TALK_HANDSHAKE_REFUSED,      /* it carries "error" */
    AI_TALK_HANDSHAKE_UNSUPPORTED,  /* no session id, or a format other than G.711 A-law 8 kHz mono */
} ai_talk_handshake_t;

/* A reply whose string "id" equals request_id (non-empty) is the session
 * reply: refused with "error", accepted with a non-empty "result.session_id"
 * and both "input_audio" and "output_audio" naming "g711a" or "alaw", 8000
 * and 1 (any JSON number of that value). */
ai_talk_handshake_t ai_talk_handshake_check(const ai_talk_message_t *message, const char *request_id);

/* ------------------------------------------------------------- conversation */

/* True while the session runs (LISTENING, THINKING, SPEAKING); *session and
 * *link (either may be NULL) name it. */
bool ai_talk_current(uint32_t *session, uint32_t *link);
/* Sends one command on the conversation's open link (AI_TALK_COMMAND).
 * XIAOTAI_ERR_INVALID_STATE without one. */
xiaotai_err_t ai_talk_send(const char *json, size_t length);
/* A tool request or notification was dispatched; handled: a tool took it. */
void ai_talk_count_tool(bool handled);
void ai_talk_count_stale_reply(void);
void ai_talk_count_tool_reply_failure(void);

/* Counters the status does not carry, for the diagnostics section. */
typedef struct {
    uint32_t video_extra;        /* link video contributions beyond the first */
    uint32_t commands_dropped;   /* commands the transport delivered dropped */
    uint32_t events_lost;        /* state events the owner queue refused */
    uint32_t token_capacity;     /* JSON tokens per command (0: not started) */
    uint32_t tool_reply_failures; /* valid replies that could not be composed or sent */
} ai_talk_diag_t;
void ai_talk_diag(ai_talk_diag_t *out);

/* ------------------------------------------------------------------ caption */

/* Module start(): allocate the caption and its ordering table (BULK). */
void ai_talk_caption_start(size_t caption_bytes);
/* A new conversation begins: empty caption, revision 0. */
void ai_talk_caption_begin(uint32_t session);
/* One "caption" notification (params: its object). True when it was accepted
 * (not filtered as out of order or repeated, and decodable): *speaker and
 * *final then describe it and the conversation sets its phase. */
bool ai_talk_caption_apply(const xiaotai_json_view_t *params, xiaotai_ai_talk_speaker_t *speaker,
                           bool *final);
uint32_t ai_talk_caption_revision(void);
void ai_talk_caption_reset(void);

/* -------------------------------------------------------------------- tools */

/* Module init(): AT+AI and the diagnostics provider "ai_talk". */
xiaotai_err_t ai_talk_at_init(void);
/* Module start(): the tool reply buffer where the board has external RAM. */
void ai_talk_tools_start(void);
/* A method of the session the conversation does not handle itself:
 * "device_action" (device_action true) or any other one. The message views are
 * valid during the call and fit the configured inbound limit. */
void ai_talk_tools_dispatch(const ai_talk_message_t *message, bool device_action);
/* The registered name of a tool that ran, 0 the latest; NULL: none. */
const char *ai_talk_tools_recent(size_t index);
/* The conversation ends: requests not answered yet are dropped. */
void ai_talk_tools_end(uint32_t session);
void ai_talk_tools_reset(void);

#ifdef __cplusplus
}
#endif
