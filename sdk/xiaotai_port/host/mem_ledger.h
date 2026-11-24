// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai_mem_host.h"

#define XIAOTAI_LEDGER_CALLER ((uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)))

void xiaotai_mem_host_task_open(const void *task, const char *name, xiaotai_mem_kind_t kind,
                                size_t bytes, uintptr_t site);
void xiaotai_mem_host_task_close(const void *task);
void *xiaotai_mem_host_fixture_alloc(xiaotai_mem_kind_t kind, size_t size);
void *xiaotai_mem_host_fixture_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
