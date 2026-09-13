#include "qr_scanner.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "quirc.h"

#include "app_memory_policy.h"
#include "camera_driver.h"

#define QR_SCANNER_MAX_FRAMES          12
#define QR_SCANNER_FRAME_DELAY_MS      80
#define QR_SCANNER_LIVE_FRAME_DELAY_MS 30
#define QR_SCANNER_TASK_STACK_SIZE     20480
#define QR_SCANNER_TASK_PRIORITY       3
#define QR_SCANNER_STOP_WAIT_MS        1000
#define QR_SCANNER_PROGRESS_LOG_MS     1000
#define QR_SCANNER_CAMERA_WIDTH        800U
#define QR_SCANNER_CAMERA_HEIGHT       640U
#define QR_SCANNER_CAMERA_FPS          15U

static const char *TAG = "qr_scanner";

static void *qr_scanner_calloc(size_t count, size_t size)
{
	return app_memory_calloc_psram(count, size);
}

static SemaphoreHandle_t s_scan_lock;
static TaskHandle_t s_scan_task;
static portMUX_TYPE s_scan_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_scan_running;
static bool s_scan_stop_requested;

typedef struct {
	qr_scanner_preview_cb_t preview_cb;
	qr_scanner_result_cb_t result_cb;
	void *ctx;
} qr_scanner_task_args_t;

static qr_scanner_task_args_t s_scan_task_args;

typedef enum {
	QR_SCANNER_IMAGE_NORMAL = 0,
	QR_SCANNER_IMAGE_BYTE_SWAP,
	QR_SCANNER_IMAGE_MIRROR_X,
	QR_SCANNER_IMAGE_MIRROR_X_BYTE_SWAP,
	QR_SCANNER_IMAGE_MIRROR_Y,
	QR_SCANNER_IMAGE_MIRROR_Y_BYTE_SWAP,
} qr_scanner_image_transform_t;

static void qr_scanner_trim_copy(char *dst, size_t dst_len, const char *src, size_t src_len)
{
	if (dst == NULL || dst_len == 0) {
		return;
	}

	dst[0] = '\0';
	if (src == NULL) {
		return;
	}

	while (src_len > 0 && isspace((unsigned char)*src)) {
		src++;
		src_len--;
	}
	while (src_len > 0 && isspace((unsigned char)src[src_len - 1])) {
		src_len--;
	}
	if (src_len >= dst_len) {
		src_len = dst_len - 1;
	}

	memcpy(dst, src, src_len);
	dst[src_len] = '\0';
}

static int qr_scanner_hex_value(char ch)
{
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	return -1;
}

static void qr_scanner_url_decode_in_place(char *text)
{
	char *read_ptr = text;
	char *write_ptr = text;

	if (text == NULL) {
		return;
	}

	while (*read_ptr != '\0') {
		if (*read_ptr == '%' &&
		    qr_scanner_hex_value(read_ptr[1]) >= 0 &&
		    qr_scanner_hex_value(read_ptr[2]) >= 0) {
			int hi = qr_scanner_hex_value(read_ptr[1]);
			int lo = qr_scanner_hex_value(read_ptr[2]);
			*write_ptr++ = (char)((hi << 4) | lo);
			read_ptr += 3;
			continue;
		}
		*write_ptr++ = *read_ptr;
		read_ptr++;
	}
	*write_ptr = '\0';
}

static bool qr_scanner_is_open_id_char(char ch)
{
	return (ch >= '0' && ch <= '9') ||
	       (ch >= 'a' && ch <= 'z') ||
	       (ch >= 'A' && ch <= 'Z') ||
	       ch == '_' ||
	       ch == '-';
}

static bool qr_scanner_is_plain_open_id(const char *text)
{
	size_t len = 0;

	if (text == NULL) {
		return false;
	}

	len = strlen(text);
	if (len != 28U) {
		return false;
	}

	for (size_t index = 0; index < len; ++index) {
		if (!qr_scanner_is_open_id_char(text[index])) {
			return false;
		}
	}
	return true;
}

static bool qr_scanner_is_plain_device_id(const char *text)
{
	size_t len = 0;

	if (text == NULL) {
		return false;
	}
	len = strlen(text);
	if (len != 12U) {
		return false;
	}
	for (size_t index = 0; index < len; ++index) {
		if (!qr_scanner_is_open_id_char(text[index])) {
			return false;
		}
	}
	return true;
}

