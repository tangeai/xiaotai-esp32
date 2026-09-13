#include "camera_driver.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_private/dw_gdma.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "sdkconfig.h"

#include "app_memory_policy.h"
#include "app_log_policy.h"
#include "hardware_board.h"
#include "hardware_board_config.h"

static const char *TAG = "camera";

#define CAMERA_DRIVER_FRAME_TIMEOUT_MS 3000
#define CAMERA_DRIVER_FRAME_BUSY_WAIT_MS 5
#define CAMERA_DRIVER_FRAME_POLL_WAIT_MS 1
#define CAMERA_DRIVER_TARGET_FORMAT    V4L2_PIX_FMT_YUV420
#define CAMERA_DRIVER_MEMORY_TYPE      V4L2_MEMORY_USERPTR
#define CAMERA_DRIVER_BUFFER_ALIGNMENT 64U
#define CAMERA_DRIVER_MAX_FRAME_BYTES  \
	((size_t)HARDWARE_BOARD_CAMERA_WIDTH * HARDWARE_BOARD_CAMERA_HEIGHT * 3U / 2U)

typedef struct {
	struct v4l2_buffer buffer;
} camera_driver_frame_owner_t;

static bool s_video_ready;
static bool s_camera_initialized;
static bool s_streaming;
static bool s_buffers_requested;
static bool s_first_frame_logged;
static bool s_format_list_logged;
static int s_fd = -1;
static struct v4l2_format s_active_format;
static void *s_buffers[HARDWARE_BOARD_CAMERA_BUFFER_COUNT];
static size_t s_buffer_capacities[HARDWARE_BOARD_CAMERA_BUFFER_COUNT];
static bool s_persistent_pool_ready;
static SemaphoreHandle_t s_lock;
static camera_driver_frame_owner_t s_active_frame;
static bool s_frame_outstanding;
static uint32_t s_client_count;
static uint16_t s_target_width = HARDWARE_BOARD_CAMERA_WIDTH;
static uint16_t s_target_height = HARDWARE_BOARD_CAMERA_HEIGHT;
static uint8_t s_target_fps = 30;
static uint8_t s_sensor_fps;
static uint32_t s_last_delivered_sequence;
static bool s_last_delivered_sequence_valid;
static dw_gdma_channel_handle_t s_csi_dma_clock_guard;

