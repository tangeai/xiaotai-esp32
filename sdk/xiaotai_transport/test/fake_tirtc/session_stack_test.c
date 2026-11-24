// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

extern void *rtc_thread_create(uint32_t bytes);
void *fixture_rtc_thread_create(uint32_t bytes);
static bool external_ram, fail;
static uint32_t received;
static unsigned calls;
static void *volatile result;
static char thread;

bool xiaotai_mem_has_external(void) { return external_ram; }

void *fixture_rtc_thread_create(uint32_t bytes)
{
    received = bytes;
    ++calls;
    return fail ? NULL : &thread;
}

__attribute__((noinline, section("fixture_in")))
static void inbound(uint32_t bytes) { result = rtc_thread_create(bytes); }

__attribute__((noinline, section("fixture_out")))
static void outbound(uint32_t bytes) { result = rtc_thread_create(bytes); }

__attribute__((noinline))
static void ordinary(uint32_t bytes) { result = rtc_thread_create(bytes); }

int main(void)
{
    inbound(8192);
    assert(received == 9216 && calls == 1 && result == &thread);
    outbound(8192);
    assert(received == 9216 && calls == 2 && result == &thread);
    ordinary(8192);
    assert(received == 8192 && calls == 3);
    inbound(4096);
    assert(received == 4096 && calls == 4);
    outbound(10240);
    assert(received == 10240 && calls == 5);
    external_ram = true;
    outbound(8192);
    assert(received == 8192 && calls == 6);
    external_ram = false;
    fail = true;
    outbound(8192);
    assert(received == 9216 && calls == 7 && result == NULL);
    return 0;
}
