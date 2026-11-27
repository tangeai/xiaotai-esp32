// SPDX-License-Identifier: MIT
/* Every mode a page of its own (xiaotai_app_door_config_t.mode_pages). The
 * four home pages follow one another when paging; each keeps its own
 * counterparts, the four lists in one record. The page that stays in front
 * for XIAOTAI_APP_DOOR_MODE_SETTLE_MS, or is used, becomes the stored mode:
 * the one the device answers in from any page. Pages only paged through leave
 * it as it was. */
#include "door_internal.h"

#include "xiaotai/modules/settings.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#include <string.h>

#define PAGES_SPACE "xt_door"
#define PAGES_KEY "pages"
#define PAGES_VERSION 1U
#define MODES 4U
/* The version, then for every mode in the enum's order a count and as many
 * ids, each a length and its bytes. */
#define RECORD_MAX (1U + MODES * (1U + XIAOTAI_APP_DOOR_PEERS_MAX * XIAOTAI_SETTING_TEXT_MAX))

/* Paging forward from the application before: the access machine, the access
 * system's indoor station, the doorbell, the doorbell's indoor station; then
 * the application after. */
static const xiaotai_app_door_mode_t order[MODES] = {
    XIAOTAI_APP_DOOR_ACCESS, XIAOTAI_APP_DOOR_INDOOR, XIAOTAI_APP_DOOR_BELL, XIAOTAI_APP_DOOR_BELL_INDOOR,
};

typedef struct {
    door_peer_id_t lists[MODES][XIAOTAI_APP_DOOR_PEERS_MAX];   /* by mode */
    uint8_t record[RECORD_MAX];
    uint32_t timer;
} pages_t;
static pages_t *pages;      /* from init on, with mode_pages */

bool door_pages_on(void) { return pages != NULL; }

xiaotai_err_t door_pages_init(void)
{
    if (!door_config.mode_pages) return XIAOTAI_OK;
    pages = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*pages));
    return pages ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

static void cancel(void)
{
    if (!pages || !pages->timer) return;
    xiaotai_timer_cancel(pages->timer);
    pages->timer = 0;
}

void door_pages_reset(void)
{
    cancel();
    xiaotai_mem_free(pages);
    pages = NULL;
}

static xiaotai_app_door_mode_t saved_mode(void)
{
    int32_t mode = 0;
    if (xiaotai_setting_get_int(XIAOTAI_SETTING_APP_DOOR_SENDER, &mode) != XIAOTAI_OK ||
        mode < XIAOTAI_APP_DOOR_INDOOR || mode > XIAOTAI_APP_DOOR_BELL_INDOOR) mode = XIAOTAI_APP_DOOR_INDOOR;
    return (xiaotai_app_door_mode_t)mode;
}

/* --------------------------------------------------------------- the record */

static size_t encode(void)
{
    uint8_t *out = pages->record;
    size_t at = 0;
    out[at++] = PAGES_VERSION;
    for (size_t m = 0; m < MODES; ++m) {
        const size_t count_at = at++;
        uint8_t count = 0;
        for (size_t i = 0; i < XIAOTAI_APP_DOOR_PEERS_MAX; ++i) {
            const size_t length = strnlen(pages->lists[m][i], XIAOTAI_SETTING_TEXT_MAX - 1U);
            if (!length) continue;
            out[at++] = (uint8_t)length;
            memcpy(out + at, pages->lists[m][i], length);
            at += length;
            ++count;
        }
        out[count_at] = count;
    }
    return at;
}

/* Checked whole before anything is taken (apply false), then taken. */
static bool decode(size_t size, bool apply)
{
    const uint8_t *in = pages->record;
    if (!size || in[0] != PAGES_VERSION) return false;
    size_t at = 1;
    for (size_t m = 0; m < MODES; ++m) {
        if (at >= size || in[at] > XIAOTAI_APP_DOOR_PEERS_MAX) return false;
        const uint8_t count = in[at++];
        for (uint8_t i = 0; i < count; ++i) {
            if (at >= size) return false;
            const uint8_t length = in[at++];
            if (!length || length >= XIAOTAI_SETTING_TEXT_MAX || at + length > size || memchr(in + at, 0, length))
                return false;
            if (apply) {
                memcpy(pages->lists[m][i], in + at, length);
                pages->lists[m][i][length] = '\0';
            }
            at += length;
        }
    }
    return at == size;
}

