// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "console_internal.h"
#include "xiaotai_console_host.h"
#include "xiaotai/port/os.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t s_pipe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_input = PTHREAD_COND_INITIALIZER;
static bool s_pipe, s_connected = true;
static char s_rx[UINT16_MAX], s_out[16384];
static size_t s_rx_capacity, s_rx_size, s_rx_head, s_out_size, s_budget = SIZE_MAX;

void console_io_lock(void) { flockfile(stdout); }
void console_io_unlock(void) { funlockfile(stdout); }
void xiaotai_console_bulk(bool active) { (void)active; }

xiaotai_err_t console_io_open(size_t rx)
{
    s_rx_capacity = rx;
    s_rx_head = s_rx_size = 0;
    return XIAOTAI_OK;
}

void xiaotai_console_info(xiaotai_console_info_t *info)
{
    if (info) *info = (xiaotai_console_info_t){.device = "stdio", .present = true};
}

bool console_io_connected(void)
{
    pthread_mutex_lock(&s_pipe_lock);
    const bool connected = !s_pipe || s_connected;
    pthread_mutex_unlock(&s_pipe_lock);
    return connected;
}

static struct timespec wait_until(uint32_t ms)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        ++t.tv_sec;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}

static int wait_fd(int fd, short events, uint32_t ms)
{
    struct pollfd watch = {.fd = fd, .events = events};
    const int64_t now = xiaotai_time_us(), budget = (int64_t)ms * 1000;
    const int64_t deadline = now > INT64_MAX - budget ? INT64_MAX : now + budget;
    int wait = ms > INT_MAX ? INT_MAX : (int)ms;
    for (;;) {
        const int ready = poll(&watch, 1, wait);
        if (ready > 0) return watch.revents & events ? 1 : -1;
        if (ready == 0 || errno != EINTR) return ready;
        const int64_t left = deadline - xiaotai_time_us();
        if (left <= 0) return 0;
        wait = left / 1000 > INT_MAX ? INT_MAX : (int)((left + 999) / 1000);
    }
}

size_t console_io_read(char *data, size_t size, uint32_t ms)
{
    if (!s_pipe) {
        if (wait_fd(STDIN_FILENO, POLLIN, ms) != 1) return 0;
        const ssize_t n = read(STDIN_FILENO, data, size);
        return n > 0 ? (size_t)n : 0;
    }
    pthread_mutex_lock(&s_pipe_lock);
    if (!s_rx_size && ms) {
        const struct timespec until = wait_until(ms);
        while (!s_rx_size && pthread_cond_timedwait(&s_input, &s_pipe_lock, &until) == 0) {}
    }
    const size_t n = s_rx_size < size ? s_rx_size : size;
    for (size_t i = 0; i < n; ++i) {
        data[i] = s_rx[s_rx_head++];
        if (s_rx_head == s_rx_capacity) s_rx_head = 0;
    }
    s_rx_size -= n;
    pthread_mutex_unlock(&s_pipe_lock);
    return n;
}

static ssize_t write_without_sigpipe(const char *data, size_t size)
{
    sigset_t blocked, restore, pending;
    sigemptyset(&blocked);
    sigemptyset(&pending);
    sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &restore) != 0) return -1;
    (void)sigpending(&pending);
    const bool was_pending = sigismember(&pending, SIGPIPE) == 1;
    const ssize_t sent = data ? write(STDOUT_FILENO, data, size) : fflush(stdout);
    const int saved_errno = errno;
    if (sent < 0 && saved_errno == EPIPE && !was_pending) {
        const struct timespec immediate = {0};
        while (sigtimedwait(&blocked, NULL, &immediate) < 0 && errno == EINTR) {}
    }
    (void)pthread_sigmask(SIG_SETMASK, &restore, NULL);
    errno = saved_errno;
    return sent;
}

xiaotai_err_t console_io_flush(void)
{
    if (s_pipe) return XIAOTAI_OK;
    const int flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return XIAOTAI_FAIL;
    const bool ok = write_without_sigpipe(NULL, 0) == 0;
    const int error = errno;
    if (fcntl(STDOUT_FILENO, F_SETFL, flags) < 0) return XIAOTAI_FAIL;
    if (ok) return XIAOTAI_OK;
    return error == EAGAIN || error == EWOULDBLOCK ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_FAIL;
}

xiaotai_err_t console_io_write(const char *data, size_t size, size_t *accepted)
{
    *accepted = 0;
    if (s_pipe) {
        pthread_mutex_lock(&s_pipe_lock);
        const bool connected = s_connected;
        if (connected) {
            size_t actual = sizeof(s_out) - s_out_size;
            if (actual > s_budget) actual = s_budget;
            if (actual > size) actual = size;
            memcpy(s_out + s_out_size, data, actual);
            s_out_size += actual;
            *accepted = actual;
            if (s_budget != SIZE_MAX) s_budget -= actual;
        }
        pthread_mutex_unlock(&s_pipe_lock);
        if (!connected) return XIAOTAI_FAIL;
        return *accepted == size ? XIAOTAI_OK : XIAOTAI_ERR_TIMEOUT;
    }
    const int flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return XIAOTAI_FAIL;
    xiaotai_err_t err = XIAOTAI_OK;
    while (*accepted < size && err == XIAOTAI_OK) {
        const int ready = wait_fd(STDOUT_FILENO, POLLOUT, XIAOTAI_CONSOLE_WRITE_TIMEOUT_MS);
        if (ready < 0) err = XIAOTAI_FAIL;
        else if (!ready) err = XIAOTAI_ERR_TIMEOUT;
        else {
            const ssize_t sent = write_without_sigpipe(data + *accepted, size - *accepted);
            if (sent > 0) *accepted += (size_t)sent;
            else err = sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_FAIL;
        }
    }
    if (fcntl(STDOUT_FILENO, F_SETFL, flags) < 0 && err == XIAOTAI_OK) err = XIAOTAI_FAIL;
    return err;
}

void xiaotai_console_host_pipe(void)
{
    s_pipe = s_connected = true;
    s_budget = SIZE_MAX;
    s_out_size = 0;
}

size_t xiaotai_console_host_feed(const char *bytes, size_t size)
{
    if (!s_pipe || !bytes || !s_rx_capacity) return 0;
    pthread_mutex_lock(&s_pipe_lock);
    size_t n = s_rx_capacity - s_rx_size;
    if (n > size) n = size;
    const size_t end = (s_rx_head + s_rx_size) % s_rx_capacity;
    for (size_t i = 0; i < n; ++i) s_rx[(end + i) % s_rx_capacity] = bytes[i];
    s_rx_size += n;
    if (n) pthread_cond_signal(&s_input);
    console_note_rx_overflow(size - n);
    pthread_mutex_unlock(&s_pipe_lock);
    return n;
}

size_t xiaotai_console_host_take(char *data, size_t capacity)
{
    if (!s_pipe || !data || !capacity) return 0;
    pthread_mutex_lock(&s_pipe_lock);
    const size_t n = s_out_size < capacity ? s_out_size : capacity;
    memcpy(data, s_out, n);
    s_out_size -= n;
    memmove(s_out, s_out + n, s_out_size);
    pthread_mutex_unlock(&s_pipe_lock);
    return n;
}

void xiaotai_console_host_connected(bool connected)
{
    pthread_mutex_lock(&s_pipe_lock);
    s_connected = connected;
    pthread_mutex_unlock(&s_pipe_lock);
}

void xiaotai_console_host_write_budget(size_t budget)
{
    pthread_mutex_lock(&s_pipe_lock);
    s_budget = budget;
    pthread_mutex_unlock(&s_pipe_lock);
}
