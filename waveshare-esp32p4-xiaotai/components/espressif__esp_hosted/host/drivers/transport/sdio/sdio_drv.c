// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2023 Espressif Systems (Shanghai) PTE LTD
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

/** Includes **/
#include "string.h"
#include "sdio_drv.h"
#include "sdio_reg.h"
#include "serial_drv.h"
#include "stats.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_hosted_log.h"
#include "hci_drv.h"
#include "endian.h"
#include "esp_hosted_transport_init.h"

static const char TAG[] = "H_SDIO_DRV";

/* when enabled, read all required SDIO slave registers in a single
 * read into a buffer, instead of reading individual SDIO slave
 * registers
 */
#define DO_COMBINED_REG_READ (1)

#define SDIO_REG_SNAPSHOT_MAX_ATTEMPTS 3U
#define SDIO_REG_SNAPSHOT_RETRY_US     200U

/** Constants/Macros **/
#define TO_SLAVE_QUEUE_SIZE               H_SDIO_TX_Q
#define FROM_SLAVE_QUEUE_SIZE             H_SDIO_RX_Q

#define RX_TASK_STACK_SIZE                4096
#define TX_TASK_STACK_SIZE                4096
#define PROCESS_RX_TASK_STACK_SIZE        4096
#define RX_TIMEOUT_TICKS                  50
#define SDIO_STREAM_RX_PSRAM_PREALLOC_BYTES   (64U * 1024U)
#define SDIO_STREAM_RX_INTERNAL_FALLBACK_BYTES (4U * 1024U)

#if CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define SDIO_STREAM_RX_CACHE_ALIGNMENT CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define SDIO_STREAM_RX_CACHE_ALIGNMENT CONFIG_CACHE_L1_CACHE_LINE_SIZE
#endif

#define BUFFER_AVAILABLE                  1
#define BUFFER_UNAVAILABLE                0

// max number of time to try to read write buffer available reg
#define MAX_WRITE_BUF_RETRIES             50

/* Actual data sdio_write max retry */
#define MAX_SDIO_WRITE_RETRY              2

// this locks the sdio transaction at the driver level, instead of at the HAL layer
#define USE_DRIVER_LOCK

#if defined(USE_DRIVER_LOCK)
#define ACQUIRE_LOCK false
#else
#define ACQUIRE_LOCK true
#endif

#if defined(USE_DRIVER_LOCK)
static void * sdio_bus_lock;

#define SDIO_DRV_LOCK()   g_h.funcs->_h_lock_mutex(sdio_bus_lock, HOSTED_BLOCK_MAX);
#define SDIO_DRV_UNLOCK() g_h.funcs->_h_unlock_mutex(sdio_bus_lock);

#else
#define SDIO_DRV_LOCK()
#define SDIO_DRV_UNLOCK()
#endif

#if DO_COMBINED_REG_READ
// read data from ESP_SLAVE_INT_RAW_REG to ESP_SLAVE_PACKET_LEN_REG
// plus 4 for the len of the register
#define REG_BUF_LEN (ESP_SLAVE_PACKET_LEN_REG - ESP_SLAVE_INT_RAW_REG + 4)

// byte index into the buffer to locate the register
#define INT_RAW_INDEX (0)
#define PACKET_LEN_INDEX (ESP_SLAVE_PACKET_LEN_REG - ESP_SLAVE_INT_RAW_REG)

static uint8_t *reg_buf = NULL;
static uint32_t sdio_reg_snapshot_recovered_count;
static uint32_t sdio_reg_snapshot_failed_count;
#endif

/* Create mempool for cache mallocs */
static struct mempool * buf_mp_g;

/* TODO to move this in transport drv */
extern transport_channel_t *chan_arr[ESP_MAX_IF];

static void * sdio_handle = NULL;
static void * sdio_bus_lock;
static void * sdio_read_thread;
static void * sdio_process_rx_thread;
static void * sdio_write_thread;

static queue_handle_t to_slave_queue[MAX_PRIORITY_QUEUES];
semaphore_handle_t sem_to_slave_queue;
static queue_handle_t from_slave_queue[MAX_PRIORITY_QUEUES];
semaphore_handle_t sem_from_slave_queue;

/* Counter to hold the amount of buffers already sent to sdio slave */
static uint32_t sdio_tx_buf_count = 0;

/* Counter to hold the amount of bytes already received from sdio slave */
static uint32_t sdio_rx_byte_count = 0;
#if H_SDIO_HOST_RX_MODE == H_SDIO_HOST_STREAMING_MODE
static uint32_t sdio_rx_alloc_drop_count = 0;
static uint32_t sdio_rx_stream_alloc_drop_count = 0;
#endif
static uint32_t sdio_rx_copy_drop_count = 0;

// one-time trigger to start write thread
static bool sdio_start_write_thread = false;

/** structs to do double buffering
 * sdio_read_task() writes Rx SDIO data to one buffer while
 * sdio_data_to_rx_buf_task() transfers previously received data
 * to the rx queue
 */
typedef struct {
	uint8_t * buf;
	uint32_t buf_size;
} buf_info_t;

typedef struct {
	buf_info_t buffer[2];
	int write_index;
} double_buf_t;

static double_buf_t double_buf;

typedef struct {
	int buffer_index;
	uint32_t data_len;
} sdio_rx_batch_t;

/* A real two-slot ownership queue. The upstream implementation only tracked
 * one pending read_index, so a second completed SDIO transfer was discarded
 * whenever stream splitting had not finished yet. */
static queue_handle_t sdio_rx_free_buf_queue;
static queue_handle_t sdio_rx_ready_buf_queue;
static uint32_t sdio_rx_backpressure_count;
static uint32_t sdio_rx_backpressure_max_us;
static int64_t sdio_rx_backpressure_last_log_us;
static uint32_t sdio_tx_throttle_start_count;
static uint32_t sdio_tx_throttle_stop_count;
static int64_t sdio_tx_throttle_start_us;

static void * sdio_rx_buf_thread;
static void sdio_data_to_rx_buf_task(void const* pvParameters);

/* Streaming RX packets are copied to PSRAM before entering the host queue, so
 * the shared internal-DMA pool only needs to cover the serial TX owner. */
#define SDIO_TX_POOL_PREALLOC_BLOCKS 2U

static esp_err_t sdio_generate_slave_intr(uint8_t intr_no);

static void sdio_write_task(void const* pvParameters);
static void sdio_read_task(void const* pvParameters);
static void sdio_process_rx_task(void const* pvParameters);

/*
 * Keep the interrupt-facing SDIO reader at Espressif's realtime priority, but
 * do not run the packet forwarding loops at that same priority. Under sustained
 * full-duplex video, sdio_write and sdio_process_rx can remain continuously
 * ready; at priority 23 they occupy both P4 cores and prevent the RTC socket
 * thread from running for hundreds of milliseconds. Keep the blocking reader
 * and buffer handoff high, but let RTC, audio, and media workers run ahead of
 * the sustained packet-forwarding loops. The queues retain the short bursts.
 */
#define SDIO_READ_TASK_PRIORITY        DFLT_TASK_PRIO
#define SDIO_RX_BUF_TASK_PRIORITY      (DFLT_TASK_PRIO - 1U)
#define SDIO_DATA_PATH_TASK_PRIORITY   16U
static void sdio_log_dma_no_mem(const char *stage, uint32_t len, uint32_t drops);

