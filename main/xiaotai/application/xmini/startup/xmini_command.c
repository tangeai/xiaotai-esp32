// SPDX-License-Identifier: MIT
#include "xmini_command.h"

#include <stddef.h>
#include <string.h>

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_hex(const char *hex, size_t length, char *out, size_t capacity)
{
    if ((length & 1U) || length / 2 >= capacity) return 0;
    for (size_t i = 0; i < length / 2; ++i) {
        int high = hex_digit(hex[2 * i]), low = hex_digit(hex[2 * i + 1]);
        if (high < 0 || low < 0 || !(high || low)) return 0;
        out[i] = (char)(high * 16 + low);
    }
    out[length / 2] = '\0';
    return 1;
}

static int identifier(const char *text)
{
    size_t length = strlen(text);
    if (!length || length >= 65) return 0;
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int digits(const char *text, size_t count)
{
    if (strlen(text) != count) return 0;
    for (size_t i = 0; i < count; ++i) if (text[i] < '0' || text[i] > '9') return 0;
    return 1;
}

static int page_number(const char *text, unsigned *page)
{
    unsigned value = 0;
    if (!*text) return 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > 6553) return 0;
        value = value * 10 + (unsigned)(*p - '0');
        if (value > 65535) return 0;
    }
    *page = value;
    return 1;
}

