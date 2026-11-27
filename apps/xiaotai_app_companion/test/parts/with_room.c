// SPDX-License-Identifier: MIT
#include "fakes.h"

static void room_view(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    view->kind = XIAOTAI_VIEW_LIST;
}

static xiaotai_err_t init(void)
{
    static const companion_page_t room = {.id = "room", .title = "Group room", .build = room_view};
    return companion_page_register(&room);
}

static xiaotai_err_t on_main(int32_t action)
{
    ++fixture.main_requests;
    fixture.main_arg = (unsigned)action;
    return XIAOTAI_OK;
}

static bool preparing(void) { return fixture.rtc_preparing; }

const xiaotai_app_companion_part_t xiaotai_app_companion_with_room = {
    .name = "room", .init = init, .main = on_main, .service_preparing = preparing,
};