static esp_err_t camera_driver_prepare_persistent_buffers(void)
{
	if (s_persistent_pool_ready) {
		return ESP_OK;
	}

	size_t allocated = 0U;

	for (uint32_t i = 0; i < HARDWARE_BOARD_CAMERA_BUFFER_COUNT; ++i) {
		if (s_buffers[i] != NULL &&
		    s_buffer_capacities[i] >= CAMERA_DRIVER_MAX_FRAME_BYTES) {
			allocated += s_buffer_capacities[i];
			continue;
		}

		void *buffer = app_memory_aligned_alloc_psram(
			CAMERA_DRIVER_BUFFER_ALIGNMENT,
			CAMERA_DRIVER_MAX_FRAME_BYTES,
			MALLOC_CAP_CACHE_ALIGNED);
		if (buffer == NULL) {
			for (uint32_t cleanup = 0;
			     cleanup < HARDWARE_BOARD_CAMERA_BUFFER_COUNT;
			     ++cleanup) {
				heap_caps_free(s_buffers[cleanup]);
				s_buffers[cleanup] = NULL;
				s_buffer_capacities[cleanup] = 0U;
			}
			ESP_LOGE(TAG,
				 "reserve camera PSRAM buffer failed: index=%u size=%u psram_free=%u psram_largest=%u",
				 (unsigned)i,
				 (unsigned)CAMERA_DRIVER_MAX_FRAME_BYTES,
				 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
				 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
			return ESP_ERR_NO_MEM;
		}

		s_buffers[i] = buffer;
		s_buffer_capacities[i] = CAMERA_DRIVER_MAX_FRAME_BYTES;
		allocated += CAMERA_DRIVER_MAX_FRAME_BYTES;
	}

	s_persistent_pool_ready = true;
	APP_LOG_DETAIL(TAG,
		       "camera PSRAM pool reserved: count=%u frame_capacity=%u total=%u psram_free=%u psram_largest=%u",
		       (unsigned)HARDWARE_BOARD_CAMERA_BUFFER_COUNT,
		       (unsigned)CAMERA_DRIVER_MAX_FRAME_BYTES,
		       (unsigned)allocated,
		       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
		       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
	return ESP_OK;
}

static bool camera_driver_bind_user_buffer(struct v4l2_buffer *buf)
{
	if (buf == NULL || buf->index >= HARDWARE_BOARD_CAMERA_BUFFER_COUNT ||
	    s_buffers[buf->index] == NULL ||
	    s_buffer_capacities[buf->index] < s_active_format.fmt.pix.sizeimage) {
		return false;
	}

	buf->memory = CAMERA_DRIVER_MEMORY_TYPE;
	buf->m.userptr = (unsigned long)s_buffers[buf->index];
	buf->length = s_buffer_capacities[buf->index];
	return true;
}

static esp_err_t camera_driver_hold_csi_dma_clock(void)
{
	if (s_csi_dma_clock_guard != NULL) {
		return ESP_OK;
	}

	/*
	 * ESP-IDF 5.5.4 releases the last DW-GDMA group reference before it
	 * frees that channel's shared interrupt. A pending CSI interrupt can
	 * therefore enter shared_intr_isr after the register clock is gated.
	 * Keep one unused channel for the application lifetime so camera stream
	 * teardown can free its interrupt while the DW-GDMA registers stay live.
	 */
	const dw_gdma_channel_alloc_config_t config = {
		.src = {
			.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
			.role = DW_GDMA_ROLE_MEM,
			.handshake_type = DW_GDMA_HANDSHAKE_SW,
			.num_outstanding_requests = 1,
		},
		.dst = {
			.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
			.role = DW_GDMA_ROLE_MEM,
			.handshake_type = DW_GDMA_HANDSHAKE_SW,
			.num_outstanding_requests = 1,
		},
		.flow_controller = DW_GDMA_FLOW_CTRL_SELF,
		.chan_priority = 0,
	};

	ESP_RETURN_ON_ERROR(dw_gdma_new_channel(&config, &s_csi_dma_clock_guard),
			    TAG, "reserve CSI DW-GDMA clock guard failed");
	ESP_LOGI(TAG, "CSI DW-GDMA teardown guard ready");
	return ESP_OK;
}

static void camera_driver_format_to_str(uint32_t format, char out[5])
{
	out[0] = (char)(format & 0xff);
	out[1] = (char)((format >> 8) & 0xff);
	out[2] = (char)((format >> 16) & 0xff);
	out[3] = (char)((format >> 24) & 0xff);
	out[4] = '\0';
}

static void camera_driver_prepare_pix_format(struct v4l2_format *format, uint32_t pixelformat)
{
	format->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format->fmt.pix.width = s_target_width;
	format->fmt.pix.height = s_target_height;
	format->fmt.pix.pixelformat = pixelformat;
	format->fmt.pix.field = V4L2_FIELD_NONE;
	format->fmt.pix.colorspace = V4L2_COLORSPACE_DEFAULT;
	format->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->fmt.pix.quantization = V4L2_QUANTIZATION_DEFAULT;
	format->fmt.pix.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	format->fmt.pix.bytesperline = 0;
	format->fmt.pix.sizeimage = 0;
	if (pixelformat == V4L2_PIX_FMT_YUV420) {
		format->fmt.pix.bytesperline = s_target_width;
		format->fmt.pix.sizeimage = s_target_width * s_target_height * 3 / 2;
	}
}

static void camera_driver_log_supported_formats(void)
{
	if (s_format_list_logged) {
		return;
	}
	s_format_list_logged = true;

	for (uint32_t index = 0; index < 8; ++index) {
		struct v4l2_fmtdesc desc = {
			.index = index,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		};
		if (ioctl(s_fd, VIDIOC_ENUM_FMT, &desc) != 0) {
			if (index == 0) {
				ESP_LOGW(TAG, "camera format enum unsupported errno=%d", errno);
			}
			break;
		}
		char fourcc[5] = {0};
		camera_driver_format_to_str(desc.pixelformat, fourcc);
		ESP_LOGD(TAG, "camera format enum[%u]: %s %s",
			 (unsigned)index,
			 fourcc,
			 (const char *)desc.description);
	}
}

static esp_err_t camera_driver_init_lock(void)
{
	if (s_lock != NULL) {
		return ESP_OK;
	}
	s_lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
	return s_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t camera_driver_video_init_once(void)
{
	if (s_video_ready) {
		return ESP_OK;
	}

	ESP_RETURN_ON_ERROR(camera_driver_hold_csi_dma_clock(), TAG,
			    "CSI DMA guard init failed");

	ESP_RETURN_ON_ERROR(hardware_board_init_i2c(), TAG, "camera i2c init failed");
	i2c_master_bus_handle_t i2c_bus = hardware_board_get_i2c_bus_handle();
	ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "camera i2c handle missing");

	const esp_video_init_csi_config_t csi_config = {
		.sccb_config = {
			.init_sccb = false,
			.i2c_handle = i2c_bus,
			.freq = HARDWARE_BOARD_CAMERA_I2C_FREQ_HZ,
		},
		.reset_pin = -1,
		.pwdn_pin = -1,
	};
	const esp_video_init_config_t video_config = {
		.csi = &csi_config,
	};

	ESP_RETURN_ON_ERROR(esp_video_init(&video_config), TAG, "esp_video init failed");
	s_video_ready = true;
	APP_LOG_DETAIL(TAG,
		       "esp_video ready: dev=%s i2c=%d scl=%d sda=%d",
		       ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
		       (int)HARDWARE_BOARD_I2C_NUM,
		       (int)HARDWARE_BOARD_I2C_SCL,
		       (int)HARDWARE_BOARD_I2C_SDA);
	return ESP_OK;
}

static esp_err_t camera_driver_open_device(void)
{
	struct v4l2_capability capability = {0};

	if (s_fd >= 0) {
		return ESP_OK;
	}

	s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY | O_NONBLOCK);
	ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_FAIL, TAG, "open %s failed errno=%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno);

	if (ioctl(s_fd, VIDIOC_QUERYCAP, &capability) != 0) {
		ESP_LOGE(TAG, "query camera capability failed errno=%d", errno);
		close(s_fd);
		s_fd = -1;
		return ESP_FAIL;
	}

	APP_LOG_DETAIL(TAG,
		       "camera capability: driver=%s card=%s bus=%s caps=0x%08" PRIx32,
		       capability.driver,
		       capability.card,
		       capability.bus_info,
		       capability.capabilities);
	return ESP_OK;
}

static esp_err_t camera_driver_config_format(void)
{
	struct v4l2_format format = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
	};

	if (ioctl(s_fd, VIDIOC_G_FMT, &format) != 0) {
		ESP_LOGE(TAG, "get camera format failed errno=%d", errno);
		return ESP_FAIL;
	}

	camera_driver_log_supported_formats();

	if (format.fmt.pix.width != s_target_width ||
	    format.fmt.pix.height != s_target_height ||
	    format.fmt.pix.pixelformat != CAMERA_DRIVER_TARGET_FORMAT) {
		struct v4l2_format target = format;
		camera_driver_prepare_pix_format(&target, CAMERA_DRIVER_TARGET_FORMAT);
		const int set_ret = ioctl(s_fd, VIDIOC_S_FMT, &target);
		const int set_errno = errno;
		if (set_ret == 0) {
			format = target;
			if (ioctl(s_fd, VIDIOC_G_FMT, &format) != 0) {
				ESP_LOGE(TAG, "get active camera format failed errno=%d", errno);
				return ESP_FAIL;
			}
		} else {
			char target_fourcc[5] = {0};
			camera_driver_format_to_str(CAMERA_DRIVER_TARGET_FORMAT, target_fourcc);
			ESP_LOGW(TAG,
				 "set camera %s %ux%u failed ret=%d errno=%d cs=%u ycbcr=%u quant=%u xfer=%u, keep current format",
				 target_fourcc,
				 s_target_width,
				 s_target_height,
				 set_ret,
				 set_errno,
				 (unsigned)target.fmt.pix.colorspace,
				 (unsigned)target.fmt.pix.ycbcr_enc,
				 (unsigned)target.fmt.pix.quantization,
				 (unsigned)target.fmt.pix.xfer_func);
		}
	}

	char fourcc[5] = {0};
	camera_driver_format_to_str(format.fmt.pix.pixelformat, fourcc);
	ESP_LOGI(TAG,
		 "camera output format: %ux%u %s sizeimage=%u bytesperline=%u",
		 (unsigned)format.fmt.pix.width,
		 (unsigned)format.fmt.pix.height,
		 fourcc,
		 (unsigned)format.fmt.pix.sizeimage,
		 (unsigned)format.fmt.pix.bytesperline);

	if (format.fmt.pix.pixelformat != CAMERA_DRIVER_TARGET_FORMAT) {
		return ESP_ERR_NOT_SUPPORTED;
	}

	s_active_format = format;
	return ESP_OK;
}