static uint8_t *sdio_alloc_stream_rx_psram(uint32_t len)
{
#if CONFIG_SPIRAM && CONFIG_SOC_PSRAM_DMA_CAPABLE
	return (uint8_t *)heap_caps_aligned_alloc(
		SDIO_STREAM_RX_CACHE_ALIGNMENT,
		len,
		MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
#else
	(void)len;
	return NULL;
#endif
}

static uint8_t *sdio_alloc_stream_packet_psram(uint32_t len)
{
#if CONFIG_SPIRAM
	return (uint8_t *)heap_caps_aligned_alloc(
		SDIO_STREAM_RX_CACHE_ALIGNMENT,
		len,
		MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
	(void)len;
	return NULL;
#endif
}

static void sdio_prealloc_stream_rx_buffers(void)
{
#if H_SDIO_HOST_RX_MODE == H_SDIO_HOST_STREAMING_MODE
	bool psram_buffer[2] = {false, false};

	for (int index = 0; index < 2; ++index) {
		if (double_buf.buffer[index].buf != NULL) {
			continue;
		}

		uint32_t allocated_size = SDIO_STREAM_RX_PSRAM_PREALLOC_BYTES;
		double_buf.buffer[index].buf =
			sdio_alloc_stream_rx_psram(allocated_size);
		psram_buffer[index] = double_buf.buffer[index].buf != NULL;
		if (double_buf.buffer[index].buf == NULL) {
			/*
			 * Keep the generic ESP-Hosted fallback small. On P4, SDMMC can
			 * DMA directly to cache-aligned PSRAM and performs cache sync
			 * in the host driver, so large burst buffers must not fragment
			 * the scarce internal DMA heap used by H264 and Wi-Fi packets.
			 */
			allocated_size = SDIO_STREAM_RX_INTERNAL_FALLBACK_BYTES;
			double_buf.buffer[index].buf = (uint8_t *)MEM_ALLOC(allocated_size);
		}
		if (double_buf.buffer[index].buf == NULL) {
			sdio_rx_stream_alloc_drop_count++;
			sdio_log_dma_no_mem("stream_prealloc",
					    allocated_size,
					    sdio_rx_stream_alloc_drop_count);
			continue;
		}
		double_buf.buffer[index].buf_size = allocated_size;
	}

	if (double_buf.buffer[0].buf != NULL && double_buf.buffer[1].buf != NULL) {
		const char *location =
			psram_buffer[0] && psram_buffer[1] ? "psram-dma" :
			(!psram_buffer[0] && !psram_buffer[1] ? "internal-dma" : "mixed");
		ESP_LOGI(TAG,
			 "SDIO streaming RX buffers ready: memory=%s sizes=%lu/%lu",
			 location,
			 (unsigned long)double_buf.buffer[0].buf_size,
			 (unsigned long)double_buf.buffer[1].buf_size);
	}
#endif
}

static inline void sdio_mempool_create(void)
{
	MEM_DUMP("sdio_mempool_create");
	buf_mp_g = mempool_create(MAX_SDIO_BUFFER_SIZE);
#ifdef H_USE_MEMPOOL
	assert(buf_mp_g);
	/* Keep the SDIO write owner deterministic. Streaming RX packets have a
	 * separate PSRAM lifetime and no longer compete for this DMA pool. */
	uint32_t reserved = mempool_reserve(buf_mp_g, SDIO_TX_POOL_PREALLOC_BLOCKS);
	ESP_LOGI(TAG,
		 "SDIO TX DMA pool reserved: blocks=%lu/%u queue=%u block_size=%u bytes=%lu",
		 (unsigned long)reserved,
		 (unsigned)SDIO_TX_POOL_PREALLOC_BLOCKS,
		 (unsigned)TO_SLAVE_QUEUE_SIZE,
		 (unsigned)MEMPOOL_ALIGNED(MEMPOOL_ALIGNED(MAX_SDIO_BUFFER_SIZE)),
		 (unsigned long)(reserved *
				 MEMPOOL_ALIGNED(MEMPOOL_ALIGNED(MAX_SDIO_BUFFER_SIZE))));
#endif
}

static inline void sdio_mempool_destroy(void)
{
	mempool_destroy(buf_mp_g);
}

static inline void *sdio_buffer_alloc(uint need_memset)
{
	return mempool_alloc(buf_mp_g, MAX_SDIO_BUFFER_SIZE, need_memset);
}

static inline void sdio_buffer_free(void *buf)
{
	mempool_free(buf_mp_g, buf);
}

static void sdio_log_dma_no_mem(const char *stage, uint32_t len, uint32_t drops)
{
	if (drops != 1 && (drops % 32U) != 0) {
		return;
	}

	size_t dma_free =
		heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
	size_t dma_largest =
		heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
	size_t psram_largest =
		heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	ESP_LOGW(TAG,
		"SDIO RX no DMA memory: stage=%s len=%lu drops=%lu dma_free=%u dma_largest=%u psram_largest=%u",
		stage,
		(unsigned long)len,
		(unsigned long)drops,
		(unsigned)dma_free,
		(unsigned)dma_largest,
		(unsigned)psram_largest);
}

void transport_deinit_internal(void)
{
	/* TODO */
}

void transport_cleanup_failed_init(void)
{
	/* Called by the configuring task after card-init failed, before opening
	 * the data path. No producer owns these queues or buffers at this point.
	 * Delete the failed reader externally: self-delete WithCaps would allocate
	 * another internal-RAM cleanup task on the error path. */
	void **threads[] = {&sdio_read_thread, &sdio_rx_buf_thread,
		&sdio_process_rx_thread, &sdio_write_thread};
	for (unsigned i = 0; i < sizeof(threads) / sizeof(threads[0]); ++i) {
		if (*threads[i]) g_h.funcs->_h_thread_cancel(*threads[i]);
		*threads[i] = NULL;
	}
	for (unsigned i = 0; i < MAX_PRIORITY_QUEUES; ++i) {
		if (from_slave_queue[i]) g_h.funcs->_h_destroy_queue(from_slave_queue[i]);
		if (to_slave_queue[i]) g_h.funcs->_h_destroy_queue(to_slave_queue[i]);
		from_slave_queue[i] = to_slave_queue[i] = NULL;
	}
	if (sdio_rx_free_buf_queue) g_h.funcs->_h_destroy_queue(sdio_rx_free_buf_queue);
	if (sdio_rx_ready_buf_queue) g_h.funcs->_h_destroy_queue(sdio_rx_ready_buf_queue);
	sdio_rx_free_buf_queue = sdio_rx_ready_buf_queue = NULL;
	if (sem_to_slave_queue) g_h.funcs->_h_destroy_semaphore(sem_to_slave_queue);
	if (sem_from_slave_queue) g_h.funcs->_h_destroy_semaphore(sem_from_slave_queue);
	sem_to_slave_queue = sem_from_slave_queue = NULL;
	for (unsigned i = 0; i < 2; ++i) {
		heap_caps_free(double_buf.buffer[i].buf);
		double_buf.buffer[i] = (buf_info_t){0};
	}
	sdio_mempool_destroy();
	buf_mp_g = NULL;
	if (sdio_bus_lock) g_h.funcs->_h_destroy_mutex(sdio_bus_lock);
	sdio_bus_lock = NULL;
	/* The card wrapper already released SDMMC/card resources on failure. */
	sdio_handle = NULL;
}

static int sdio_generate_slave_intr(uint8_t intr_no)
{
	uint8_t intr_mask = BIT(intr_no + ESP_SDIO_CONF_OFFSET);

	if (intr_no >= BIT(ESP_MAX_HOST_INTERRUPT)) {
		ESP_LOGE(TAG,"Invalid slave interrupt number");
		return ESP_ERR_INVALID_ARG;
	}

	return g_h.funcs->_h_sdio_write_reg(sdio_handle, HOST_TO_SLAVE_INTR, &intr_mask,
		sizeof(intr_mask), ACQUIRE_LOCK);
}

static inline int sdio_get_intr(uint32_t *interrupts)
{
	return g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_INT_RAW_REG, (uint8_t *)interrupts,
		sizeof(uint32_t), ACQUIRE_LOCK);
}

static inline int sdio_clear_intr(uint32_t interrupts)
{
	return g_h.funcs->_h_sdio_write_reg(sdio_handle, ESP_SLAVE_INT_CLR_REG, (uint8_t *)&interrupts,
		sizeof(uint32_t), ACQUIRE_LOCK);
}

static int sdio_get_tx_buffer_num(uint32_t *tx_num, bool is_lock_needed)
{
	uint32_t len = 0;
	int ret = 0;

	ret = g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_TOKEN_RDATA, (uint8_t *)&len,
		sizeof(len), is_lock_needed);

	if (ret) {
		ESP_LOGE(TAG, "%s: err: %d", __func__, ret);
		return ret;
	}

	len = (len >> 16) & ESP_TX_BUFFER_MASK;
	len = (len + ESP_TX_BUFFER_MAX - sdio_tx_buf_count) % ESP_TX_BUFFER_MAX;

	*tx_num = len;

	return ret;
}

