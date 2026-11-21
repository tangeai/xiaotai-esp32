// SPDX-License-Identifier: MIT
#include "fakes.h"

static bool valid_filter(int32_t arg) { return arg >= 0 && arg <= XIAOTAI_APP_COMPANION_CONTACTS_WECHAT; }

static const char *entry_title(int32_t arg)
{
    switch (arg) {
    case XIAOTAI_APP_COMPANION_CONTACTS_DEVICE: return "Device contacts";
    case XIAOTAI_APP_COMPANION_CONTACTS_WECHAT: return "WeChat contacts";
    default: return "Contacts";
    }
}

static void contact(xiaotai_view_t *view, int32_t arg)
{
    view->kind = XIAOTAI_VIEW_NOTICE;
    view->text = entry_title(arg);
}

static xiaotai_err_t pages(void)
{
    static const companion_page_t contacts = {.id = "contacts", .title = "Contacts",
        .arg_valid = valid_filter, .entry_title = entry_title, .build = contact};
    static const companion_page_t detail = {.id = "contact", .title = "Detail", .build = contact};
    xiaotai_err_t err = companion_page_register(&contacts);
    if (err == XIAOTAI_OK) err = companion_page_register(&detail);
    return err;
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_contacts = {.name = "contacts", .init = pages};