static void camera_driver_log_sensor_format(void)
{
	esp_cam_sensor_format_t sensor_format = {0};

	if (ioctl(s_fd, VIDIOC_G_SENSOR_FMT, &sensor_format) != 0) {
		APP_LOG_DETAIL(TAG, "camera sensor format query unsupported errno=%d", errno);
		return;
	}

	s_sensor_fps = sensor_format.fps;
	ESP_LOGI(TAG,
		 "camera sensor format: name=%s size=%ux%u fps=%u pixfmt=%u xclk=%d mipi_clk=%llu lanes=%u line_sync=%d",
		 sensor_format.name != NULL ? sensor_format.name : "unknown",
		 (unsigned)sensor_format.width,
		 (unsigned)sensor_format.height,
		 (unsigned)sensor_format.fps,
		 (unsigned)sensor_format.format,
		 sensor_format.xclk,
		 (unsigned long long)sensor_format.mipi_info.mipi_clk,
		 (unsigned)sensor_format.mipi_info.lane_num,
		 sensor_format.mipi_info.line_sync_en ? 1 : 0);
}

static uint64_t camera_driver_buffer_timestamp_us(const struct v4l2_buffer *buf)
{
	if (buf == NULL) {
		return 0;
	}
	return ((uint64_t)buf->timestamp.tv_sec * 1000000ULL) + (uint64_t)buf->timestamp.tv_usec;
}