#if DO_COMBINED_REG_READ
static int sdio_read_regs(uint8_t * buf)
{
	return g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_INT_RAW_REG, buf, REG_BUF_LEN, ACQUIRE_LOCK);
}

static bool sdio_regs_are_all_ones(const uint8_t *buf)
{
	for (size_t index = 0; index < REG_BUF_LEN; ++index) {
		if (buf[index] != UINT8_MAX) {
			return false;
		}
	}
	return true;
}

/* Do not acknowledge an interrupt using an all-ones bus-error snapshot. */
static int sdio_read_valid_regs(uint8_t *buf)
{
	int ret = ESP_FAIL;

	for (uint32_t attempt = 1U; attempt <= SDIO_REG_SNAPSHOT_MAX_ATTEMPTS; ++attempt) {
		ret = sdio_read_regs(buf);
		if (ret == ESP_OK && !sdio_regs_are_all_ones(buf)) {
			if (attempt > 1U) {
				sdio_reg_snapshot_recovered_count++;
				ESP_LOGW(TAG,
						"SDIO register snapshot recovered: attempts=%lu recovered=%lu",
						(unsigned long)attempt,
						(unsigned long)sdio_reg_snapshot_recovered_count);
			}
			return ESP_OK;
		}
		if (attempt < SDIO_REG_SNAPSHOT_MAX_ATTEMPTS) {
			g_h.funcs->_h_usleep(SDIO_REG_SNAPSHOT_RETRY_US);
		}
	}

	sdio_reg_snapshot_failed_count++;
	ESP_LOGE(TAG,
			"SDIO register snapshot invalid after retries: ret=%d failed=%lu",
			ret,
			(unsigned long)sdio_reg_snapshot_failed_count);
	return ESP_FAIL;
}
#endif

#if H_SDIO_HOST_RX_MODE != H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE

#if DO_COMBINED_REG_READ
// get the length from the provided register value
static int sdio_get_len_from_slave(uint32_t *rx_size, uint32_t reg_val, bool is_lock_needed)
{
	uint32_t len = reg_val;
	uint32_t temp;

	if (!rx_size)
		return ESP_FAIL;
	*rx_size = 0;

	len &= ESP_SLAVE_LEN_MASK;

	if (len >= sdio_rx_byte_count)
		len = (len + ESP_RX_BYTE_MAX - sdio_rx_byte_count) % ESP_RX_BYTE_MAX;
	else {
		/* Handle a case of roll over */
		temp = ESP_RX_BYTE_MAX - sdio_rx_byte_count;
		len = temp + len;
	}

#if H_SDIO_HOST_RX_MODE != H_SDIO_HOST_STREAMING_MODE
	if (len > ESP_RX_BUFFER_SIZE) {
		ESP_LOGE(TAG, "%s: Len from slave[%ld] exceeds max [%d]",
				__func__, len, ESP_RX_BUFFER_SIZE);
		return ESP_FAIL;
	}
#endif

	*rx_size = len;

	return 0;
}
#else
// get the length by reading the register
static int sdio_get_len_from_slave(uint32_t *rx_size, bool is_lock_needed)
{
	uint32_t len;
	uint32_t temp;
	int ret = 0;

	if (!rx_size)
		return ESP_FAIL;
	*rx_size = 0;

	ret = g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_PACKET_LEN_REG,
		(uint8_t *)&len, sizeof(len), is_lock_needed);

	if (ret) {
		ESP_LOGE(TAG, "len read err: %d", ret);
		return ret;
	}

	len &= ESP_SLAVE_LEN_MASK;

	if (len >= sdio_rx_byte_count)
		len = (len + ESP_RX_BYTE_MAX - sdio_rx_byte_count) % ESP_RX_BYTE_MAX;
	else {
		/* Handle a case of roll over */
		temp = ESP_RX_BYTE_MAX - sdio_rx_byte_count;
		len = temp + len;
	}

#if H_SDIO_HOST_RX_MODE != H_SDIO_HOST_STREAMING_MODE
	if (len > ESP_RX_BUFFER_SIZE) {
		ESP_LOGE(TAG, "%s: Len from slave[%ld] exceeds max [%d]",
				__func__, len, ESP_RX_BUFFER_SIZE);
		return ESP_FAIL;
	}
#endif

	*rx_size = len;

	return 0;
}
#endif

#endif

static int sdio_is_write_buffer_available(uint32_t buf_needed)
{
	static uint32_t buf_available = 0;
	uint8_t retry = MAX_WRITE_BUF_RETRIES;
	uint32_t max_retry_sdio_not_responding = 2;

	/*If buffer needed are less than buffer available
	  then only read for available buffer number from slave*/
	if (buf_available < buf_needed) {
		while (retry) {
			if (sdio_get_tx_buffer_num(&buf_available, ACQUIRE_LOCK) ==
					ESP_HOSTED_SDIO_UNRESPONSIVE_CODE) {
				max_retry_sdio_not_responding--;
				/* restart the host to avoid the sdio locked out state */

				if (!max_retry_sdio_not_responding) {
					ESP_LOGE(TAG, "%s: SDIO slave unresponsive, restart host", __func__);
					g_h.funcs->_h_restart_host();
				}
				continue;
			}

			if (buf_available < buf_needed) {

				ESP_LOGV(TAG, "Retry get write buffers %d", retry);
				retry--;

				if (retry < MAX_WRITE_BUF_RETRIES/2)
					g_h.funcs->_h_msleep(1);

				continue;
			}
			break;
		}
	}

	if (buf_available >= buf_needed)
		buf_available -= buf_needed;

	if (!retry) {
		/* No buffer available at slave */
		return BUFFER_UNAVAILABLE;
	}

	return BUFFER_AVAILABLE;
}

