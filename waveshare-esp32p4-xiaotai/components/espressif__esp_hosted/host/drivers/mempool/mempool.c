// Copyright 2015-2022 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "mempool.h"
#include "esp_hosted_config.h"
#include "stats.h"
#include "esp_log.h"
#define MEMPOOL_DEBUG 1

#define MEMPOOL_MAX_CACHED_BLOCKS 2U


static char * MEM_TAG = "mpool";
#if H_MEM_STATS
#include "esp_log.h"


#endif

struct mempool * mempool_create_with_allocator(uint32_t block_size,
		mempool_block_alloc_fn_t block_alloc)
{
#ifdef H_USE_MEMPOOL
	struct mempool * new = (struct mempool *)g_h.funcs->_h_malloc(MEMPOOL_ALIGNED(sizeof(struct mempool)));

	if (!new) {
		ESP_LOGE(MEM_TAG, "Prob to create mempool size(%u)", MEMPOOL_ALIGNED(sizeof(struct mempool)));
		return NULL;
	}

	if (!IS_MEMPOOL_ALIGNED((long)new)) {

		ESP_LOGV(MEM_TAG, "Nonaligned");
		g_h.funcs->_h_free(new);
		new = (struct mempool *)g_h.funcs->_h_malloc(MEMPOOL_ALIGNED(sizeof(struct mempool)));
	}

	if (!new) {
		ESP_LOGE(MEM_TAG, "failed to create mempool size(%u)", MEMPOOL_ALIGNED(sizeof(struct mempool)));
		return NULL;
	}

	new->spinlock = g_h.funcs->_h_create_lock_mempool();

	new->block_size = MEMPOOL_ALIGNED(block_size);
	new->cached_count = 0;
	new->cache_limit = MEMPOOL_MAX_CACHED_BLOCKS;
	new->trimmed_count = 0;
	new->block_alloc = block_alloc;
	SLIST_INIT(&(new->head));


	ESP_LOGV(MEM_TAG, "Create mempool %p with block_size:%lu", new, (unsigned long int)block_size);
	return new;
#else
	return NULL;
#endif
}

struct mempool * mempool_create(uint32_t block_size)
{
	return mempool_create_with_allocator(block_size, NULL);
}

uint32_t mempool_reserve(struct mempool *mp, uint32_t block_count)
{
#ifdef H_USE_MEMPOOL
	uint32_t cached_count = 0;

	if (!mp || block_count == 0U) {
		return 0U;
	}

	g_h.funcs->_h_lock_mempool(mp->spinlock);
	if (mp->cache_limit < block_count) {
		mp->cache_limit = block_count;
	}
	cached_count = mp->cached_count;
	g_h.funcs->_h_unlock_mempool(mp->spinlock);

	while (cached_count < block_count) {
		void *block = mp->block_alloc ?
			mp->block_alloc(MEMPOOL_ALIGNED(mp->block_size)) :
			MEM_ALLOC(MEMPOOL_ALIGNED(mp->block_size));
		bool stored = false;
		if (block == NULL) {
			break;
		}

		g_h.funcs->_h_lock_mempool(mp->spinlock);
		if (mp->cached_count < block_count) {
			SLIST_INSERT_HEAD(&(mp->head), (struct mempool_entry *)block, entries);
			mp->cached_count++;
			stored = true;
		}
		cached_count = mp->cached_count;
		g_h.funcs->_h_unlock_mempool(mp->spinlock);

		if (!stored) {
			g_h.funcs->_h_free(block);
		}
	}

	return cached_count;
#else
	(void)mp;
	(void)block_count;
	return 0U;
#endif
}

void mempool_destroy(struct mempool* mp)
{
#ifdef H_USE_MEMPOOL
	void * node1 = NULL;

	if (!mp)
		return;


	ESP_LOGV(MEM_TAG, "Destroy mempool %p", mp);

	while ((node1 = SLIST_FIRST(&(mp->head))) != NULL) {
		SLIST_REMOVE_HEAD(&(mp->head), entries);
		if (mp->cached_count > 0) {
			mp->cached_count--;
		}
		g_h.funcs->_h_free(node1);
	}
	SLIST_INIT(&(mp->head));

	g_h.funcs->_h_free(mp);
#endif
}

void * mempool_alloc(struct mempool* mp, int nbytes, int need_memset)
{
	void *buf = NULL;

#ifdef H_USE_MEMPOOL
	if (!mp || mp->block_size < nbytes)
		return NULL;


	g_h.funcs->_h_lock_mempool(mp->spinlock);


	if (!SLIST_EMPTY(&(mp->head))) {
		buf = SLIST_FIRST(&(mp->head));
		SLIST_REMOVE_HEAD(&(mp->head), entries);
		if (mp->cached_count > 0) {
			mp->cached_count--;
		}


	g_h.funcs->_h_unlock_mempool(mp->spinlock);



#if H_MEM_STATS
	h_stats_g.mp_stats.num_reuse++;
	ESP_LOGV(MEM_TAG, "%p: num_reuse: %lu", mp, (unsigned long int)(h_stats_g.mp_stats.num_reuse));
#endif
	} else {

		g_h.funcs->_h_unlock_mempool(mp->spinlock);

		buf = mp->block_alloc ?
			mp->block_alloc(MEMPOOL_ALIGNED(mp->block_size)) :
			MEM_ALLOC(MEMPOOL_ALIGNED(mp->block_size));
#if H_MEM_STATS
		h_stats_g.mp_stats.num_fresh_alloc++;
		ESP_LOGV(MEM_TAG, "%p: num_alloc: %lu", mp, (unsigned long int)(h_stats_g.mp_stats.num_fresh_alloc));
#endif
	}
#else
	buf = g_h.funcs->_h_malloc_align(MEMPOOL_ALIGNED(nbytes), MEMPOOL_ALIGNMENT_BYTES);
#endif
	ESP_LOGV(MEM_TAG, "alloc %u bytes at %p", nbytes, buf);

	if (buf && need_memset)
		g_h.funcs->_h_memset(buf, 0, nbytes);

	return buf;

}

void mempool_free(struct mempool* mp, void *mem)
{
	if (!mem)
		return;
#ifdef H_USE_MEMPOOL
	if (!mp)
		return;

	g_h.funcs->_h_lock_mempool(mp->spinlock);

	if (mp->cached_count >= mp->cache_limit) {
		mp->trimmed_count++;
		uint32_t trimmed_count = mp->trimmed_count;
		g_h.funcs->_h_unlock_mempool(mp->spinlock);

		g_h.funcs->_h_free(mem);
		if (trimmed_count == 1U || (trimmed_count % 64U) == 0U) {
			ESP_LOGD(MEM_TAG,
				 "trim mempool %p block_size=%lu cached=%lu trimmed=%lu",
				 mp,
				 (unsigned long)mp->block_size,
				 (unsigned long)mp->cache_limit,
				 (unsigned long)trimmed_count);
		}
		return;
	}

	SLIST_INSERT_HEAD(&(mp->head), (struct mempool_entry *)mem, entries);
	mp->cached_count++;

	g_h.funcs->_h_unlock_mempool(mp->spinlock);

#if H_MEM_STATS
	h_stats_g.mp_stats.num_free++;
	ESP_LOGV(MEM_TAG, "%p: num_ret: %lu", mp, (unsigned long int)(h_stats_g.mp_stats.num_free));
#endif

#else
	ESP_LOGV(MEM_TAG, "free at %p", mem);
	g_h.funcs->_h_free_align(mem);
#endif
}