static bool camera_driver_sequence_is_newer(uint32_t candidate, uint32_t reference)
{
	return (int32_t)(candidate - reference) > 0;
}

static bool camera_driver_buffer_is_usable(const struct v4l2_buffer *buf)
{
	return buf != NULL &&
	       buf->index < HARDWARE_BOARD_CAMERA_BUFFER_COUNT &&
	       s_buffers[buf->index] != NULL &&
	       s_buffer_capacities[buf->index] >= s_active_format.fmt.pix.sizeimage;
}

static void camera_driver_requeue_buffer(const struct v4l2_buffer *buf, const char *reason)
{
	if (buf == NULL || s_fd < 0) {
		return;
	}
	struct v4l2_buffer queued = *buf;
	if (!camera_driver_bind_user_buffer(&queued)) {
		ESP_LOGE(TAG,
			 "bind camera %s buffer failed index=%u",
			 reason != NULL ? reason : "invalid",
			 (unsigned)buf->index);
		return;
	}
	if (ioctl(s_fd, VIDIOC_QBUF, &queued) != 0) {
		ESP_LOGW(TAG, "requeue camera %s buffer failed index=%u errno=%d",
			 reason != NULL ? reason : "invalid",
			 (unsigned)buf->index,
			 errno);
	}
}

static void camera_driver_config_frame_rate(void)
{
	struct v4l2_streamparm parm = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
	};

	if (ioctl(s_fd, VIDIOC_G_PARM, &parm) != 0) {
		ESP_LOGI(TAG,
			 "camera cadence: sensor=%ufps target=%ufps pacing=application",
			 (unsigned)s_sensor_fps,
			 (unsigned)s_target_fps);
		return;
	}

	uint32_t before_num = parm.parm.capture.timeperframe.numerator;
	uint32_t before_den = parm.parm.capture.timeperframe.denominator;
	APP_LOG_DETAIL(TAG,
		       "camera frame interval before: numerator=%u denominator=%u capability=0x%08" PRIx32,
		 (unsigned)before_num,
		 (unsigned)before_den,
		 parm.parm.capture.capability);

	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = s_target_fps;
	if (ioctl(s_fd, VIDIOC_S_PARM, &parm) != 0) {
		ESP_LOGW(TAG, "set camera frame interval 1/%u failed errno=%d", (unsigned)s_target_fps, errno);
		return;
	}

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(s_fd, VIDIOC_G_PARM, &parm) == 0) {
		ESP_LOGI(TAG,
			 "camera frame interval: requested=1/%u active=%u/%u capability=0x%08" PRIx32,
			 (unsigned)s_target_fps,
			 (unsigned)parm.parm.capture.timeperframe.numerator,
			 (unsigned)parm.parm.capture.timeperframe.denominator,
			 parm.parm.capture.capability);
	}
}