static bool qr_scanner_extract_value(const char *payload,
				     const char *key,
				     char *value,
				     size_t value_len)
{
	const char *cursor = payload;
	size_t key_len = key != NULL ? strlen(key) : 0;

	if (payload == NULL || key_len == 0 || value == NULL || value_len == 0) {
		return false;
	}

	while ((cursor = strstr(cursor, key)) != NULL) {
		const char *scan = cursor + key_len;
		const char *start = NULL;
		const char *end = NULL;
		bool quoted = false;

		while (*scan == ' ' || *scan == '\t' || *scan == '"') {
			scan++;
		}
		if (*scan != ':' && *scan != '=') {
			cursor++;
			continue;
		}
		scan++;
		while (*scan == ' ' || *scan == '\t') {
			scan++;
		}
		if (*scan == '"') {
			quoted = true;
			scan++;
		}

		start = scan;
		end = start;
		while (*end != '\0') {
			if (quoted) {
				if (*end == '"') {
					break;
				}
			} else if (*end == '\r' || *end == '\n' || *end == '&' ||
				   *end == ';' || *end == ',' || *end == '}') {
				break;
			}
			end++;
		}

		qr_scanner_trim_copy(value, value_len, start, (size_t)(end - start));
		qr_scanner_url_decode_in_place(value);
		return value[0] != '\0';
	}

	return false;
}

static esp_err_t qr_scanner_parse_contact_payload_bytes(const uint8_t *payload,
							size_t payload_len,
							qr_scanner_contact_t *contact)
{
	char *text = NULL;
	char type[32] = {0};
	bool has_type = false;
	bool has_device_id = false;
	bool has_open_id = false;
	bool has_pair_key = false;
	esp_err_t ret = ESP_ERR_INVALID_RESPONSE;

	ESP_RETURN_ON_FALSE(payload != NULL && contact != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid qr payload");

	text = qr_scanner_calloc(1, QR_SCANNER_PAYLOAD_MAX);
	ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_NO_MEM, TAG, "qr payload text alloc failed");

	qr_scanner_trim_copy(text, QR_SCANNER_PAYLOAD_MAX, (const char *)payload, payload_len);
	if (text[0] == '\0') {
		goto done;
	}

	memset(contact, 0, sizeof(*contact));
	strlcpy(contact->raw_payload, text, sizeof(contact->raw_payload));

	if (qr_scanner_is_plain_open_id(text)) {
		strlcpy(contact->open_id, text, sizeof(contact->open_id));
		ret = ESP_OK;
		goto done;
	}
	if (qr_scanner_is_plain_device_id(text)) {
		strlcpy(contact->device_id, text, sizeof(contact->device_id));
		ret = ESP_OK;
		goto done;
	}

	has_type = qr_scanner_extract_value(text, "type", type, sizeof(type));
	if (has_type &&
	    strcmp(type, "contact_add") != 0 &&
	    strcmp(type, "tirtc_config") != 0 &&
	    strcmp(type, "device_credentials") != 0 &&
	    strcmp(type, "wechat_contact") != 0 &&
	    strcmp(type, "wechat_contact_add") != 0 &&
	    strcmp(type, "wx_contact_add") != 0) {
		ESP_LOGW(TAG, "unsupported qr type: type=%s", type);
		goto done;
	}

	has_open_id = qr_scanner_extract_value(text,
					       "open_id",
					       contact->open_id,
					       sizeof(contact->open_id));
	if (!has_open_id) {
		has_open_id = qr_scanner_extract_value(text,
						       "openid",
						       contact->open_id,
						       sizeof(contact->open_id));
	}
	if (!has_open_id) {
		has_open_id = qr_scanner_extract_value(text,
						       "wx_open_id",
						       contact->open_id,
						       sizeof(contact->open_id));
	}
	if (has_open_id) {
		ret = ESP_OK;
		goto done;
	}

	has_device_id = qr_scanner_extract_value(text,
						  "device_id",
						  contact->device_id,
						  sizeof(contact->device_id));
	if (!has_device_id) {
		has_device_id = qr_scanner_extract_value(text,
							  "remote_device_id",
							  contact->device_id,
							  sizeof(contact->device_id));
	}

	has_pair_key = qr_scanner_extract_value(text,
						"device_secret_key",
						contact->pair_key,
						sizeof(contact->pair_key));
	if (!has_pair_key) {
		has_pair_key = qr_scanner_extract_value(text,
							"pair_key",
							contact->pair_key,
							sizeof(contact->pair_key));
	}
	if (!has_pair_key) {
		has_pair_key = qr_scanner_extract_value(text,
							"device_secret",
							contact->pair_key,
							sizeof(contact->pair_key));
	}
	if (!has_pair_key) {
		has_pair_key = qr_scanner_extract_value(text,
							"secret_key",
							contact->pair_key,
							sizeof(contact->pair_key));
	}

	bool credentials_payload = !has_type ||
		strcmp(type, "tirtc_config") == 0 ||
		strcmp(type, "device_credentials") == 0;
	if (!has_device_id || (credentials_payload && !has_pair_key)) {
		ESP_LOGW(TAG,
			 "qr payload missing fields: type=%s has_device_id=%u has_device_secret_key=%u",
			 has_type ? type : "legacy_credentials",
			 has_device_id ? 1U : 0U,
			 has_pair_key ? 1U : 0U);
		goto done;
	}

	ret = ESP_OK;

done:
	free(text);
	return ret;
}

