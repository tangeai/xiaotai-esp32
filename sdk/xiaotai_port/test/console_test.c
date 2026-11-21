// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "xiaotai/port/console.h"
#include "xiaotai/port/os.h"
#include "xiaotai_console_host.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void *feed_later(void *unused)
{
    (void)unused;
    xiaotai_delay_ms(4);
    assert(xiaotai_console_host_feed("\nq", 2) == 2);
    return NULL;
}

static void input(void)
{
    char raw[24];
    assert(!xiaotai_console_read(raw, sizeof(raw), 0));
    assert(xiaotai_console_host_feed("a\r", 2) == 2);
    assert(xiaotai_console_read(raw, sizeof(raw), 0) == 2 && !memcmp(raw, "a\n", 2));
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, feed_later, NULL));
    assert(xiaotai_console_read(raw, 1, 1000) == 1 && raw[0] == 'q');
    assert(!pthread_join(producer, NULL));
    assert(xiaotai_console_host_feed("ab\r\ncd\ndef", 10) == 8);
    assert(xiaotai_console_read(raw, sizeof(raw), 0) == 7 && !memcmp(raw, "ab\ncd\nd", 7));
    assert(!xiaotai_console_read(raw, sizeof(raw), 1));
    xiaotai_console_stats_t st;
    xiaotai_console_stats(&st);
    assert(st.rx_bytes == 12 && st.rx_overflows == 2);
}

static void output(void)
{
    char got[512];
    assert(xiaotai_console_write("A\r\nB\nC", 6) == XIAOTAI_OK);
    assert(xiaotai_console_host_take(got, sizeof(got)) == 7 && !memcmp(got, "A\r\nB\r\nC", 7));
    char text[456];
    memset(text, 'k', sizeof(text));
    text[223] = '\n';
    text[447] = '\n';
    assert(xiaotai_console_write(text, sizeof(text)) == XIAOTAI_OK);
    assert(xiaotai_console_host_take(got, sizeof(got)) == 458);
    assert(!memcmp(got + 223, "\r\n", 2) && !memcmp(got + 448, "\r\n", 2));
    xiaotai_console_stats_t base, st;
    xiaotai_console_stats(&base);
    xiaotai_console_host_connected(false);
    assert(xiaotai_console_write("\nabc", 4) == XIAOTAI_FAIL);
    xiaotai_console_stats(&st);
    assert(!st.connected && st.tx_dropped == base.tx_dropped + 5 && st.tx_timeouts == base.tx_timeouts);
    xiaotai_console_host_connected(true);
    xiaotai_console_host_write_budget(226);
    assert(xiaotai_console_write(text, sizeof(text)) == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_console_host_take(got, sizeof(got)) == 226);
    xiaotai_console_stats(&st);
    assert(st.connected && st.tx_dropped == base.tx_dropped + 5 + 458 - 226 &&
           st.tx_timeouts == base.tx_timeouts + 1 && st.tx_bytes == base.tx_bytes + 226);
    xiaotai_console_host_write_budget(SIZE_MAX);
}

static atomic_bool attempting, log_taken;
static void *log_writer(void *unused)
{
    (void)unused;
    atomic_store(&attempting, true);
    flockfile(stdout);
    atomic_store(&log_taken, true);
    funlockfile(stdout);
    return NULL;
}

static void *parallel_record(void *token)
{
    const char *value = token;
    for (unsigned i = 0; i < 30; ++i) {
        xiaotai_console_record_begin();
        assert(xiaotai_console_write(value, 1) == XIAOTAI_OK);
        xiaotai_delay_ms(1);
        assert(xiaotai_console_write(value, 1) == XIAOTAI_OK);
        assert(xiaotai_console_write("\r", 1) == XIAOTAI_OK);
        assert(xiaotai_console_write("\n", 1) == XIAOTAI_OK);
        xiaotai_console_record_end();
    }
    return NULL;
}

static void atomic_records(void)
{
    pthread_t log, a, b;
    xiaotai_console_record_begin();
    assert(!pthread_create(&log, NULL, log_writer, NULL));
    while (!atomic_load(&attempting)) xiaotai_delay_ms(1);
    assert(!atomic_load(&log_taken));
    assert(xiaotai_console_write("one", 3) == XIAOTAI_OK);
    assert(xiaotai_console_write("two", 3) == XIAOTAI_OK && !atomic_load(&log_taken));
    xiaotai_console_record_end();
    assert(!pthread_join(log, NULL) && atomic_load(&log_taken));
    assert(!pthread_create(&a, NULL, parallel_record, "a"));
    assert(!pthread_create(&b, NULL, parallel_record, "b"));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    char got[512];
    const size_t n = xiaotai_console_host_take(got, sizeof(got));
    assert(n == 6 + 60 * 4 && !memcmp(got, "onetwo", 6));
    unsigned as = 0, bs = 0;
    for (size_t i = 6; i < n; i += 4) {
        assert(got[i] == got[i + 1] && got[i + 2] == '\r' && got[i + 3] == '\n');
        as += got[i] == 'a';
        bs += got[i] == 'b';
    }
    assert(as == 30 && bs == 30);
}

static void actual_pipe(void)
{
    int output_end[2], input_end[2];
    assert(!pipe(output_end) && !pipe(input_end));
    assert(dup2(output_end[1], STDOUT_FILENO) == STDOUT_FILENO);
    assert(dup2(input_end[0], STDIN_FILENO) == STDIN_FILENO);
    close(output_end[1]);
    close(input_end[0]);
    assert(xiaotai_console_open(NULL) == XIAOTAI_OK);
    assert(xiaotai_console_write("value\n", 6) == XIAOTAI_OK);
    char bytes[20];
    assert(read(output_end[0], bytes, sizeof(bytes)) == 7 && !memcmp(bytes, "value\r\n", 7));
    assert(write(input_end[1], "ready\r\n", 7) == 7);
    assert(xiaotai_console_read(bytes, sizeof(bytes), 100) == 6 && !memcmp(bytes, "ready\n", 6));
    close(input_end[1]);
    const int flags = fcntl(STDOUT_FILENO, F_GETFL);
    assert(flags >= 0 && !fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK));
    char filler[512] = {0};
    while (write(STDOUT_FILENO, filler, sizeof(filler)) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(!fcntl(STDOUT_FILENO, F_SETFL, flags));
    assert(xiaotai_console_write("blocked", 7) == XIAOTAI_ERR_TIMEOUT);
    assert(fcntl(STDOUT_FILENO, F_GETFL) == flags);
    close(output_end[0]);
    assert(xiaotai_console_write("closed", 6) == XIAOTAI_FAIL);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "stdio")) {
        actual_pipe();
        return 0;
    }
    xiaotai_console_host_pipe();
    assert(xiaotai_console_write("closed", 6) == XIAOTAI_ERR_NOT_SUPPORTED);
    xiaotai_console_config_t cfg = {.rx_buffer_bytes = 8};
    assert(xiaotai_console_open(&cfg) == XIAOTAI_OK);
    assert(xiaotai_console_open(NULL) == XIAOTAI_OK); /* Keeps the eight-byte ring. */
    xiaotai_console_info_t info;
    xiaotai_console_info(&info);
    assert(info.present && !info.baud_rate && !strcmp(info.device, "stdio"));
    input();
    output();
    atomic_records();
    xiaotai_console_bulk(true);
    xiaotai_console_bulk(false);
}
