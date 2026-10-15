#include "rtc_adapter.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *saved_peer, *saved_token;
static unsigned submits;
int TiRtcWhipConnect(const char *peer, const char *token, TIRTCCONNECTCALLBACK callback, void *user)
{
    assert(callback && user == (void *)(uintptr_t)7);
    saved_peer = peer;
    saved_token = token;
    ++submits;
    return 0;
}
int TiRtcConnSetUserData(tirtc_conn_t h, void *user)
{
    (void)h;
    (void)user;
    return 0;
}
void test_log(const char *level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    (void)format;
}
int main(void)
{
    char *peer = strdup("whips://test?room=sample"), *token = strdup("sample-token");
    assert(peer && token);
    assert(rtc_connect(7, peer, token) == 0);
    assert(saved_peer != peer && saved_token != token);
    memset(peer, 'x', strlen(peer));
    memset(token, 'x', strlen(token));
    free(peer);
    free(token);
    assert(strcmp(saved_peer, "whips://test?room=sample") == 0);
    assert(strcmp(saved_token, "sample-token") == 0);
    assert(rtc_connect(7, "whips://busy", "busy-token") == TIRTC_E_BUSY);
    char oversized[2049];
    memset(oversized, 'a', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = 0;
    assert(rtc_connect(7, oversized, "test") == TIRTC_E_INVALID_PARAMETER);
    assert(rtc_connect(7, NULL, "test") == TIRTC_E_INVALID_PARAMETER);
    assert(submits == 1 && strcmp(saved_token, "sample-token") == 0);
    rtc_connect_inputs_release();
    assert(rtc_connect(7, "whips://second", "second-token") == 0);
    assert(submits == 2 && strcmp(saved_peer, "whips://second") == 0);
    rtc_connect_inputs_release();
    puts("RTC input ownership tests passed");
}