esp_err_t qr_scanner_parse_contact_payload(const char *payload_text,
					   qr_scanner_contact_t *contact)
{
	if (payload_text == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	return qr_scanner_parse_contact_payload_bytes((const uint8_t *)payload_text,
						      strlen(payload_text),
						      contact);
}

static uint8_t qr_scanner_luma_from_rgb565(uint16_t pixel)
{
	uint8_t red = (uint8_t)((pixel >> 11) & 0x1FU);
	uint8_t green = (uint8_t)((pixel >> 5) & 0x3FU);
	uint8_t blue = (uint8_t)(pixel & 0x1FU);

	red = (uint8_t)((red << 3) | (red >> 2));
	green = (uint8_t)((green << 2) | (green >> 4));
	blue = (uint8_t)((blue << 3) | (blue >> 2));

	return (uint8_t)(((uint16_t)red * 77U + (uint16_t)green * 150U + (uint16_t)blue * 29U) >> 8);
}

static const char *qr_scanner_transform_name(qr_scanner_image_transform_t transform)
{
	switch (transform) {
	case QR_SCANNER_IMAGE_BYTE_SWAP:
		return "byte-swap";
	case QR_SCANNER_IMAGE_MIRROR_X:
		return "mirror-x";
	case QR_SCANNER_IMAGE_MIRROR_X_BYTE_SWAP:
		return "mirror-x+byte-swap";
	case QR_SCANNER_IMAGE_MIRROR_Y:
		return "mirror-y";
	case QR_SCANNER_IMAGE_MIRROR_Y_BYTE_SWAP:
		return "mirror-y+byte-swap";
	case QR_SCANNER_IMAGE_NORMAL:
	default:
		return "normal";
	}
}

static bool qr_scanner_transform_mirrors_x(qr_scanner_image_transform_t transform)
{
	return transform == QR_SCANNER_IMAGE_MIRROR_X ||
	       transform == QR_SCANNER_IMAGE_MIRROR_X_BYTE_SWAP;
}

static bool qr_scanner_transform_mirrors_y(qr_scanner_image_transform_t transform)
{
	return transform == QR_SCANNER_IMAGE_MIRROR_Y ||
	       transform == QR_SCANNER_IMAGE_MIRROR_Y_BYTE_SWAP;
}

static bool qr_scanner_transform_swaps_bytes(qr_scanner_image_transform_t transform)
{
	return transform == QR_SCANNER_IMAGE_BYTE_SWAP ||
	       transform == QR_SCANNER_IMAGE_MIRROR_X_BYTE_SWAP ||
	       transform == QR_SCANNER_IMAGE_MIRROR_Y_BYTE_SWAP;
}

static uint16_t qr_scanner_maybe_swap_rgb565(uint16_t pixel, bool swap_bytes)
{
	if (!swap_bytes) {
		return pixel;
	}
	return (uint16_t)((pixel >> 8) | (pixel << 8));
}

static quirc_decode_error_t qr_scanner_decode_code_with_flip(struct quirc_code *code,
							     struct quirc_data *data,
							     bool *used_flip)
{
	quirc_decode_error_t decode_error = QUIRC_SUCCESS;

	if (used_flip != NULL) {
		*used_flip = false;
	}
	if (code == NULL || data == NULL) {
		return QUIRC_ERROR_INVALID_GRID_SIZE;
	}

	decode_error = quirc_decode(code, data);
	if (decode_error == QUIRC_ERROR_DATA_ECC) {
		quirc_flip(code);
		decode_error = quirc_decode(code, data);
		if (decode_error == QUIRC_SUCCESS && used_flip != NULL) {
			*used_flip = true;
		}
	}
	return decode_error;
}

static esp_err_t qr_scanner_copy_frame_to_decoder(uint8_t *image,
						  size_t image_len,
						  uint16_t image_width,
						  uint16_t image_height,
						  const camera_driver_frame_t *frame,
						  qr_scanner_image_transform_t transform)
{
	size_t source_pixel_count = (size_t)frame->width * frame->height;
	size_t image_pixel_count = (size_t)image_width * image_height;
	uint16_t source_step_x = frame->width / image_width;
	uint16_t source_step_y = frame->height / image_height;
	bool mirror_x = qr_scanner_transform_mirrors_x(transform);
	bool mirror_y = qr_scanner_transform_mirrors_y(transform);
	bool swap_bytes = qr_scanner_transform_swaps_bytes(transform);

	ESP_RETURN_ON_FALSE(image != NULL && frame != NULL &&
				    image_width > 0U && image_height > 0U &&
				    frame->width % image_width == 0U &&
				    frame->height % image_height == 0U,
			    ESP_ERR_INVALID_ARG,
			    TAG,
			    "invalid frame copy args");
	ESP_RETURN_ON_FALSE(image_len >= image_pixel_count,
			    ESP_ERR_INVALID_SIZE,
			    TAG,
			    "decoder image buffer too small");

	if (frame->pixel_format == CAMERA_DRIVER_PIXEL_FORMAT_GRAYSCALE) {
		ESP_RETURN_ON_FALSE(frame->data != NULL && frame->data_len >= source_pixel_count,
				    ESP_ERR_INVALID_SIZE,
				    TAG,
				    "camera grayscale frame is incomplete");
		if (!mirror_x && !mirror_y && source_step_x == 1U && source_step_y == 1U) {
			memcpy(image, frame->data, image_pixel_count);
		} else {
			for (uint16_t y = 0; y < image_height; ++y) {
				uint16_t sampled_y = (uint16_t)(y * source_step_y);
				uint16_t src_y = mirror_y ?
					(uint16_t)(frame->height - 1U - sampled_y) : sampled_y;
				const uint8_t *src_row = frame->data + ((size_t)src_y * frame->width);
				uint8_t *dst_row = image + ((size_t)y * image_width);

				for (uint16_t x = 0; x < image_width; ++x) {
					uint16_t sampled_x = (uint16_t)(x * source_step_x);
					uint16_t src_x = mirror_x ?
						(uint16_t)(frame->width - 1U - sampled_x) : sampled_x;
					dst_row[x] = src_row[src_x];
				}
			}
		}
		return ESP_OK;
	}

	if (frame->pixel_format == CAMERA_DRIVER_PIXEL_FORMAT_YUV420_OUYY_EVYY) {
		ESP_RETURN_ON_FALSE(frame->data != NULL &&
				    frame->data_len >= source_pixel_count * 3U / 2U,
				    ESP_ERR_INVALID_SIZE,
				    TAG,
				    "camera yuv420 frame is incomplete");
		/* The P4 camera's V4L2 YU12 buffer is the packed O_UYY_E_VYY
		 * layout consumed by PPA/H264, not planar I420. Each two pixels use
		 * one chroma byte followed by two full-resolution luma bytes. */
		const size_t source_row_stride = (size_t)frame->width * 3U / 2U;
		for (uint16_t y = 0; y < image_height; ++y) {
			uint16_t sampled_y = (uint16_t)(y * source_step_y);
			uint16_t src_y = mirror_y ?
				(uint16_t)(frame->height - 1U - sampled_y) : sampled_y;
			const uint8_t *src_row =
				frame->data + ((size_t)src_y * source_row_stride);
			uint8_t *dst_row = image + ((size_t)y * image_width);

			for (uint16_t x = 0; x < image_width; ++x) {
				uint16_t sampled_x = (uint16_t)(x * source_step_x);
				uint16_t src_x = mirror_x ?
					(uint16_t)(frame->width - 1U - sampled_x) : sampled_x;
				dst_row[x] = src_row[((size_t)src_x / 2U) * 3U +
							 1U + (src_x & 1U)];
			}
		}
		return ESP_OK;
	}

	if (frame->pixel_format == CAMERA_DRIVER_PIXEL_FORMAT_RGB565) {
		const uint16_t *pixels = (const uint16_t *)frame->data;

		ESP_RETURN_ON_FALSE(frame->data != NULL && frame->data_len >= source_pixel_count * sizeof(uint16_t),
				    ESP_ERR_INVALID_SIZE,
				    TAG,
				    "camera rgb565 frame is incomplete");
		for (uint16_t y = 0; y < image_height; ++y) {
			uint16_t sampled_y = (uint16_t)(y * source_step_y);
			size_t row_offset = (size_t)y * image_width;
			size_t src_row_offset = (size_t)(mirror_y ?
				(uint16_t)(frame->height - 1U - sampled_y) : sampled_y) * frame->width;

			for (uint16_t x = 0; x < image_width; ++x) {
				uint16_t sampled_x = (uint16_t)(x * source_step_x);
				size_t src_index = src_row_offset + (mirror_x ?
					(frame->width - 1U - sampled_x) : sampled_x);
				uint16_t pixel = qr_scanner_maybe_swap_rgb565(pixels[src_index], swap_bytes);
				image[row_offset + x] = qr_scanner_luma_from_rgb565(pixel);
			}
		}
		return ESP_OK;
	}

	return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t qr_scanner_decode_frame(struct quirc *decoder,
					 const camera_driver_frame_t *frame,
					 qr_scanner_contact_t *contact,
					 uint16_t *decoder_width,
					 uint16_t *decoder_height)
{
	int image_width = 0;
	int image_height = 0;
	uint16_t decode_width = 0;
	uint16_t decode_height = 0;
	uint8_t *image = NULL;
	size_t pixel_count = 0;
	esp_err_t first_error = ESP_ERR_NOT_FOUND;
	const qr_scanner_image_transform_t transforms[] = {
		QR_SCANNER_IMAGE_NORMAL,
		QR_SCANNER_IMAGE_BYTE_SWAP,
		QR_SCANNER_IMAGE_MIRROR_X,
		QR_SCANNER_IMAGE_MIRROR_X_BYTE_SWAP,
		QR_SCANNER_IMAGE_MIRROR_Y,
		QR_SCANNER_IMAGE_MIRROR_Y_BYTE_SWAP,
	};

	ESP_RETURN_ON_FALSE(decoder != NULL && frame != NULL && contact != NULL &&
			    decoder_width != NULL && decoder_height != NULL,
			    ESP_ERR_INVALID_ARG,
			    TAG,
			    "invalid decode args");

	decode_width = frame->width;
	decode_height = frame->height;
	if ((frame->pixel_format == CAMERA_DRIVER_PIXEL_FORMAT_YUV420_OUYY_EVYY ||
	     frame->pixel_format == CAMERA_DRIVER_PIXEL_FORMAT_GRAYSCALE) &&
	    frame->width >= 640U && frame->height >= 480U) {
		decode_width = (uint16_t)(frame->width / 2U);
		decode_height = (uint16_t)(frame->height / 2U);
	}
	pixel_count = (size_t)decode_width * decode_height;

	for (size_t transform_index = 0; transform_index < sizeof(transforms) / sizeof(transforms[0]); ++transform_index) {
		qr_scanner_image_transform_t transform = transforms[transform_index];
		int code_count = 0;
		/* The P4 camera provides an orientation-correct Y plane. Quirc handles
		 * QR rotation itself, so mirror/byte-swap retries only waste two full
		 * image scans and make the live preview visibly stall. Keep correction
		 * retries for the legacy RGB565 input path only. */
		if (frame->pixel_format != CAMERA_DRIVER_PIXEL_FORMAT_RGB565 &&
		    transform != QR_SCANNER_IMAGE_NORMAL) {
			continue;
		}

		/* quirc_resize always allocates replacement image and flood-fill
		 * buffers, even when the dimensions are unchanged. The camera profile
		 * is stable during a scan, so resize only when the decoded image shape
		 * actually changes instead of churning two large heap blocks per frame. */
		if (*decoder_width != decode_width || *decoder_height != decode_height) {
			ESP_RETURN_ON_FALSE(quirc_resize(decoder, decode_width, decode_height) >= 0,
					    ESP_ERR_NO_MEM,
					    TAG,
					    "qr decoder resize failed");
			*decoder_width = decode_width;
			*decoder_height = decode_height;
		}

		image = quirc_begin(decoder, &image_width, &image_height);
		ESP_RETURN_ON_FALSE(image != NULL &&
				    image_width == decode_width &&
				    image_height == decode_height,
				    ESP_ERR_INVALID_SIZE,
				    TAG,
				    "qr decoder image buffer mismatch");
		ESP_RETURN_ON_ERROR(qr_scanner_copy_frame_to_decoder(image,
							      pixel_count,
							      decode_width,
							      decode_height,
							      frame,
							      transform),
				    TAG,
				    "camera frame convert failed");
		quirc_end(decoder);

		code_count = quirc_count(decoder);
		if (code_count == 0) {
			continue;
		}

		ESP_LOGI(TAG,
			 "qr candidates detected: count=%d transform=%s",
			 code_count,
			 qr_scanner_transform_name(transform));
		for (int index = 0; index < code_count; ++index) {
			struct quirc_code *code = NULL;
			struct quirc_data *data = NULL;
			quirc_decode_error_t decode_error;
			char *payload_text = NULL;
			qr_scanner_contact_t *parsed_contact = NULL;
			bool used_flip = false;

			code = qr_scanner_calloc(1, sizeof(*code));
			data = qr_scanner_calloc(1, sizeof(*data));
			payload_text = qr_scanner_calloc(1, QR_SCANNER_PAYLOAD_MAX);
			parsed_contact = qr_scanner_calloc(1, sizeof(*parsed_contact));
			if (code == NULL || data == NULL || payload_text == NULL || parsed_contact == NULL) {
				free(code);
				free(data);
				free(payload_text);
				free(parsed_contact);
				return ESP_ERR_NO_MEM;
			}

			quirc_extract(decoder, index, code);
			ESP_LOGD(TAG,
				 "qr decode attempt: transform=%s index=%d size=%d",
				 qr_scanner_transform_name(transform),
				 index,
				 code->size);
			decode_error = qr_scanner_decode_code_with_flip(code, data, &used_flip);
			if (decode_error != QUIRC_SUCCESS) {
				ESP_LOGW(TAG,
					 "qr decode failed: transform=%s err=%s",
					 qr_scanner_transform_name(transform),
					 quirc_strerror(decode_error));
				free(code);
				free(data);
				free(payload_text);
				free(parsed_contact);
				continue;
			}

			qr_scanner_trim_copy(payload_text,
					     QR_SCANNER_PAYLOAD_MAX,
					     (const char *)data->payload,
					     data->payload_len);
			ESP_LOGD(TAG,
				 "qr payload decoded: transform=%s code_flip=%u len=%u",
				 qr_scanner_transform_name(transform),
				 used_flip ? 1U : 0U,
				 (unsigned)data->payload_len);
			esp_err_t parse_ret = qr_scanner_parse_contact_payload_bytes(data->payload,
										     data->payload_len,
										     parsed_contact);
			if (parse_ret == ESP_OK) {
				*contact = *parsed_contact;
				if (transform != QR_SCANNER_IMAGE_NORMAL) {
					ESP_LOGI(TAG,
						 "qr decoded after image correction: transform=%s",
						 qr_scanner_transform_name(transform));
				}
				free(code);
				free(data);
				free(payload_text);
				free(parsed_contact);
				return ESP_OK;
			}
			first_error = parse_ret;
			if (contact->raw_payload[0] == '\0' && parsed_contact->raw_payload[0] != '\0') {
				*contact = *parsed_contact;
			}
			ESP_LOGW(TAG,
				 "qr payload parse failed: transform=%s ret=%s",
				 qr_scanner_transform_name(transform),
				 esp_err_to_name(parse_ret));
			free(code);
			free(data);
			free(payload_text);
			free(parsed_contact);
			continue;
		}
	}

	return first_error;
}

static esp_err_t qr_scanner_lock(void)
{
	if (s_scan_lock == NULL) {
		s_scan_lock = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
		if (s_scan_lock == NULL) {
			return ESP_ERR_NO_MEM;
		}
		xSemaphoreGive(s_scan_lock);
	}
	return xSemaphoreTake(s_scan_lock, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void qr_scanner_set_running(bool running)
{
	taskENTER_CRITICAL(&s_scan_state_lock);
	s_scan_running = running;
	if (!running) {
		s_scan_stop_requested = false;
	}
	taskEXIT_CRITICAL(&s_scan_state_lock);
}

static bool qr_scanner_is_running(void)
{
	bool running = false;

	taskENTER_CRITICAL(&s_scan_state_lock);
	running = s_scan_running;
	taskEXIT_CRITICAL(&s_scan_state_lock);
	return running;
}

static bool qr_scanner_should_stop(void)
{
	bool stop = false;

	taskENTER_CRITICAL(&s_scan_state_lock);
	stop = s_scan_stop_requested;
	taskEXIT_CRITICAL(&s_scan_state_lock);
	return stop;
}

static void qr_scanner_request_stop(void)
{
	taskENTER_CRITICAL(&s_scan_state_lock);
	s_scan_stop_requested = true;
	taskEXIT_CRITICAL(&s_scan_state_lock);
}

static void qr_scanner_emit_preview(const qr_scanner_task_args_t *args,
				    const camera_driver_frame_t *frame)
{
	if (args == NULL || args->preview_cb == NULL || frame == NULL ||
	    frame->data == NULL) {
		return;
	}

	size_t pixel_count = (size_t)frame->width * frame->height;
	size_t needed = 0U;
	scan_preview_pixel_format_t preview_format = SCAN_PREVIEW_PIXEL_FORMAT_GRAYSCALE;
	switch (frame->pixel_format) {
	case CAMERA_DRIVER_PIXEL_FORMAT_RGB565:
		preview_format = SCAN_PREVIEW_PIXEL_FORMAT_RGB565;
		needed = pixel_count * sizeof(uint16_t);
		break;
	case CAMERA_DRIVER_PIXEL_FORMAT_YUV420_OUYY_EVYY:
		preview_format = SCAN_PREVIEW_PIXEL_FORMAT_YUV420_OUYY_EVYY;
		needed = pixel_count * 3U / 2U;
		break;
	case CAMERA_DRIVER_PIXEL_FORMAT_GRAYSCALE:
	default:
		preview_format = SCAN_PREVIEW_PIXEL_FORMAT_GRAYSCALE;
		needed = pixel_count;
		break;
	}
	if (frame->data_len < needed) {
		return;
	}

	const scan_preview_frame_t preview = {
		.data = frame->data,
		.data_len = frame->data_len,
		.width = frame->width,
		.height = frame->height,
		.pixel_format = preview_format,
	};
	args->preview_cb(&preview, args->ctx);
}

static void qr_scanner_run_live_session(const qr_scanner_task_args_t *session_args)
{
	qr_scanner_task_args_t args = {0};
	qr_scanner_contact_t *contact = NULL;
	struct quirc *decoder = NULL;
	esp_err_t ret = ESP_ERR_NOT_FOUND;
	bool cancelled = false;
	bool camera_acquired = false;
	uint32_t frame_count = 0;
	uint16_t decoder_width = 0;
	uint16_t decoder_height = 0;
	TickType_t last_progress_log_tick = xTaskGetTickCount();

	if (session_args != NULL) {
		args = *session_args;
	}
	contact = qr_scanner_calloc(1, sizeof(*contact));
	if (contact == NULL) {
		ret = ESP_ERR_NO_MEM;
		goto done;
	}

	decoder = quirc_new();
	if (decoder == NULL) {
		ret = ESP_ERR_NO_MEM;
		goto done;
	}

	ret = camera_driver_acquire();
	if (ret != ESP_OK) {
		goto done;
	}
	camera_acquired = true;
	ESP_LOGI(TAG, "contact qr scan started");

	while (!qr_scanner_should_stop()) {
		camera_driver_frame_t frame = {0};

		ret = camera_driver_capture(&frame);
		if (ret == ESP_OK) {
			++frame_count;
			if (qr_scanner_should_stop()) {
				camera_driver_release(&frame);
				break;
			}
			qr_scanner_emit_preview(&args, &frame);
			if (qr_scanner_should_stop()) {
				camera_driver_release(&frame);
				break;
			}
			ret = qr_scanner_decode_frame(decoder,
						      &frame,
						      contact,
						      &decoder_width,
						      &decoder_height);
			camera_driver_release(&frame);
			if (ret == ESP_OK || ret == ESP_ERR_INVALID_RESPONSE) {
				break;
			}
			ret = ESP_ERR_NOT_FOUND;
		}
		TickType_t now = xTaskGetTickCount();
		if ((now - last_progress_log_tick) >= pdMS_TO_TICKS(QR_SCANNER_PROGRESS_LOG_MS)) {
			last_progress_log_tick = now;
			ESP_LOGD(TAG,
				 "contact qr scanning: frames=%" PRIu32 " last=%s",
				 frame_count,
				 esp_err_to_name(ret));
		}

		vTaskDelay(pdMS_TO_TICKS(QR_SCANNER_LIVE_FRAME_DELAY_MS));
	}

	cancelled = qr_scanner_should_stop();

done:
	cancelled = cancelled || qr_scanner_should_stop();
	if (decoder != NULL) {
		quirc_destroy(decoder);
	}
	if (camera_acquired) {
		camera_driver_release_device();
	}
	ESP_LOGI(TAG,
		 "contact qr scan stopped: cancelled=%u frames=%" PRIu32 " result=%s",
		 cancelled ? 1U : 0U,
		 frame_count,
		 esp_err_to_name(ret));
	qr_scanner_set_running(false);
	if (s_scan_lock != NULL) {
		xSemaphoreGive(s_scan_lock);
	}

	if (!cancelled && args.result_cb != NULL) {
		args.result_cb(ret,
			       (contact != NULL && (ret == ESP_OK || contact->raw_payload[0] != '\0')) ? contact : NULL,
			       args.ctx);
	}

	free(contact);
}

static void qr_scanner_live_task(void *arg)
{
	(void)arg;
	ESP_LOGI(TAG, "contact qr scanner worker ready");

	for (;;) {
		qr_scanner_task_args_t args = {0};

		(void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		taskENTER_CRITICAL(&s_scan_state_lock);
		args = s_scan_task_args;
		taskEXIT_CRITICAL(&s_scan_state_lock);
		qr_scanner_run_live_session(&args);
	}
}

esp_err_t qr_scanner_start_contact(qr_scanner_preview_cb_t preview_cb,
				   qr_scanner_result_cb_t result_cb,
				   void *ctx)
{
	if (!camera_driver_is_configured()) {
		ESP_LOGW(TAG, "camera is not configured; check HARDWARE_BOARD_CAMERA_* in hardware_board_config.h");
		return ESP_ERR_NOT_SUPPORTED;
	}
	ESP_RETURN_ON_ERROR(camera_driver_set_stream_target(QR_SCANNER_CAMERA_WIDTH,
							 QR_SCANNER_CAMERA_HEIGHT,
							 QR_SCANNER_CAMERA_FPS),
			    TAG,
			    "configure qr camera profile failed");
	ESP_RETURN_ON_ERROR(qr_scanner_lock(), TAG, "qr scanner is busy");

	if (s_scan_task == NULL) {
		BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(qr_scanner_live_task,
								   "qr_scan_live",
								   QR_SCANNER_TASK_STACK_SIZE,
								   NULL,
								   QR_SCANNER_TASK_PRIORITY,
								   &s_scan_task,
								   tskNO_AFFINITY,
								   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (task_ret != pdPASS) {
			s_scan_task = NULL;
			xSemaphoreGive(s_scan_lock);
			return ESP_ERR_NO_MEM;
		}
	}

	taskENTER_CRITICAL(&s_scan_state_lock);
	s_scan_task_args.preview_cb = preview_cb;
	s_scan_task_args.result_cb = result_cb;
	s_scan_task_args.ctx = ctx;
	taskEXIT_CRITICAL(&s_scan_state_lock);
	qr_scanner_set_running(true);
	xTaskNotifyGive(s_scan_task);

	return ESP_OK;
}

esp_err_t qr_scanner_stop(void)
{
	TickType_t start_tick = xTaskGetTickCount();
	TickType_t wait_ticks = pdMS_TO_TICKS(QR_SCANNER_STOP_WAIT_MS);

	if (!qr_scanner_is_running()) {
		return ESP_ERR_INVALID_STATE;
	}

	qr_scanner_request_stop();
	while (qr_scanner_is_running()) {
		if ((xTaskGetTickCount() - start_tick) >= wait_ticks) {
			return ESP_ERR_TIMEOUT;
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}

	return ESP_OK;
}

esp_err_t qr_scanner_scan_contact(qr_scanner_contact_t *contact)
{
	struct quirc *decoder = NULL;
	esp_err_t ret = ESP_ERR_NOT_FOUND;
	bool camera_acquired = false;
	uint16_t decoder_width = 0;
	uint16_t decoder_height = 0;

	ESP_RETURN_ON_FALSE(contact != NULL, ESP_ERR_INVALID_ARG, TAG, "contact is null");
	if (!camera_driver_is_configured()) {
		ESP_LOGW(TAG, "camera is not configured; check HARDWARE_BOARD_CAMERA_* in hardware_board_config.h");
		return ESP_ERR_NOT_SUPPORTED;
	}
	ESP_RETURN_ON_ERROR(camera_driver_set_stream_target(QR_SCANNER_CAMERA_WIDTH,
							 QR_SCANNER_CAMERA_HEIGHT,
							 QR_SCANNER_CAMERA_FPS),
			    TAG,
			    "configure qr camera profile failed");
	ESP_RETURN_ON_ERROR(qr_scanner_lock(), TAG, "qr scanner is busy");

	decoder = quirc_new();
	if (decoder == NULL) {
		ret = ESP_ERR_NO_MEM;
		goto done;
	}

	ret = camera_driver_acquire();
	if (ret != ESP_OK) {
		goto done;
	}
	camera_acquired = true;

	for (uint8_t frame_index = 0; frame_index < QR_SCANNER_MAX_FRAMES; ++frame_index) {
		camera_driver_frame_t frame = {0};

		ret = camera_driver_capture(&frame);
		if (ret == ESP_OK) {
			ret = qr_scanner_decode_frame(decoder,
						      &frame,
						      contact,
						      &decoder_width,
						      &decoder_height);
			camera_driver_release(&frame);
		}
		if (ret == ESP_OK || ret == ESP_ERR_INVALID_RESPONSE) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(QR_SCANNER_FRAME_DELAY_MS));
	}

done:
	if (decoder != NULL) {
		quirc_destroy(decoder);
	}
	if (camera_acquired) {
		camera_driver_release_device();
	}
	xSemaphoreGive(s_scan_lock);

	if (ret == ESP_OK) {
		if (contact->open_id[0] != '\0') {
			ESP_LOGI(TAG, "wechat contact QR parsed");
		} else {
			ESP_LOGI(TAG, "contact QR parsed: device_id_len=%u", (unsigned)strlen(contact->device_id));
		}
	}
	return ret;
}