static esp_err_t camera_driver_prepare_buffers(void)
{
	ESP_RETURN_ON_ERROR(camera_driver_prepare_persistent_buffers(),
			    TAG, "camera PSRAM pool unavailable");

	struct v4l2_requestbuffers req = {
		.count = HARDWARE_BOARD_CAMERA_BUFFER_COUNT,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = CAMERA_DRIVER_MEMORY_TYPE,
	};

	if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) {
		app_memory_snapshot_t memory = {0};
		const size_t frame_bytes = s_active_format.fmt.pix.sizeimage;
		const size_t requested_bytes =
			frame_bytes * HARDWARE_BOARD_CAMERA_BUFFER_COUNT;

		app_memory_get_snapshot(&memory);
		ESP_LOGE(TAG,
			 "request camera buffers failed errno=%d count=%u frame=%u total=%u "
			 "psram_free=%u psram_largest=%u",
			 errno,
			 (unsigned)HARDWARE_BOARD_CAMERA_BUFFER_COUNT,
			 (unsigned)frame_bytes,
			 (unsigned)requested_bytes,
			 (unsigned)memory.psram_free,
			 (unsigned)memory.psram_largest);
		return ESP_FAIL;
	}
	s_buffers_requested = true;
	if (req.count < HARDWARE_BOARD_CAMERA_BUFFER_COUNT) {
		ESP_LOGE(TAG, "camera returned fewer buffers: requested=%u actual=%u",
			 (unsigned)HARDWARE_BOARD_CAMERA_BUFFER_COUNT,
			 (unsigned)req.count);
		return ESP_ERR_NO_MEM;
	}

	for (uint32_t i = 0; i < HARDWARE_BOARD_CAMERA_BUFFER_COUNT; ++i) {
		struct v4l2_buffer buf = {
			.index = i,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = CAMERA_DRIVER_MEMORY_TYPE,
		};

		if (ioctl(s_fd, VIDIOC_QUERYBUF, &buf) != 0) {
			ESP_LOGE(TAG, "query camera buffer %" PRIu32 " failed errno=%d", i, errno);
			return ESP_FAIL;
		}

		if (!camera_driver_bind_user_buffer(&buf)) {
			ESP_LOGE(TAG,
				 "camera PSRAM buffer too small: index=%" PRIu32
				 " required=%u capacity=%u",
				 i,
				 (unsigned)buf.length,
				 (unsigned)s_buffer_capacities[i]);
			return ESP_ERR_INVALID_SIZE;
		}

		if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
			ESP_LOGE(TAG, "queue camera buffer %" PRIu32 " failed errno=%d", i, errno);
			return ESP_FAIL;
		}
	}

	APP_LOG_DETAIL(TAG,
		       "camera USERPTR buffers ready: count=%u frame=%u capacity=%u",
		 (unsigned)HARDWARE_BOARD_CAMERA_BUFFER_COUNT,
		 (unsigned)s_active_format.fmt.pix.sizeimage,
		 (unsigned)s_buffer_capacities[0]);
	return ESP_OK;
}