static void sdio_write_task(void const* pvParameters)
{
	uint16_t len = 0;
	uint8_t *sendbuf = NULL;
	void (*free_func)(void* ptr) = NULL;
	struct esp_payload_header * payload_header = NULL;
	uint8_t * payload  = NULL;
	interface_buffer_handle_t buf_handle = {0};
	int retries = 0;

	int ret = 0;
	uint8_t *pos = NULL;
	uint32_t data_left;
	uint32_t len_to_send;
	uint32_t buf_needed;
	uint8_t tx_needed = 1;

	while (!sdio_start_write_thread)
		g_h.funcs->_h_msleep(10);

	for (;;) {
		/* Check if higher layers have anything to transmit */
		g_h.funcs->_h_get_semaphore(sem_to_slave_queue, HOSTED_BLOCK_MAX);

		/* Tx msg is present as per sem */
		if (g_h.funcs->_h_dequeue_item(to_slave_queue[PRIO_Q_SERIAL], &buf_handle, 0))
			if (g_h.funcs->_h_dequeue_item(to_slave_queue[PRIO_Q_BT], &buf_handle, 0))
				if (g_h.funcs->_h_dequeue_item(to_slave_queue[PRIO_Q_OTHERS], &buf_handle, 0)) {
					tx_needed = 0; /* No Tx msg */
				}

		if (tx_needed)
			len = buf_handle.payload_len;

		if (!len) {
			ESP_LOGE(TAG, "%s: Empty len", __func__);
			goto done;
		}

		if (!buf_handle.payload_zcopy) {
			sendbuf = sdio_buffer_alloc(MEMSET_REQUIRED);
			assert(sendbuf);
			free_func = sdio_buffer_free;
		} else {
			sendbuf = buf_handle.payload;
			free_func = buf_handle.free_buf_handle;
		}

		if (!sendbuf) {
			ESP_LOGE(TAG, "sdio buff malloc failed");
			free_func = NULL;
			goto done;
		}

		if (buf_handle.payload_len > MAX_SDIO_BUFFER_SIZE - sizeof(struct esp_payload_header)) {
			ESP_LOGE(TAG, "Pkt len [%u] > Max [%u]. Drop",
					buf_handle.payload_len, MAX_SDIO_BUFFER_SIZE - sizeof(struct esp_payload_header));
			goto done;
		}

		/* Form Tx header */
		payload_header = (struct esp_payload_header *) sendbuf;
		payload  = sendbuf + sizeof(struct esp_payload_header);

		payload_header->len = htole16(len);
		payload_header->offset = htole16(sizeof(struct esp_payload_header));
		payload_header->if_type = buf_handle.if_type;
		payload_header->if_num = buf_handle.if_num;
		payload_header->seq_num = htole16(buf_handle.seq_num);
		payload_header->flags = buf_handle.flag;

		if (payload_header->if_type == ESP_HCI_IF) {
			// special handling for HCI
			if (!buf_handle.payload_zcopy) {
				// copy first byte of payload into header
				payload_header->hci_pkt_type = buf_handle.payload[0];
				// adjust actual payload len
				len -= 1;
				payload_header->len = htole16(len);
				g_h.funcs->_h_memcpy(payload, &buf_handle.payload[1], len);
			}
		} else
		if (!buf_handle.payload_zcopy)
			g_h.funcs->_h_memcpy(payload, buf_handle.payload, len);

#if H_SDIO_CHECKSUM
		payload_header->checksum = htole16(compute_checksum(sendbuf,
			sizeof(struct esp_payload_header) + len));
#endif

		buf_needed = (len + sizeof(struct esp_payload_header) + ESP_RX_BUFFER_SIZE - 1)
			/ ESP_RX_BUFFER_SIZE;

		SDIO_DRV_LOCK();

		ret = sdio_is_write_buffer_available(buf_needed);
		if (ret != BUFFER_AVAILABLE) {
			ESP_LOGV(TAG, "no SDIO write buffers on slave device");
			goto unlock_done;
		}

		pos = sendbuf;
		data_left = len + sizeof(struct esp_payload_header);

		ESP_HEXLOGV("h_sdio_tx", sendbuf, min(32,data_left));

		len_to_send = 0;
		retries = 0;
		do {
			len_to_send = data_left;

#if H_SDIO_TX_BLOCK_ONLY_XFER
			/* Extend the transfer length to do block only transfers.
			 * This is safe as slave only reads up to data_left, which
			 * is not changed here. Rest of data is discarded by
			 * slave.
			 */
			uint32_t block_send_len = ((len_to_send + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;

			ret = g_h.funcs->_h_sdio_write_block(sdio_handle, ESP_SLAVE_CMD53_END_ADDR - data_left,
				pos, block_send_len, ACQUIRE_LOCK);
#else
			ret = g_h.funcs->_h_sdio_write_block(sdio_handle, ESP_SLAVE_CMD53_END_ADDR - data_left,
				pos, len_to_send, ACQUIRE_LOCK);
#endif
			if (ret) {
				ESP_LOGE(TAG, "%s: %d: Failed to send data: %d %ld %ld", __func__,
					retries, ret, len_to_send, data_left);
				retries++;
				if (retries < MAX_SDIO_WRITE_RETRY) {
					ESP_LOGD(TAG, "retry");
					continue;
				} else {
					SDIO_DRV_UNLOCK();
					ESP_LOGE(TAG, "Unrecoverable host sdio state, reset host mcu");
					g_h.funcs->_h_restart_host();
					goto done;
				}
			}

			data_left -= len_to_send;
			pos += len_to_send;
		} while (data_left);

		sdio_tx_buf_count += buf_needed;
		sdio_tx_buf_count = sdio_tx_buf_count % ESP_TX_BUFFER_MAX;

#if ESP_PKT_STATS
			if (buf_handle.if_type == ESP_STA_IF)
				pkt_stats.sta_tx_out++;
#endif

unlock_done:
		SDIO_DRV_UNLOCK();
done:
		if (len && !buf_handle.payload_zcopy) {
			/* free allocated buffer, only if zerocopy is not requested */
			H_FREE_PTR_WITH_FUNC(buf_handle.free_buf_handle, buf_handle.priv_buffer_handle);
		}
		H_FREE_PTR_WITH_FUNC(free_func, sendbuf);
	}
}

static int is_valid_sdio_rx_packet(uint8_t *rxbuff_a, uint16_t *len_a, uint16_t *offset_a)
{
	struct esp_payload_header * h = (struct esp_payload_header *)rxbuff_a;
	uint16_t len = 0, offset = 0;
#if H_SDIO_CHECKSUM
	uint16_t rx_checksum = 0, checksum = 0;
#endif

	if (!h || !len_a || !offset_a)
		return 0;

	/* Fetch length and offset from payload header */
	len = le16toh(h->len);
	offset = le16toh(h->offset);

	if ((!len) ||
		(len > MAX_PAYLOAD_SIZE) ||
		(offset != sizeof(struct esp_payload_header))) {

		/* Free up buffer, as one of following -
		 * 1. no payload to process
		 * 2. input packet size > driver capacity
		 * 3. payload header size mismatch,
		 * wrong header/bit packing?
		 * */
		return 0;

	}

#if H_SDIO_CHECKSUM
	rx_checksum = le16toh(h->checksum);
	h->checksum = 0;
	checksum = compute_checksum((uint8_t*)h, len + offset);

	if (checksum != rx_checksum) {
		ESP_LOGE(TAG, "SDIO RX rx_chksum[%u] != checksum[%u]. Drop.",
				checksum, rx_checksum);
		return 0;
	}
#endif

#if ESP_PKT_STATS
	if (h->if_type == ESP_STA_IF)
		pkt_stats.sta_rx_in++;
#endif

	*len_a = len;
	*offset_a = offset;

	return 1;
}

// pushes received packet data on to rx queue
static esp_err_t sdio_push_pkt_to_queue(uint8_t * rxbuff,
					uint16_t len,
					uint16_t offset,
					void (*free_buf_handle)(void *),
					bool direct_netstack_handoff)
{
	uint8_t pkt_prio = PRIO_Q_OTHERS;
	struct esp_payload_header *h= NULL;
	interface_buffer_handle_t buf_handle;

	h = (struct esp_payload_header *)rxbuff;

	memset(&buf_handle, 0, sizeof(interface_buffer_handle_t));

	buf_handle.priv_buffer_handle = rxbuff;
	buf_handle.free_buf_handle    = free_buf_handle;
	buf_handle.payload_len        = len;
	buf_handle.if_type            = h->if_type;
	buf_handle.if_num             = h->if_num;
	buf_handle.payload            = rxbuff + offset;
	buf_handle.seq_num            = le16toh(h->seq_num);
	buf_handle.flag               = h->flags;
	buf_handle.payload_zcopy      = direct_netstack_handoff &&
					       (h->if_type == ESP_STA_IF || h->if_type == ESP_AP_IF);

	if (buf_handle.if_type == ESP_SERIAL_IF)
		pkt_prio = PRIO_Q_SERIAL;
	else if (buf_handle.if_type == ESP_HCI_IF)
		pkt_prio = PRIO_Q_BT;
	/* else OTHERS by default */

	g_h.funcs->_h_queue_item(from_slave_queue[pkt_prio], &buf_handle, HOSTED_BLOCK_MAX);
	g_h.funcs->_h_post_semaphore(sem_from_slave_queue);

	return ESP_OK;
}

/**
 * These function definitions depend on whether we are in SDIO
 * streaming mode or not.
 */
#if H_SDIO_HOST_RX_MODE != H_SDIO_HOST_STREAMING_MODE
// SDIO packet mode
// return a buffer big enough to contain the data
static inline uint8_t * sdio_rx_get_buffer(uint32_t len)
{
	int index = double_buf.write_index;
	uint8_t ** buf = &double_buf.buffer[index].buf;

	*buf = (uint8_t *)sdio_buffer_alloc(MEMSET_REQUIRED);
	double_buf.buffer[index].buf_size = len;

	return *buf;
}

// this frees the buffer *before* it is queued
static void sdio_rx_free_buffer(uint8_t * buf)
{
	sdio_buffer_free(buf);
}

// push buffer on to the queue
static esp_err_t sdio_push_data_to_queue(uint8_t * buf, uint32_t buf_len)
{
	uint16_t len = 0;
	uint16_t offset = 0;

	/* Drop packet if no processing needed */
	if (!is_valid_sdio_rx_packet(buf, &len, &offset)) {
		/* Free up buffer, as one of following -
		 * 1. no payload to process
		 * 2. input packet size > driver capacity
		 * 3. payload header size mismatch,
		 * wrong header/bit packing?
		 * */
		ESP_LOGE(TAG, "Dropping packet");
		HOSTED_FREE(buf);
		return ESP_FAIL;
	}

	if (sdio_push_pkt_to_queue(buf, len, offset, sdio_buffer_free, false)) {
		ESP_LOGE(TAG, "Failed to push Rx packet to queue");
		return ESP_FAIL;
	}

	return ESP_OK;
}
#else // H_SDIO_HOST_STREAMING_MODE
// SDIO streaming mode
// return a buffer big enough to contain the data
static uint8_t * sdio_rx_get_buffer(uint32_t len)
{
#if H_SDIO_RX_BLOCK_ONLY_XFER
	// we need to allocate enough memory to hold the padded data
	len = ((len + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;
#endif

	// (re)allocate a write buffer big enough to contain the data stream
	int index = double_buf.write_index;
	uint8_t ** buf = &double_buf.buffer[index].buf;

	if (len > double_buf.buffer[index].buf_size) {
		uint8_t *new_buf = sdio_alloc_stream_rx_psram(len);
		if (!new_buf) {
			new_buf = (uint8_t *)MEM_ALLOC(len);
		}
		if (!new_buf) {
			sdio_rx_stream_alloc_drop_count++;
			sdio_log_dma_no_mem("stream_buffer", len, sdio_rx_stream_alloc_drop_count);
			return NULL;
		}
		if (*buf) {
			// free already allocated memory
			g_h.funcs->_h_free(*buf);
		}
		*buf = new_buf;
		double_buf.buffer[index].buf_size = len;
		ESP_LOGD(TAG, "buf %d size: %ld", index, double_buf.buffer[index].buf_size);
	}
	return *buf;
}

// this frees the buffer *before* it is queued
static void sdio_rx_free_buffer(uint8_t * buf)
{
	// no op - keep the allocated static buffer as it is
}

// extract packets from the stream and push on to the queue
static esp_err_t sdio_push_data_to_queue(uint8_t * buf, uint32_t buf_len)
{
	uint8_t * pkt_rxbuff = NULL;
	uint16_t len = 0;
	uint16_t offset = 0;
	uint32_t packet_size;

	// break up the data stream into packets to send to the queue
	do {
		if (!is_valid_sdio_rx_packet(buf, &len, &offset)) {
			/* Have to drop packets in the stream as we cannot decode
			 * them after this error */
			ESP_LOGE(TAG, "Dropping packet(s) from stream");
			return ESP_FAIL;
		}
		packet_size = len + offset;
		void (*packet_free)(void *) = sdio_buffer_free;
		bool direct_netstack_handoff = false;

		/* The stream itself already lives in cache-aligned PSRAM. Keep the
		 * independent packet lifetime there as well; this buffer is consumed by
		 * the CPU/netstack and is never an SDIO DMA target. */
		pkt_rxbuff = sdio_alloc_stream_packet_psram(packet_size);
		if (pkt_rxbuff != NULL) {
			packet_free = g_h.funcs->_h_free;
			direct_netstack_handoff = true;
		} else {
			pkt_rxbuff = sdio_buffer_alloc(MEMSET_REQUIRED);
		}
		if (!pkt_rxbuff) {
			sdio_rx_alloc_drop_count++;
			sdio_log_dma_no_mem("packet_buffer", len, sdio_rx_alloc_drop_count);
			return ESP_ERR_NO_MEM;
		}

		if (packet_size > buf_len) {
			ESP_LOGE(TAG, "packet size too big for remaining stream data");
			packet_free(pkt_rxbuff);
			return ESP_FAIL;
		}
		memcpy(pkt_rxbuff, buf, packet_size);

		if (sdio_push_pkt_to_queue(pkt_rxbuff,
					       len,
					       offset,
					       packet_free,
					       direct_netstack_handoff)) {
			ESP_LOGI(TAG, "Failed to push a packet to queue from stream");
			packet_free(pkt_rxbuff);
		}

		// move to the next packet in the stream
		buf_len -= packet_size;
		buf     += packet_size;
	} while (buf_len);

	return ESP_OK;
}
#endif

// double buffer task to transfer data from the current buffer to the queue
static void sdio_data_to_rx_buf_task(void const* pvParameters)
{
	sdio_rx_batch_t batch;

	while (1) {
		if (g_h.funcs->_h_dequeue_item(sdio_rx_ready_buf_queue,
						      &batch,
						      HOSTED_BLOCK_MAX)) {
			ESP_LOGE(TAG, "failed to receive SDIO RX batch");
			continue;
		}

		if (batch.buffer_index < 0 || batch.buffer_index >= 2) {
			ESP_LOGE(TAG, "invalid SDIO RX buffer index: %d", batch.buffer_index);
			continue;
		}

		if (sdio_push_data_to_queue(
				double_buf.buffer[batch.buffer_index].buf,
				batch.data_len)) {
			ESP_LOGE(TAG, "Failed to push data to rx queue");
		}

		if (g_h.funcs->_h_queue_item(sdio_rx_free_buf_queue,
						  &batch.buffer_index,
						  HOSTED_BLOCK_MAX)) {
			ESP_LOGE(TAG, "failed to release SDIO RX buffer: %d",
				 batch.buffer_index);
		}
	}
}

static void sdio_read_task(void const* pvParameters)
{
	esp_err_t res;
	uint8_t *rxbuff = NULL;
	int ret;
	int rx_buffer_index;
	uint32_t len_from_slave = 0;

	uint32_t data_left;
	uint32_t len_to_read;
	uint8_t *pos;
	uint32_t interrupts;

#if DO_COMBINED_REG_READ
	uint32_t *intr_index = NULL;
#if H_SDIO_HOST_RX_MODE != H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE
	uint32_t *read_len_index = NULL;
#endif
#endif

	assert(sdio_handle);

	// wait for transport to be in reset state
	while (true) {
		vTaskDelay(pdMS_TO_TICKS(100));
		if (is_transport_rx_ready()) {
			break;
		}
	}

	res = g_h.funcs->_h_sdio_card_init(sdio_handle);
	if (res != ESP_OK) {
		ESP_LOGE(TAG, "sdio card init failed: %s", esp_err_to_name(res));
		transport_drv_report_init_error(res);
		/* The configuring owner cancels and frees this task. A FreeRTOS task
		 * must never return through its uninitialized return address. */
		for (;;) vTaskSuspend(NULL);
	}

	create_debugging_tasks();

#if DO_COMBINED_REG_READ
	reg_buf = MEM_ALLOC(REG_BUF_LEN);
	assert(reg_buf);
#endif

	// display which SDIO mode we are operating in
#if H_SDIO_HOST_RX_MODE == H_SDIO_HOST_STREAMING_MODE
	ESP_LOGI(TAG, "SDIO Host operating in STREAMING MODE");
#else
	ESP_LOGI(TAG, "SDIO Host operating in PACKET MODE");
#endif

	ESP_LOGI(TAG, "generate slave intr");

	// inform the slave device that we are ready
	sdio_generate_slave_intr(ESP_OPEN_DATA_PATH);

	for (;;) {

		// wait for sdio interrupt from slave
		// call will block until there is an interrupt, timeout or error
		res = g_h.funcs->_h_sdio_wait_slave_intr(sdio_handle, HOSTED_BLOCK_MAX);

		if (res != ESP_OK) {
			ESP_LOGE(TAG, "wait_slave_intr error: %d", res);
			continue;
		}

		SDIO_DRV_LOCK();

#if DO_COMBINED_REG_READ
		if (sdio_read_valid_regs(reg_buf) != ESP_OK) {

			SDIO_DRV_UNLOCK();
			continue;
		}

		intr_index = (uint32_t *)&reg_buf[INT_RAW_INDEX];
#if H_SDIO_HOST_RX_MODE != H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE
		read_len_index = (uint32_t *)&reg_buf[PACKET_LEN_INDEX];
#endif

		interrupts = *intr_index;
#else
		// clear slave interrupts
		if (sdio_get_intr(&interrupts)) {
			ESP_LOGE(TAG, "failed to read interrupt register");

			SDIO_DRV_UNLOCK();
			ESP_LOGI(TAG, "Host is reseting itself, to avoid any sdio race condition");
			g_h.funcs->_h_restart_host();
			continue;
		}
#endif
		ESP_LOGV(TAG, "Intr: %08"PRIX32, interrupts);

		/* Validate packet length before acknowledging the interrupt. */
		if (BIT(SDIO_INT_NEW_PACKET) & interrupts) {
#if H_SDIO_HOST_RX_MODE == H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE
			len_from_slave = MAX_TRANSPORT_BUFFER_SIZE;
#else
#if DO_COMBINED_REG_READ
			ret = sdio_get_len_from_slave(&len_from_slave, *read_len_index, ACQUIRE_LOCK);
#else
			ret = sdio_get_len_from_slave(&len_from_slave, ACQUIRE_LOCK);
#endif
			if (ret || !len_from_slave) {
				ESP_LOGD(TAG, "invalid ret or len_from_slave: %d %ld", ret, len_from_slave);
				SDIO_DRV_UNLOCK();
				continue;
			}
#endif
		}

		ret = sdio_clear_intr(interrupts);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "failed to clear slave interrupt: %d", ret);
			SDIO_DRV_UNLOCK();
			continue;
		}

		/* Check all supported interrupts */
		if (BIT(SDIO_INT_START_THROTTLE) & interrupts) {
			if (!wifi_tx_throttling) {
				sdio_tx_throttle_start_count++;
				sdio_tx_throttle_start_us = esp_timer_get_time();
				ESP_LOGW(TAG,
					 "ESP-Hosted Wi-Fi TX throttle start: count=%" PRIu32,
					 sdio_tx_throttle_start_count);
			}
			wifi_tx_throttling = 1;
		}

		if (BIT(SDIO_INT_STOP_THROTTLE) & interrupts) {
			if (wifi_tx_throttling) {
				int64_t now_us = esp_timer_get_time();
				uint32_t duration_ms = sdio_tx_throttle_start_us > 0 ?
					(uint32_t)((now_us - sdio_tx_throttle_start_us) / 1000LL) : 0;

				sdio_tx_throttle_stop_count++;
				ESP_LOGW(TAG,
					 "ESP-Hosted Wi-Fi TX throttle stop: count=%" PRIu32 " duration=%" PRIu32 "ms",
					 sdio_tx_throttle_stop_count,
					 duration_ms);
			}
			wifi_tx_throttling = 0;
		}

		if (!(BIT(SDIO_INT_NEW_PACKET) & interrupts)) {

			SDIO_DRV_UNLOCK();
			continue;
		}
		/* Reserve one of the two stream buffers before starting the DMA read.
		 * If both batches are still being split, wait instead of consuming and
		 * then discarding a complete C6-to-P4 transfer. Release the bus while
		 * waiting so host-to-C6 traffic is not stalled by RX backpressure. */
		bool backpressured =
			g_h.funcs->_h_queue_msg_waiting(sdio_rx_free_buf_queue) == 0;
		int64_t wait_start_us = esp_timer_get_time();
		SDIO_DRV_UNLOCK();
		ret = g_h.funcs->_h_dequeue_item(sdio_rx_free_buf_queue,
							 &rx_buffer_index,
							 HOSTED_BLOCK_MAX);
		SDIO_DRV_LOCK();
		if (ret) {
			ESP_LOGE(TAG, "failed to reserve SDIO RX buffer");
			SDIO_DRV_UNLOCK();
			continue;
		}
		double_buf.write_index = rx_buffer_index;

		if (backpressured) {
			int64_t now_us = esp_timer_get_time();
			uint32_t wait_us = (uint32_t)(now_us - wait_start_us);
			sdio_rx_backpressure_count++;
			if (wait_us > sdio_rx_backpressure_max_us) {
				sdio_rx_backpressure_max_us = wait_us;
			}
			if (sdio_rx_backpressure_count == 1 ||
			    now_us - sdio_rx_backpressure_last_log_us >= 10000000LL) {
				ESP_LOGW(TAG,
					 "SDIO RX backpressure: waits=%lu wait=%luus max=%luus; batch preserved",
					 (unsigned long)sdio_rx_backpressure_count,
					 (unsigned long)wait_us,
					 (unsigned long)sdio_rx_backpressure_max_us);
				sdio_rx_backpressure_last_log_us = now_us;
			}
		}

		/* Allocate rx buffer */
		rxbuff = sdio_rx_get_buffer(len_from_slave);
		if (!rxbuff) {
			g_h.funcs->_h_queue_item(sdio_rx_free_buf_queue,
						   &rx_buffer_index,
						   HOSTED_BLOCK_MAX);
			SDIO_DRV_UNLOCK();
			vTaskDelay(pdMS_TO_TICKS(1));
			continue;
		}

		data_left = len_from_slave;
		pos = rxbuff;

		do {
			len_to_read = data_left;

#if H_SDIO_RX_BLOCK_ONLY_XFER
			/* Extend the transfer length to do block only transfers.
			 * This is safe as slave will pad data with 0, which we
			 * will ignore.
			 */
			uint32_t block_read_len = ((len_to_read + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;
			ret = g_h.funcs->_h_sdio_read_block(sdio_handle,
					ESP_SLAVE_CMD53_END_ADDR - data_left,
					pos, block_read_len, ACQUIRE_LOCK);
#else
			ret = g_h.funcs->_h_sdio_read_block(sdio_handle,
					ESP_SLAVE_CMD53_END_ADDR - data_left,
					pos, len_to_read, ACQUIRE_LOCK);
#endif
			if (ret) {
				ESP_LOGE(TAG, "%s: Failed to read data - %d %ld %ld",
					__func__, ret, len_to_read, data_left);
				sdio_rx_free_buffer(rxbuff);
				break;
			}
			data_left -= len_to_read;
			pos += len_to_read;
		} while (data_left);

		SDIO_DRV_UNLOCK();

		//TODO: unclear, on failure case
		//sdio_rx_byte_count += (len_from_slave-data_left);
		sdio_rx_byte_count += len_from_slave;
		sdio_rx_byte_count = sdio_rx_byte_count % ESP_RX_BYTE_MAX;

		if (unlikely(ret))
		{
			g_h.funcs->_h_queue_item(sdio_rx_free_buf_queue,
						   &rx_buffer_index,
						   HOSTED_BLOCK_MAX);
			continue;
		}

		sdio_rx_batch_t batch = {
			.buffer_index = rx_buffer_index,
			.data_len = len_from_slave,
		};
		if (g_h.funcs->_h_queue_item(sdio_rx_ready_buf_queue,
						  &batch,
						  HOSTED_BLOCK_MAX)) {
			ESP_LOGE(TAG, "failed to queue completed SDIO RX batch");
			g_h.funcs->_h_queue_item(sdio_rx_free_buf_queue,
						   &rx_buffer_index,
						   HOSTED_BLOCK_MAX);
		}
	}
}

/**
 * TODO: unify sdio_process_rx_task() and spi_process_rx_task()
 */
static void sdio_process_rx_task(void const* pvParameters)
{
	interface_buffer_handle_t buf_handle_l = {0};
	interface_buffer_handle_t *buf_handle = NULL;
	int ret = 0;

	struct esp_priv_event *event = NULL;

	while (true) {
		vTaskDelay(pdMS_TO_TICKS(100));
		if (is_transport_rx_ready()) {
			break;
		}
	}
	ESP_LOGI(TAG, "Starting SDIO process rx task");

	while (1) {
		g_h.funcs->_h_get_semaphore(sem_from_slave_queue, HOSTED_BLOCK_MAX);

		if (g_h.funcs->_h_dequeue_item(from_slave_queue[PRIO_Q_SERIAL], &buf_handle_l, 0))
			if (g_h.funcs->_h_dequeue_item(from_slave_queue[PRIO_Q_BT], &buf_handle_l, 0))
				if (g_h.funcs->_h_dequeue_item(from_slave_queue[PRIO_Q_OTHERS], &buf_handle_l, 0)) {
					ESP_LOGI(TAG, "No element in any queue found");
					continue;
				}

		buf_handle = &buf_handle_l;

		ESP_LOGV(TAG, "h_sdio_rx: iftype:%d", (int)buf_handle->if_type);
		ESP_HEXLOGV("h_sdio_rx", buf_handle->payload, min(buf_handle->payload_len,32));

		if (buf_handle->if_type == ESP_SERIAL_IF) {
			/* serial interface path */
			serial_rx_handler(buf_handle);
		} else if((buf_handle->if_type == ESP_STA_IF) ||
				(buf_handle->if_type == ESP_AP_IF)) {
#if 1
			if (chan_arr[buf_handle->if_type] && chan_arr[buf_handle->if_type]->rx) {
				/* TODO : Need to abstract heap_caps_malloc */
				if (!buf_handle->payload_len || !buf_handle->payload) {
					ESP_LOGW(TAG, "Drop SDIO RX packet: invalid payload len=%u payload=%p",
						 buf_handle->payload_len,
						 buf_handle->payload);
					H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle,
						buf_handle->priv_buffer_handle);
					continue;
				}
				if (buf_handle->payload_zcopy) {
					ret = chan_arr[buf_handle->if_type]->rx(
						chan_arr[buf_handle->if_type]->api_chan,
						buf_handle->payload,
						buf_handle->priv_buffer_handle,
						buf_handle->payload_len);
					if (unlikely(ret)) {
						H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle,
							buf_handle->priv_buffer_handle);
					}
					continue;
				}

				uint8_t * copy_payload = (uint8_t *)g_h.funcs->_h_malloc(buf_handle->payload_len);
				if (!copy_payload) {
					sdio_rx_copy_drop_count++;
					sdio_log_dma_no_mem("rx_copy",
							    buf_handle->payload_len,
							    sdio_rx_copy_drop_count);
					H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle,
						buf_handle->priv_buffer_handle);
					continue;
				}
				memcpy(copy_payload, buf_handle->payload, buf_handle->payload_len);
				H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle, buf_handle->priv_buffer_handle);

				ret = chan_arr[buf_handle->if_type]->rx(chan_arr[buf_handle->if_type]->api_chan,
						copy_payload, copy_payload, buf_handle->payload_len);
				if (unlikely(ret))
					HOSTED_FREE(copy_payload);
			}
#else
			if (chan_arr[buf_handle->if_type] && chan_arr[buf_handle->if_type]->rx) {
				chan_arr[buf_handle->if_type]->rx(chan_arr[buf_handle->if_type]->api_chan,
						buf_handle->payload, NULL, buf_handle->payload_len);
			}
#endif
		} else if (buf_handle->if_type == ESP_PRIV_IF) {
			process_priv_communication(buf_handle);
			hci_drv_show_configuration();
			/* priv transaction received */
			ESP_LOGI(TAG, "Received INIT event");
			sdio_start_write_thread = true;

			event = (struct esp_priv_event *) (buf_handle->payload);
			if (event->event_type != ESP_PRIV_EVENT_INIT) {
				/* User can re-use this type of transaction */
			}
		} else if (buf_handle->if_type == ESP_HCI_IF) {
			hci_rx_handler(buf_handle);
		} else if (buf_handle->if_type == ESP_TEST_IF) {
#if TEST_RAW_TP
			update_test_raw_tp_rx_len(buf_handle->payload_len +
				H_ESP_PAYLOAD_HEADER_OFFSET);
#endif
		} else {
			ESP_LOGW(TAG, "unknown type %d ", buf_handle->if_type);
		}

		/* Free buffer handle */
		/* When buffer offloaded to other module, that module is
		 * responsible for freeing buffer. In case not offloaded or
		 * failed to offload, buffer should be freed here.
		 */
		if (!buf_handle->payload_zcopy) {
			H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle,
				buf_handle->priv_buffer_handle);
		}
	}
}

