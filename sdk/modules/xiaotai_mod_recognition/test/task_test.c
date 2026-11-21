// SPDX-License-Identifier: MIT
#include "recognition_protocol.h"
#include "xiaotai/port/mem.h"
#include "xiaotai_mem_host.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int budget;

void __real_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *out);

void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *out)
{
    if (!budget) {
        __real_xiaotai_mem_stats(kind, out);
        return;
    }
    assert(kind == XIAOTAI_MEM_BULK);
    *out = (xiaotai_mem_stats_t){.total = 1024U * 1024U, .free = 300U * 1024U, .largest_block = 300U * 1024U};
    if (budget == -1) out->free = 256U * 1024U;  /* Nothing left after the reserve. */
    if (budget == -2) out->largest_block = 128;  /* Enough total space, not contiguous. */
    if (out->largest_block > out->free) out->largest_block = out->free;
}

int main(int argc, char **argv)
{
    assert(argc == 8);
    FILE *file = fopen(argv[1], "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long end = ftell(file);
    assert(end > 0 && fseek(file, 0, SEEK_SET) == 0);
    const size_t length = (size_t)end;
    char *body = malloc(length);
    assert(body && fread(body, 1, length, file) == length && fclose(file) == 0);
    const int status = atoi(argv[2]), expected = atoi(argv[3]), code_expected = atoi(argv[4]);
    const int inject = atoi(argv[5]);
    budget = inject < 0 ? inject : 0;
    int code = -1;
    uint64_t revision = 0;
    rc_task_t *task = NULL;
    const uint32_t allocations = xiaotai_mem_host_allocations();
    xiaotai_mem_host_fail_nth(inject > 0 ? (unsigned)inject : 0);
    xiaotai_err_t error = argc == 8 && !strcmp(argv[6], "notification")
        ? rc_notification_decode(body, length, "fixture-device", argv[7], &revision, &task)
        : rc_task_decode(body, length, status, "fixture-device", argv[7], &code, &task);
    free(body);  /* The snapshot, when returned, is now the only source of its text. */
    if (error != expected || code != code_expected) {
        fprintf(stderr, "error=0x%x code=%d expected=0x%x/%d\n", error, code, expected, code_expected);
        return 1;
    }
    if (budget == -1 || budget == -2) {
        assert(error == XIAOTAI_ERR_NO_MEM && xiaotai_mem_host_allocations() == allocations);
    }
    if (!task) {
        assert(error != XIAOTAI_OK || revision != 0);
        printf("revision=%llu\n", (unsigned long long)revision);
    } else {
        assert(error == XIAOTAI_OK && !revision && !strcmp(task->info.request_id, argv[7]));
        assert(task->info.items >= 1 && task->info.items <= 8);
        assert(task->length == strlen(task->body));
        printf("%llu %u %u\n", (unsigned long long)task->info.revision, task->info.items, (unsigned)task->info.state);
        for (size_t i = 0; i < task->info.items; ++i) {
            const rc_item_t *item = &task->items[i];
            char id[XIAOTAI_RECOGNITION_ID_MAX], summary[XIAOTAI_RECOGNITION_SUMMARY_MAX];
            assert(rc_task_string(task, item->id, id, sizeof(id), NULL) == XIAOTAI_OK);
            assert(rc_id_valid(id));
            printf("%s %u %u %u %d %d\n", id, item->state, item->output, item->speech, item->error_code, item->speech_error_code);
            if (item->result.size) {
                assert(task->body[item->result.start] == '{');
                assert(task->body[item->result.start + item->result.size - 1] == '}');
                size_t summary_bytes;
                assert(rc_task_string(task, item->summary, summary, 1, &summary_bytes) == XIAOTAI_ERR_INVALID_SIZE);
                assert(rc_task_string(task, item->summary, summary, sizeof(summary), &summary_bytes) == XIAOTAI_OK);
                printf("%zu:%s\n", summary_bytes, summary);
            } else {
                assert(rc_task_string(task, item->summary, summary, sizeof(summary), NULL) == XIAOTAI_ERR_NOT_FOUND);
            }
        }
        rc_task_free(task);
    }
    assert(!xiaotai_mem_host_outstanding());
}
