// SPDX-License-Identifier: MIT
#include "xiaotai/port/os.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

static bool s_external;

bool __wrap_xiaotai_mem_has_external(void) { return s_external; }

typedef struct {
    xiaotai_signal_t run;
    xiaotai_signal_t done;
    xiaotai_err_t result;
} probe_t;

static void probe(void *arg)
{
    probe_t *p = arg;
    if (!xiaotai_signal_take(&p->run, 5000)) return;
    p->result = xiaotai_require_internal_stack("flash_probe");
    xiaotai_signal_give(&p->done);
}

static int run_probe(xiaotai_mem_kind_t kind, bool external, xiaotai_err_t expected)
{
    probe_t p = {0};
    xiaotai_task_t task;
    const xiaotai_task_config_t config = {
        .name = "stack_probe", .entry = probe, .arg = &p, .stack_bytes = 4096,
        .stack = kind, .priority = 1, .core = XIAOTAI_CORE_ANY,
    };
    CHECK(xiaotai_signal_init(&p.run) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&p.done) == XIAOTAI_OK);
    s_external = external;
    CHECK(xiaotai_task_create(&config, &task) == XIAOTAI_OK);
    /* Model the stack selected when the task was created, not a subsequent query. */
    s_external = !external;
    xiaotai_signal_give(&p.run);
    CHECK(xiaotai_signal_take(&p.done, 5000));
    CHECK(p.result == expected);
    xiaotai_signal_deinit(&p.done);
    xiaotai_signal_deinit(&p.run);
    return 0;
}

int main(void)
{
    /* Do the death test before creating any threads in the parent. */
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        const struct rlimit no_core = {0, 0};
        if (setrlimit(RLIMIT_CORE, &no_core) != 0 || setenv("XIAOTAI_HOST_STACK_STRICT", "1", 1) != 0) _Exit(2);
#ifdef __linux__
        /* A piped core handler (such as WSL's capture) can ignore the size limit. */
        if (prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) != 0) _Exit(2);
#endif
        _Exit(run_probe(XIAOTAI_MEM_BULK, true, XIAOTAI_ERR_INVALID_STATE));
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    CHECK(unsetenv("XIAOTAI_HOST_STACK_STRICT") == 0);
    CHECK(xiaotai_require_internal_stack("main_probe") == XIAOTAI_OK);
    CHECK(run_probe(XIAOTAI_MEM_INTERNAL, true, XIAOTAI_OK) == 0);
    CHECK(run_probe(XIAOTAI_MEM_BULK, false, XIAOTAI_OK) == 0);
    CHECK(run_probe(XIAOTAI_MEM_BULK, true, XIAOTAI_ERR_INVALID_STATE) == 0);
    return 0;
}