static esp_err_t camera_driver_stream_on(void)
{
	if (s_streaming) {
		return ESP_OK;
	}

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
		ESP_LOGE(TAG, "camera stream on failed errno=%d", errno);
		return ESP_FAIL;
	}

	s_streaming = true;
	APP_LOG_DETAIL(TAG, "camera stream on");
	return ESP_OK;
}

static void camera_driver_cleanup(void)
{
	if (s_streaming && s_fd >= 0) {
		int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(s_fd, VIDIOC_STREAMOFF, &type) != 0) {
			ESP_LOGW(TAG, "camera stream off failed errno=%d", errno);
		}
	}
	s_streaming = false;

	/*
	 * Only the V4L2 queue metadata is session-owned. The application-owned
	 * USERPTR buffers stay reserved across app switches so later high-resolution
	 * starts never depend on finding three new contiguous PSRAM blocks.
	 */
	if (s_fd >= 0 && s_buffers_requested) {
		struct v4l2_requestbuffers release_req = {
			.count = 0,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = CAMERA_DRIVER_MEMORY_TYPE,
		};
		if (ioctl(s_fd, VIDIOC_REQBUFS, &release_req) != 0) {
			ESP_LOGW(TAG, "release camera buffers failed errno=%d", errno);
		} else {
			APP_LOG_DETAIL(TAG, "camera buffers released");
		}
		s_buffers_requested = false;
	}

	if (s_fd >= 0) {
		close(s_fd);
		s_fd = -1;
	}
	s_frame_outstanding = false;
	s_sensor_fps = 0U;
	s_last_delivered_sequence = 0U;
	s_last_delivered_sequence_valid = false;
	memset(&s_active_frame, 0, sizeof(s_active_frame));
	memset(&s_active_format, 0, sizeof(s_active_format));
	s_camera_initialized = false;
	s_first_frame_logged = false;
}

bool camera_driver_is_configured(void)
{
	return HARDWARE_BOARD_CAMERA_ENABLED != 0;
}

esp_err_t camera_driver_prepare_video_subsystem(void)
{
	ESP_RETURN_ON_ERROR(camera_driver_init_lock(), TAG, "camera lock init failed");

	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	esp_err_t ret = camera_driver_prepare_persistent_buffers();
	if (ret == ESP_OK) {
		ret = hardware_board_set_camera_power(true);
	}
	if (ret == ESP_OK) {
		ret = camera_driver_video_init_once();
	}

	xSemaphoreGive(s_lock);
	return ret;
}

esp_err_t camera_driver_set_stream_target(uint16_t width, uint16_t height, uint8_t fps)
{
	if (width < 320U) {
		width = 320U;
	} else if (width > HARDWARE_BOARD_CAMERA_WIDTH) {
		width = HARDWARE_BOARD_CAMERA_WIDTH;
	}
	if (height < 240U) {
		height = 240U;
	} else if (height > HARDWARE_BOARD_CAMERA_HEIGHT) {
		height = HARDWARE_BOARD_CAMERA_HEIGHT;
	}
	if ((width & 1U) != 0U) {
		width--;
	}
	if ((height & 1U) != 0U) {
		height--;
	}
	if (fps < 5U) {
		fps = 5U;
	} else if (fps > 30U) {
		fps = 30U;
	}

	ESP_RETURN_ON_ERROR(camera_driver_init_lock(), TAG, "camera lock init failed");
	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	if (s_target_width == width && s_target_height == height && s_target_fps == fps) {
		xSemaphoreGive(s_lock);
		return ESP_OK;
	}
	if (s_client_count > 0U) {
		ESP_LOGW(TAG,
			 "camera target change rejected while in use: clients=%" PRIu32
			 " active=%ux%u@%u requested=%ux%u@%u",
			 s_client_count,
			 (unsigned)s_target_width,
			 (unsigned)s_target_height,
			 (unsigned)s_target_fps,
			 (unsigned)width,
			 (unsigned)height,
			 (unsigned)fps);
		xSemaphoreGive(s_lock);
		return ESP_ERR_INVALID_STATE;
	}
	if (s_frame_outstanding) {
		xSemaphoreGive(s_lock);
		return ESP_ERR_INVALID_STATE;
	}

	uint16_t old_width = s_target_width;
	uint16_t old_height = s_target_height;
	uint8_t old_fps = s_target_fps;
	s_target_width = width;
	s_target_height = height;
	s_target_fps = fps;
	if (s_camera_initialized) {
		camera_driver_cleanup();
	}
	xSemaphoreGive(s_lock);

	ESP_LOGI(TAG,
		 "camera stream target: %ux%u@%u -> %ux%u@%u",
		 (unsigned)old_width,
		 (unsigned)old_height,
		 (unsigned)old_fps,
		 (unsigned)width,
		 (unsigned)height,
		 (unsigned)fps);
	return ESP_OK;
}

