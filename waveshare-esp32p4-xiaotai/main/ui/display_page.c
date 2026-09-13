#include "display_page.h"

static lv_obj_t *display_page_entry_obj(const display_page_entry_t *entry)
{
    if (entry == NULL || entry->page == NULL) {
        return NULL;
    }

    return *entry->page;
}

void display_page_registry_init(display_page_registry_t *registry,
                                const display_page_entry_t *entries,
                                size_t count)
{
    if (registry == NULL) {
        return;
    }

    registry->entries = entries;
    registry->count = count;
    registry->current = NULL;
}

void display_page_registry_hide_all(display_page_registry_t *registry)
{
    if (registry == NULL || registry->entries == NULL) {
        return;
    }

    for (size_t index = 0; index < registry->count; ++index) {
        lv_obj_t *page = display_page_entry_obj(&registry->entries[index]);
        if (page != NULL) {
            lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
        }
    }
    registry->current = NULL;
}

void display_page_registry_show(display_page_registry_t *registry, lv_obj_t *page)
{
    if (registry == NULL) {
        return;
    }

    display_page_registry_hide_all(registry);
    if (page != NULL) {
        lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
    }
    registry->current = page;
}

bool display_page_registry_is_visible(const display_page_registry_t *registry, lv_obj_t *page)
{
    if (page == NULL) {
        return false;
    }

    if (registry != NULL && registry->current != NULL) {
        return page == registry->current && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN);
    }

    return !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *display_page_registry_current(const display_page_registry_t *registry)
{
    return registry != NULL ? registry->current : NULL;
}
