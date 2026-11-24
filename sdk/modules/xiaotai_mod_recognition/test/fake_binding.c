// SPDX-License-Identifier: MIT
#include "operation_fixture.h"

#include <string.h>

static xiaotai_binding_prepare_fn_t preparation;
static void *preparation_ctx;

xiaotai_err_t xiaotai_binding_prepare_register(const char *name, xiaotai_binding_prepare_fn_t prepare, void *ctx)
{
    CHECK(strcmp(name, "recognition") == 0 && prepare && !preparation);
    preparation = prepare;
    preparation_ctx = ctx;
    return XIAOTAI_OK;
}

xiaotai_err_t fake_binding_prepare(xiaotai_binding_prepared_fn_t done, void *ctx)
{
    CHECK(preparation);
    return preparation(done, ctx, preparation_ctx);
}