esp_err_t camera_driver_init(void)
{
	ESP_RETURN_ON_ERROR(camera_driver_init_lock(), TAG, "camera lock init failed");

	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	esp_err_t ret = ESP_OK;
	if (s_camera_initialized) {
		xSemaphoreGive(s_lock);
		return ESP_OK;
	}

	ret = camera_driver_prepare_persistent_buffers();
	if (ret == ESP_OK) {
		ret = hardware_board_set_camera_power(true);
	}
	if (ret == ESP_OK) {
		ret = camera_driver_video_init_once();
	}
	if (ret == ESP_OK) {
		ret = camera_driver_open_device();
	}
	if (ret == ESP_OK) {
		ret = camera_driver_config_format();
	}
	if (ret == ESP_OK) {
		camera_driver_log_sensor_format();
	}
	if (ret == ESP_OK) {
		camera_driver_config_frame_rate();
	}
	if (ret == ESP_OK) {
		ret = camera_driver_prepare_buffers();
	}
	if (ret == ESP_OK) {
		ret = camera_driver_stream_on();
	}

	if (ret == ESP_OK) {
		s_camera_initialized = true;
		ESP_LOGI(TAG, "camera ready");
	} else {
		camera_driver_cleanup();
		(void)hardware_board_set_camera_power(false);
	}

	xSemaphoreGive(s_lock);
	return ret;
}

esp_err_t camera_driver_acquire(void)
{
	ESP_RETURN_ON_ERROR(camera_driver_init(), TAG, "camera init failed");

	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	s_client_count++;
	ESP_LOGD(TAG, "camera acquire: clients=%" PRIu32, s_client_count);
	xSemaphoreGive(s_lock);
	return ESP_OK;
}

void camera_driver_release_device(void)
{
	if (s_lock == NULL) {
		return;
	}
	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
		return;
	}

	if (s_client_count > 0) {
		s_client_count--;
	}
	bool cleanup = s_client_count == 0;
	ESP_LOGD(TAG, "camera release device: clients=%" PRIu32, s_client_count);
	if (cleanup) {
		camera_driver_cleanup();
		(void)hardware_board_set_camera_power(false);
	}
	xSemaphoreGive(s_lock);
}