void transport_init_internal(void)
{
	uint8_t prio_q_idx = 0;
	/* register callback */

	sdio_bus_lock = g_h.funcs->_h_create_mutex();
	assert(sdio_bus_lock);

	sem_to_slave_queue = g_h.funcs->_h_create_semaphore(TO_SLAVE_QUEUE_SIZE*MAX_PRIORITY_QUEUES);
	assert(sem_to_slave_queue);
	g_h.funcs->_h_get_semaphore(sem_to_slave_queue, 0);

	sem_from_slave_queue = g_h.funcs->_h_create_semaphore(FROM_SLAVE_QUEUE_SIZE*MAX_PRIORITY_QUEUES);
	assert(sem_from_slave_queue);
	g_h.funcs->_h_get_semaphore(sem_from_slave_queue, 0);

	/* cleanup the semaphores */


	for (prio_q_idx=0; prio_q_idx<MAX_PRIORITY_QUEUES;prio_q_idx++) {
		/* Queue - rx */
		from_slave_queue[prio_q_idx] = g_h.funcs->_h_create_queue(FROM_SLAVE_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
		assert(from_slave_queue[prio_q_idx]);

		/* Queue - tx */
		to_slave_queue[prio_q_idx] = g_h.funcs->_h_create_queue(TO_SLAVE_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
		assert(to_slave_queue[prio_q_idx]);
	}

	sdio_mempool_create();

	/* initialise SDMMC before starting read/write threads
	 * which depend on SDMMC*/
	sdio_handle = g_h.funcs->_h_bus_init();
	if (!sdio_handle) {
		ESP_LOGE(TAG, "could not create sdio handle, exiting\n");
		assert(sdio_handle);
	}

	// initialise double buffering structs
	memset(&double_buf, 0, sizeof(double_buf_t));
	double_buf.write_index = 0; // we will write into the first buffer
	sdio_prealloc_stream_rx_buffers();

	sdio_rx_free_buf_queue =
		g_h.funcs->_h_create_queue(2, sizeof(int));
	sdio_rx_ready_buf_queue =
		g_h.funcs->_h_create_queue(2, sizeof(sdio_rx_batch_t));
	assert(sdio_rx_free_buf_queue);
	assert(sdio_rx_ready_buf_queue);
	for (int index = 0; index < 2; ++index) {
		assert(g_h.funcs->_h_queue_item(sdio_rx_free_buf_queue,
						       &index, 0) == 0);
	}

	sdio_rx_buf_thread = g_h.funcs->_h_thread_create("sdio_rx_buf",
		SDIO_RX_BUF_TASK_PRIORITY, DFLT_TASK_STACK_SIZE, sdio_data_to_rx_buf_task, NULL);

	sdio_read_thread = g_h.funcs->_h_thread_create("sdio_read",
		SDIO_READ_TASK_PRIORITY, DFLT_TASK_STACK_SIZE, sdio_read_task, NULL);

	sdio_process_rx_thread = g_h.funcs->_h_thread_create("sdio_process_rx",
		SDIO_DATA_PATH_TASK_PRIORITY, DFLT_TASK_STACK_SIZE, sdio_process_rx_task, NULL);

	sdio_write_thread = g_h.funcs->_h_thread_create("sdio_write",
		SDIO_DATA_PATH_TASK_PRIORITY, DFLT_TASK_STACK_SIZE, sdio_write_task, NULL);

	/* sdio_bus_lock was created before workers; do not replace/leak it here. */
}

int esp_hosted_tx(uint8_t iface_type, uint8_t iface_num,
		uint8_t * wbuffer, uint16_t wlen, uint8_t buff_zcopy,
		void (*free_wbuf_fun)(void* ptr))
{
	interface_buffer_handle_t buf_handle = {0};
	void (*free_func)(void* ptr) = NULL;
	uint8_t pkt_prio = PRIO_Q_OTHERS;
	uint8_t transport_up = is_transport_tx_ready();

	if (free_wbuf_fun)
		free_func = free_wbuf_fun;

	if (!wbuffer || !wlen ||
		(wlen > MAX_PAYLOAD_SIZE) ||
		!transport_up) {
		ESP_LOGE(TAG, "tx fail: NULL buff, invalid len (%u) or len > max len (%u), transport_up(%u))",
				wlen, MAX_PAYLOAD_SIZE, transport_up);
		H_FREE_PTR_WITH_FUNC(free_func, wbuffer);
		return ESP_FAIL;
	}
	buf_handle.payload_zcopy = buff_zcopy;
	buf_handle.if_type = iface_type;
	buf_handle.if_num = iface_num;
	buf_handle.payload_len = wlen;
	buf_handle.payload = wbuffer;
	buf_handle.priv_buffer_handle = wbuffer;
	buf_handle.free_buf_handle = free_func;

	if (buf_handle.if_type == ESP_SERIAL_IF)
		pkt_prio = PRIO_Q_SERIAL;
	else if (buf_handle.if_type == ESP_HCI_IF)
		pkt_prio = PRIO_Q_BT;
	/* else OTHERS by default */

	g_h.funcs->_h_queue_item(to_slave_queue[pkt_prio], &buf_handle, HOSTED_BLOCK_MAX);
	g_h.funcs->_h_post_semaphore(sem_to_slave_queue);

#if ESP_PKT_STATS
	if (buf_handle.if_type == ESP_STA_IF)
		pkt_stats.sta_tx_in_pass++;
#endif

	return ESP_OK;
}
