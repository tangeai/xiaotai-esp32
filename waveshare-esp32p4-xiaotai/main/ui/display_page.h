#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

typedef struct {
    const char *name;
    lv_obj_t **page;
} display_page_entry_t;

typedef struct {
    const display_page_entry_t *entries;
    size_t count;
    lv_obj_t *current;
} display_page_registry_t;

void display_page_registry_init(display_page_registry_t *registry,
                                const display_page_entry_t *entries,
                                size_t count);
void display_page_registry_hide_all(display_page_registry_t *registry);
void display_page_registry_show(display_page_registry_t *registry, lv_obj_t *page);
bool display_page_registry_is_visible(const display_page_registry_t *registry, lv_obj_t *page);
lv_obj_t *display_page_registry_current(const display_page_registry_t *registry);