esp_err_t camera_driver_capture(camera_driver_frame_t *frame)
{
	ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_ARG, TAG, "frame is null");
	ESP_RETURN_ON_ERROR(camera_driver_init(), TAG, "camera init failed");

	const int64_t deadline_us = esp_timer_get_time() + ((int64_t)CAMERA_DRIVER_FRAME_TIMEOUT_MS * 1000);
	esp_err_t ret = ESP_ERR_TIMEOUT;
	uint32_t stale_frames_dropped = 0U;
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = CAMERA_DRIVER_MEMORY_TYPE,
	};

	while (true) {
		if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
			if (!s_frame_outstanding) {
				break;
			}
			xSemaphoreGive(s_lock);
		}

		if (esp_timer_get_time() >= deadline_us) {
			return ESP_ERR_TIMEOUT;
		}
		vTaskDelay(pdMS_TO_TICKS(CAMERA_DRIVER_FRAME_BUSY_WAIT_MS));
	}

	/*
	 * esp_video keeps completed capture buffers in newest-first order. The
	 * camera runs faster than the application cadence, so older completed
	 * buffers can remain behind the last delivered frame. If no fresh capture
	 * has completed by the next deadline, DQBUF would otherwise return one of
	 * those older buffers and make the visible image jump backward in time.
	 * Requeue only non-monotonic completions and wait for a genuinely newer
	 * frame; this also returns stranded buffers to the sensor pipeline.
	 */
	while (esp_timer_get_time() < deadline_us) {
		if (ioctl(s_fd, VIDIOC_DQBUF, &buf) == 0) {
			if (!camera_driver_buffer_is_usable(&buf)) {
				camera_driver_requeue_buffer(&buf, "invalid");
				ret = ESP_FAIL;
				break;
			}
			if (s_last_delivered_sequence_valid &&
			    !camera_driver_sequence_is_newer(buf.sequence,
							     s_last_delivered_sequence)) {
				camera_driver_requeue_buffer(&buf, "stale");
				stale_frames_dropped++;
				continue;
			}
			ret = ESP_OK;
			break;
		}
		if (errno != EAGAIN) {
			ESP_LOGE(TAG, "dequeue camera frame failed errno=%d", errno);
			ret = ESP_FAIL;
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(CAMERA_DRIVER_FRAME_POLL_WAIT_MS));
	}

	if (ret != ESP_OK) {
		xSemaphoreGive(s_lock);
		return ret;
	}
	if (!camera_driver_bind_user_buffer(&buf)) {
		ESP_LOGE(TAG, "bind dequeued camera buffer failed index=%u", (unsigned)buf.index);
		xSemaphoreGive(s_lock);
		return ESP_FAIL;
	}
	memset(frame, 0, sizeof(*frame));
	s_active_frame.buffer = buf;
	s_frame_outstanding = true;
	s_last_delivered_sequence = buf.sequence;
	s_last_delivered_sequence_valid = true;

	frame->data = (const uint8_t *)s_buffers[buf.index];
	frame->data_len = buf.bytesused != 0 ? buf.bytesused : buf.length;
	frame->width = (uint16_t)s_active_format.fmt.pix.width;
	frame->height = (uint16_t)s_active_format.fmt.pix.height;
	frame->sequence = buf.sequence;
	frame->stale_frames_dropped = stale_frames_dropped;
	frame->sensor_timestamp_us = camera_driver_buffer_timestamp_us(&buf);
	switch (s_active_format.fmt.pix.pixelformat) {
	case V4L2_PIX_FMT_YUV420:
		frame->pixel_format = CAMERA_DRIVER_PIXEL_FORMAT_YUV420_OUYY_EVYY;
		break;
	case V4L2_PIX_FMT_RGB565:
		frame->pixel_format = CAMERA_DRIVER_PIXEL_FORMAT_RGB565;
		break;
	default:
		frame->pixel_format = CAMERA_DRIVER_PIXEL_FORMAT_GRAYSCALE;
		break;
	}
	frame->owner = &s_active_frame;

	if (!s_first_frame_logged) {
		s_first_frame_logged = true;
		ESP_LOGI(TAG,
			 "camera first frame: %ux%u bytes=%u index=%u sequence=%u drained=%u ts_us=%llu",
			 frame->width,
			 frame->height,
			 (unsigned)frame->data_len,
			 (unsigned)buf.index,
			 (unsigned)buf.sequence,
			 (unsigned)stale_frames_dropped,
			 (unsigned long long)frame->sensor_timestamp_us);
	}

	xSemaphoreGive(s_lock);
	return ESP_OK;
}

void camera_driver_release(camera_driver_frame_t *frame)
{
	if (frame == NULL || frame->owner == NULL || s_lock == NULL) {
		return;
	}

	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
		return;
	}

	if (frame->owner == &s_active_frame && s_fd >= 0 && s_frame_outstanding) {
		camera_driver_requeue_buffer(&s_active_frame.buffer, "frame");
		s_frame_outstanding = false;
	}

	memset(frame, 0, sizeof(*frame));
	xSemaphoreGive(s_lock);
}

esp_err_t camera_driver_deinit(void)
{
	if (s_lock == NULL) {
		return ESP_OK;
	}
	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	s_client_count = 0;
	camera_driver_cleanup();
	(void)hardware_board_set_camera_power(false);
	xSemaphoreGive(s_lock);
	return ESP_OK;
}