static xiaotai_err_t save(void)
{
    return xiaotai_kv_set_async(PAGES_SPACE, PAGES_KEY, pages->record, encode());
}

void door_pages_load(void)
{
    if (!pages) return;
    memset(pages->lists, 0, sizeof(pages->lists));
    size_t size = sizeof(pages->record);
    const xiaotai_err_t err = xiaotai_kv_get_blob_strict(PAGES_SPACE, PAGES_KEY, pages->record, &size,
                                                         XIAOTAI_KV_WAIT_MAX_MS);
    if (err == XIAOTAI_OK && decode(size, false)) {
        (void)decode(size, true);
        return;
    }
    /* The first start with pages, or a record that is no record: the
     * counterparts stored so far are the stored mode's. A storage that did
     * not answer keeps what it holds. */
    door_peers_stored(pages->lists[saved_mode()]);
    if (err == XIAOTAI_OK || err == XIAOTAI_ERR_NOT_FOUND || err == XIAOTAI_ERR_INVALID_SIZE ||
        err == XIAOTAI_ERR_INVALID_ARG) (void)save();
}

const door_peer_id_t *door_pages_list(xiaotai_app_door_mode_t mode)
{
    return pages && (unsigned)mode < MODES ? pages->lists[mode] : NULL;
}

xiaotai_err_t door_pages_store(door_peer_id_t *list)
{
    door_pages_commit();        /* what is changed on a page is the page in use */
    const xiaotai_app_door_mode_t mode = door_mode();
    door_peer_id_t before[XIAOTAI_APP_DOOR_PEERS_MAX];
    memcpy(before, pages->lists[mode], sizeof(before));
    memcpy(pages->lists[mode], list, sizeof(before));
    const xiaotai_err_t err = save();
    if (err != XIAOTAI_OK) {
        memcpy(pages->lists[mode], before, sizeof(before));
        return err == XIAOTAI_ERR_NO_MEM ? XIAOTAI_FAIL : err;      /* NO_MEM says "four already" to the caller */
    }
    door_peers_changed();
    return XIAOTAI_OK;
}

/* -------------------------------------------------------------------- pages */

void door_pages_commit(void)
{
    if (!pages || !door.shown) return;
    cancel();
    const uint8_t shown = door.shown;
    door.shown = 0;             /* from here on the setting says it */
    if (xiaotai_setting_set_int(XIAOTAI_SETTING_APP_DOOR_SENDER, shown - 1) != XIAOTAI_OK) {
        door.shown = shown;     /* still the page in front; answered as before */
        return;
    }
    (void)xiaotai_setting_set_int(XIAOTAI_SETTING_APP_DOOR_CHOSEN, 1);
}

static void settled(void *ctx)
{
    (void)ctx;
    if (pages) pages->timer = 0;
    door_pages_commit();
}

static void show(xiaotai_app_door_mode_t mode)
{
    cancel();
    door.shown = mode == saved_mode() ? 0 : (uint8_t)(mode + 1);
    if (door.shown && xiaotai_timer_start(XIAOTAI_APP_DOOR_MODE_SETTLE_MS, settled, NULL, &pages->timer) != XIAOTAI_OK)
        pages->timer = 0;       /* then it settles when it is used */
    door_read_settings();
}

void door_pages_enter(xiaotai_app_entry_t entry)
{
    if (!pages) return;
    show(entry == XIAOTAI_APP_ENTRY_NEXT ? order[0] : entry == XIAOTAI_APP_ENTRY_PREVIOUS ? order[MODES - 1] :
         saved_mode());
}

void door_pages_leave(void)
{
    if (!pages) return;
    cancel();
    if (!door.shown) return;
    door.shown = 0;
    door_read_settings();
}

void door_pages_saved(void)
{
    if (!pages) return;
    cancel();
    door.shown = 0;
}

bool door_pages_step(int direction)
{
    if (!pages) return false;
    const xiaotai_app_door_mode_t mode = door_mode();
    size_t at = 0;
    while (at < MODES && order[at] != mode) ++at;
    const int to = (int)at + direction;
    if (at == MODES || to < 0 || to >= (int)MODES) return false;
    door_access_clear();
    show(order[to]);
    /* The rest of that gesture may land on this page's own arrow. */
    door.last_swipe_valid = true;
    door.last_swipe_us = xiaotai_time_us();
    door_render();
    return true;
}