xmini_command_t xmini_command_parse(const char *line)
{
    xmini_command_t result = {0};
    if (line && !strncmp(line, "AT+WIFI ", 8) && strchr(line + 8, ',')) {
        const char *ssid = line + 8, *separator = strchr(ssid, ',');
        if (!separator || !decode_hex(ssid, (size_t)(separator - ssid), result.ssid, sizeof(result.ssid)) ||
            !result.ssid[0] || !decode_hex(separator + 1, strlen(separator + 1), result.password, sizeof(result.password)))
            return (xmini_command_t){0};
        size_t length = strlen(result.password);
        if (length && length < 8) return (xmini_command_t){0};
        result.kind = XMINI_CMD_WIFI;
        return result;
    }
    char text[768];
    if (!line) return result;
    size_t limit = !strncmp(line, "AT+CONTACT ", 11) ? sizeof(text) : 96;
    if (strlen(line) >= limit) return result;
    strcpy(text, line);
    char *args[5] = {0};
    unsigned count = 0;
    char *cursor = text;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (!*cursor) break;
        if (count == 5) return result;
        args[count++] = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t') ++cursor;
        if (*cursor) *cursor++ = '\0';
    }
    if (count == 1) {
        static const struct { const char *text; xmini_command_kind_t kind; } names[] = {
            {"AT", XMINI_CMD_AT}, {"AT+HELP", XMINI_CMD_HELP}, {"help", XMINI_CMD_HELP},
            {"AT+VERSION", XMINI_CMD_VERSION}, {"AT+STATUS", XMINI_CMD_STATUS},
            {"AT+PRODUCT", XMINI_CMD_PRODUCT}, {"AT+RETRY", XMINI_CMD_RETRY},
            {"AT+GUIDANCE", XMINI_CMD_GUIDANCE},
            {"AT+CONSOLE", XMINI_CMD_CONSOLE},
            {"AT+RTC", XMINI_CMD_RTC}, {"AT+CONTACTS", XMINI_CMD_CONTACTS},
            {"AT+MEMTRACE", XMINI_CMD_MEMTRACE},
            {"AT+CMDTRACE", XMINI_CMD_CMDTRACE},
            {"AT+NETWORK", XMINI_CMD_NETWORK},
            {"AT+ECHO", XMINI_CMD_ECHO},
            {"AT+PENDING", XMINI_CMD_PENDING},
            {"status", XMINI_CMD_STATUS}, {"AT+UI", XMINI_CMD_UI},
            {"AT+SCREEN", XMINI_CMD_SCREEN}, {"AT+DISPLAY", XMINI_CMD_DISPLAY},
            {"display", XMINI_CMD_DISPLAY}, {"tone", XMINI_CMD_TONE},
            {"record", XMINI_CMD_RECORD}, {"play", XMINI_CMD_PLAY}, {"stop", XMINI_CMD_STOP},
        };
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (!strcmp(args[0], names[i].text)) result.kind = names[i].kind;
        }
    } else if ((count == 2 || count == 3) && !strcmp(args[0], "AT+CONTACTS")) {
        if (!strcmp(args[1], "ALL")) result.value = 0;
        else if (!strcmp(args[1], "DEV")) result.value = 1;
        else if (!strcmp(args[1], "WX")) result.value = 2;
        else return result;
        if (count == 3 && !page_number(args[2], &result.page)) return (xmini_command_t){0};
        result.kind = XMINI_CMD_CONTACTS_PAGE;
    } else if (count == 2 && !strcmp(args[0], "AT+PENDING")) {
        if (page_number(args[1], &result.page)) result.kind = XMINI_CMD_PENDING_PAGE;
    } else if (count == 2) {
        if (!strcmp(args[0], "AT+GUIDANCE") || !strcmp(args[0], "AT+ECHO")) {
            if (!strcmp(args[1], "0") || !strcmp(args[1], "1")) {
                result.kind = !strcmp(args[0], "AT+ECHO") ? XMINI_CMD_ECHO_SET : XMINI_CMD_GUIDANCE_SET;
                result.value = args[1][0] - '0';
            }
        } else if (!strcmp(args[0], "AT+WIFI") && !strcmp(args[1], "SETUP")) {
            result.kind = XMINI_CMD_WIFI_SETUP;
        } else if (!strcmp(args[0], "AT+WIFI") && !strcmp(args[1], "CANCEL")) {
            result.kind = XMINI_CMD_WIFI_CANCEL;
        } else if (!strcmp(args[0], "AT+MEDIA") && !strcmp(args[1], "TONE")) {
            result.kind = XMINI_CMD_MEDIA_TONE;
        } else if (!strcmp(args[0], "AT+GROUP")) {
            static const struct { const char *text; xmini_command_kind_t kind; } names[] = {
                {"REFRESH", XMINI_CMD_GROUP_REFRESH}, {"CREATE", XMINI_CMD_GROUP_CREATE},
                {"CONNECT", XMINI_CMD_GROUP_CONNECT}, {"DISCONNECT", XMINI_CMD_GROUP_DISCONNECT},
                {"LEAVE", XMINI_CMD_GROUP_LEAVE}, {"PTT_DOWN", XMINI_CMD_GROUP_DOWN}, {"PTT_UP", XMINI_CMD_GROUP_UP},
            };
            for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
                if (!strcmp(args[1], names[i].text)) result.kind = names[i].kind;
        } else if (!strcmp(args[0], "AT+AI")) {
            if (!strcmp(args[1], "START")) result.kind = XMINI_CMD_AI_START;
            else if (!strcmp(args[1], "STOP")) result.kind = XMINI_CMD_HANGUP;
            else if (!strcmp(args[1], "INTERRUPT")) result.kind = XMINI_CMD_AI_INTERRUPT;
            else if (!strcmp(args[1], "SUBMIT")) result.kind = XMINI_CMD_AI_SUBMIT;
        } else if (!strcmp(args[0], "AT+CALL")) {
            if (!strcmp(args[1], "ACCEPT")) result.kind = XMINI_CMD_ACCEPT;
            else if (!strcmp(args[1], "REJECT")) result.kind = XMINI_CMD_REJECT;
            else if (!strcmp(args[1], "HANGUP")) result.kind = XMINI_CMD_HANGUP;
            else if (!strcmp(args[1], "DISMISS")) result.kind = XMINI_CMD_DISMISS;
        } else if (!strcmp(args[0], "AT+DIAG") && !strcmp(args[1], "USB_TICK")) {
            result.kind = XMINI_CMD_USB_PROBE;
        } else if (!strcmp(args[0], "AT+AUDIO")) {
            if (!strcmp(args[1], "TONE")) result.kind = XMINI_CMD_TONE;
            else if (!strcmp(args[1], "RECORD")) result.kind = XMINI_CMD_RECORD;
            else if (!strcmp(args[1], "PLAY")) result.kind = XMINI_CMD_PLAY;
            else if (!strcmp(args[1], "STOP")) result.kind = XMINI_CMD_STOP;
            else if (!strcmp(args[1], "DUMP")) result.kind = XMINI_CMD_AUDIO_DUMP;
        } else if (!strcmp(args[0], "AT+KEY")) {
            if (!strcmp(args[1], "CLICK")) result.kind = XMINI_CMD_CLICK;
            else if (!strcmp(args[1], "DOUBLE")) result.kind = XMINI_CMD_DOUBLE;
            else if (!strcmp(args[1], "HOLD")) result.kind = XMINI_CMD_HOLD;
        } else if (!strcmp(args[0], "AT+BIND") && !strcmp(args[1], "REPLAY")) {
            result.kind = XMINI_CMD_BIND_REPLAY;
        } else if (!strcmp(args[0], "AT+GUIDE")) {
            unsigned value = 0;
            for (const char *p = args[1]; *p; ++p) {
                if (*p < '0' || *p > '9' || value > 12) return result;
                value = value * 10 + (unsigned)(*p - '0');
            }
            if (value > 12 || value == 5) return result;
            result.kind = XMINI_CMD_GUIDE;
            result.value = (int)value;
        } else if (!strcmp(args[0], "AT+VOL") || !strcmp(args[0], "AT+MIC")) {
            int value = 0;
            for (const char *p = args[1]; *p; ++p) {
                if (*p < '0' || *p > '9' || value > 100) return result;
                value = value * 10 + *p - '0';
            }
            int limit = !strcmp(args[0], "AT+VOL") ? 100 : 36;
            if (value > limit) return result;
            result.kind = limit == 100 ? XMINI_CMD_VOLUME : XMINI_CMD_GAIN;
            result.value = value;
        }
    } else if ((count == 3 || count == 4) && !strcmp(args[0], "AT+GROUP") && !strcmp(args[1], "JOIN")) {
        if (!digits(args[2], 6) || (count == 4 && !digits(args[3], 4))) return result;
        result.kind = XMINI_CMD_GROUP_JOIN;
        memcpy(result.code, args[2], sizeof(result.code));
        if (count == 4) strcpy(result.password, args[3]);
    } else if (count == 3 && !strcmp(args[0], "AT+GROUP") && !strcmp(args[1], "CREATE")) {
        if (!digits(args[2], 4)) return result;
        result.kind = XMINI_CMD_GROUP_CREATE;
        strcpy(result.password, args[2]);
    } else if (count == 3 && !strcmp(args[0], "AT+CALL") &&
               (!strcmp(args[1], "DEVICE") || !strcmp(args[1], "WX"))) {
        if (!identifier(args[2])) return result;
        strcpy(result.target, args[2]);
        result.kind = !strcmp(args[1], "DEVICE") ? XMINI_CMD_DEVICE_CALL : XMINI_CMD_WECHAT_CALL;
    } else if ((count == 3 || count == 4) && !strcmp(args[0], "AT+CONTACT")) {
        if (!identifier(args[2])) return result;
        if (count == 4 && !strcmp(args[1], "REMARK")) {
            if (strcmp(args[3], "-") && !decode_hex(args[3], strlen(args[3]), result.remark, sizeof(result.remark)))
                return result;
            result.kind = XMINI_CMD_CONTACT_REMARK;
        } else if (count == 3) {
            if (!strcmp(args[1], "REQUEST")) result.kind = XMINI_CMD_CONTACT_REQUEST;
            else if (!strcmp(args[1], "ACCEPT")) result.kind = XMINI_CMD_CONTACT_ACCEPT;
            else if (!strcmp(args[1], "REJECT")) result.kind = XMINI_CMD_CONTACT_REJECT;
            else if (!strcmp(args[1], "DELETE")) result.kind = XMINI_CMD_CONTACT_DELETE;
        }
        if (result.kind) strcpy(result.target, args[2]);
    } else if (count == 3 && !strcmp(args[0], "AT+GUIDE") && !strcmp(args[1], "5") && strlen(args[2]) == 6) {
        for (unsigned i = 0; i < 6; ++i)
            if (args[2][i] < '0' || args[2][i] > '9') return result;
        result.kind = XMINI_CMD_GUIDE;
        result.value = 5;
        memcpy(result.code, args[2], sizeof(result.code));
    }
    return result;
}
