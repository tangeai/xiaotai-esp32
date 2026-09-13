#include "device_video_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "app_memory_policy.h"

#define DEVICE_VIDEO_H264_READ_CHUNK_BYTES 4096U
#define DEVICE_VIDEO_H264_BUFFER_INIT_BYTES (16U * 1024U)
#define DEVICE_VIDEO_H264_BUFFER_LIMIT_BYTES (512U * 1024U)

#define DEVICE_VIDEO_PARSE_NEED_MORE 2

static int device_video_find_nal_unit(uint8_t *buf, int size,
                                      uint8_t *nal_type, int *nal_start,
                                      int *nal_end) {
  int i;

  if (buf == NULL || nal_type == NULL || nal_start == NULL || nal_end == NULL ||
      size < 4) {
    return 0;
  }

  i = 0;
  *nal_start = 0;
  *nal_end = 0;

  while (buf[i] != 0 || buf[i + 1] != 0 ||
         !(buf[i + 2] == 1 || (buf[i + 2] == 0 && buf[i + 3] == 1))) {
    i++;
    if (size < i + 4) {
      return 0;
    }
  }

  *nal_start = i;
  if (buf[i + 2] == 1) {
    *nal_type = buf[i + 3] & 0x1f;
    i += 4;
  } else if (size > i + 4) {
    *nal_type = buf[i + 4] & 0x1f;
    i += 5;
  } else {
    return 0;
  }

  if (size < i + 4) {
    *nal_end = size - 1;
    return -1;
  }

  while (buf[i] != 0 || buf[i + 1] != 0 ||
         !(buf[i + 2] == 1 || (buf[i + 2] == 0 && buf[i + 3] == 1))) {
    i++;
    if (size < i + 4) {
      *nal_end = size - 1;
      return -1;
    }
  }

  *nal_end = i - 1;
  return *nal_end - *nal_start;
}

#define DEVICE_VIDEO_BIT(num, bit) (((num) & (1 << (7 - (bit)))) > 0)

static int device_video_exp_golomb_decode(const uint8_t *buffer, int size,
                                          int *bit_offset) {
  int total_bits;
  int leading_zero_bits;
  int i;
  int offset;
  int bit_pos;

  total_bits = size << 3;
  leading_zero_bits = 0;
  for (i = *bit_offset; i < total_bits &&
                        !DEVICE_VIDEO_BIT(buffer[i / 8], i % 8);
       i++) {
    leading_zero_bits++;
  }

  offset = 0;
  bit_pos = *bit_offset + leading_zero_bits + 1;
  for (i = 0; i < leading_zero_bits; i++) {
    offset =
        (offset << 1) + DEVICE_VIDEO_BIT(buffer[bit_pos / 8], bit_pos % 8);
    bit_pos++;
  }

  *bit_offset += leading_zero_bits + 1 + leading_zero_bits;
  return (1 << leading_zero_bits) - 1 + offset;
}

static int device_video_h264_slice_payload_offset(const uint8_t *data,
                                                  size_t size, int nal_start) {
  if (data == NULL || size <= (size_t)(nal_start + 4)) {
    return -1;
  }
  if (data[nal_start + 2] == 1) {
    return nal_start + 4;
  }
  if (size <= (size_t)(nal_start + 5)) {
    return -1;
  }
  return nal_start + 5;
}

static void device_video_source_file_compact(device_video_h264_file_t *file) {
  if (file == NULL || file->consumed_bytes == 0) {
    return;
  }

  if (file->consumed_bytes >= file->size) {
    file->size = 0;
    file->consumed_bytes = 0;
    return;
  }

  memmove(file->data, file->data + file->consumed_bytes,
          file->size - file->consumed_bytes);
  file->size -= file->consumed_bytes;
  file->consumed_bytes = 0;
}

static int device_video_source_file_reserve(device_video_h264_file_t *file,
                                            size_t required_capacity) {
  size_t new_capacity;
  uint8_t *new_data;

  if (file == NULL) {
    return DEVICE_VIDEO_ERR_INVALID_ARG;
  }
  if (required_capacity > DEVICE_VIDEO_H264_BUFFER_LIMIT_BYTES) {
    return DEVICE_VIDEO_ERR_IO;
  }
  if (file->capacity >= required_capacity) {
    return DEVICE_VIDEO_OK;
  }

  new_capacity = file->capacity == 0 ? DEVICE_VIDEO_H264_BUFFER_INIT_BYTES
                                     : file->capacity;
  while (new_capacity < required_capacity) {
    if (new_capacity >= DEVICE_VIDEO_H264_BUFFER_LIMIT_BYTES) {
      return DEVICE_VIDEO_ERR_IO;
    }
    new_capacity *= 2;
    if (new_capacity > DEVICE_VIDEO_H264_BUFFER_LIMIT_BYTES) {
      new_capacity = DEVICE_VIDEO_H264_BUFFER_LIMIT_BYTES;
    }
  }

  new_data = (uint8_t *)app_memory_alloc_psram(new_capacity);
  if (new_data == NULL) {
    return DEVICE_VIDEO_ERR_IO;
  }

  if (file->data != NULL && file->size > 0) {
    memcpy(new_data, file->data, file->size);
    free(file->data);
  }

  file->data = new_data;
  file->capacity = new_capacity;
  return DEVICE_VIDEO_OK;
}

static int device_video_source_file_fill(device_video_h264_file_t *file) {
  size_t free_space;
  size_t read_size;
  size_t read_bytes;
  int rc;

  if (file == NULL || file->fp == NULL) {
    return DEVICE_VIDEO_ERR_INVALID_ARG;
  }
  if (file->file_end_reached) {
    return DEVICE_VIDEO_OK;
  }

  device_video_source_file_compact(file);
  if (file->capacity - file->size < DEVICE_VIDEO_H264_READ_CHUNK_BYTES) {
    rc = device_video_source_file_reserve(
        file, file->size + DEVICE_VIDEO_H264_READ_CHUNK_BYTES);
    if (rc != DEVICE_VIDEO_OK) {
      return rc;
    }
  }

  free_space = file->capacity - file->size;
  if (free_space == 0) {
    return DEVICE_VIDEO_ERR_IO;
  }

  read_size = free_space;
  if (read_size > DEVICE_VIDEO_H264_READ_CHUNK_BYTES) {
    read_size = DEVICE_VIDEO_H264_READ_CHUNK_BYTES;
  }

  read_bytes = fread(file->data + file->size, 1, read_size, file->fp);
  file->size += read_bytes;

  if (read_bytes == 0) {
    if (ferror(file->fp)) {
      return DEVICE_VIDEO_ERR_IO;
    }
    if (feof(file->fp)) {
      file->file_end_reached = true;
      return DEVICE_VIDEO_OK;
    }
    return DEVICE_VIDEO_ERR_IO;
  }

  if (read_bytes < read_size && feof(file->fp)) {
    file->file_end_reached = true;
  }

  return DEVICE_VIDEO_OK;
}

static int device_video_source_file_locate_nal(
    const device_video_h264_file_t *file, int search_offset,
    uint8_t *nal_type, int *nal_start, int *nal_end) {
  int local_start;
  int local_end;
  int ret;

  if (file == NULL || file->data == NULL || nal_type == NULL ||
      nal_start == NULL || nal_end == NULL || search_offset < 0) {
    return DEVICE_VIDEO_ERR_INVALID_ARG;
  }
  if ((size_t)search_offset >= file->size) {
    return file->file_end_reached ? DEVICE_VIDEO_ERR_EOF
                                  : DEVICE_VIDEO_PARSE_NEED_MORE;
  }

  ret = device_video_find_nal_unit(
      file->data + search_offset, (int)(file->size - (size_t)search_offset),
      nal_type, &local_start, &local_end);
  if (ret == 0) {
    return file->file_end_reached ? DEVICE_VIDEO_ERR_EOF
                                  : DEVICE_VIDEO_PARSE_NEED_MORE;
  }
  if (ret == -1 && !file->file_end_reached) {
    return DEVICE_VIDEO_PARSE_NEED_MORE;
  }

  *nal_start = search_offset + local_start;
  *nal_end = search_offset + local_end;
  return DEVICE_VIDEO_OK;
}

static int device_video_source_file_find_frame(device_video_h264_file_t *file,
                                               int *frame_start,
                                               int *frame_end,
                                               int *is_key_frame) {
  uint8_t nal_type;
  int nal_start;
  int nal_end;
  int search_offset;
  int rc;
  int offset;
  int bit_offset;
  int first_mb_in_slice;
  int slice_type;
  int prev_first_mb_in_slice;
  int prev_nal_type;
  int local_is_key_frame;

  if (file == NULL || frame_start == NULL || frame_end == NULL ||
      is_key_frame == NULL) {
    return DEVICE_VIDEO_ERR_INVALID_ARG;
  }

  *frame_start = -1;
  *frame_end = -1;
  search_offset = 0;

  while (1) {
    rc = device_video_source_file_locate_nal(file, search_offset, &nal_type,
                                             &nal_start, &nal_end);
    if (rc != DEVICE_VIDEO_OK) {
      return rc;
    }

    if (*frame_start < 0) {
      *frame_start = nal_start;
    }
    if (nal_type == 1 || nal_type == 5) {
      break;
    }
    search_offset = nal_end + 1;
  }

  offset =
      device_video_h264_slice_payload_offset(file->data, file->size, nal_start);
  if (offset < 0 || (size_t)offset >= file->size) {
    return DEVICE_VIDEO_ERR_IO;
  }

  bit_offset = 0;
  first_mb_in_slice = device_video_exp_golomb_decode(
      file->data + offset, (int)(file->size - (size_t)offset), &bit_offset);
  slice_type = device_video_exp_golomb_decode(
      file->data + offset, (int)(file->size - (size_t)offset), &bit_offset);

  if (nal_type == 5) {
    local_is_key_frame = 1;
  } else {
    slice_type %= 5;
    local_is_key_frame = (slice_type == 2 || slice_type == 4) ? 1 : 0;
  }

  prev_first_mb_in_slice = first_mb_in_slice;
  prev_nal_type = nal_type;
  search_offset = nal_end + 1;

  while (1) {
    rc = device_video_source_file_locate_nal(file, search_offset, &nal_type,
                                             &nal_start, &nal_end);
    if (rc == DEVICE_VIDEO_ERR_EOF) {
      *frame_end = (int)file->size - 1;
      *is_key_frame = local_is_key_frame;
      return DEVICE_VIDEO_OK;
    }
    if (rc != DEVICE_VIDEO_OK) {
      return rc;
    }

    if (nal_type != prev_nal_type) {
      *frame_end = nal_start - 1;
      *is_key_frame = local_is_key_frame;
      return DEVICE_VIDEO_OK;
    }

    offset = device_video_h264_slice_payload_offset(file->data, file->size,
                                                    nal_start);
    if (offset < 0 || (size_t)offset >= file->size) {
      return DEVICE_VIDEO_ERR_IO;
    }

    bit_offset = 0;
    first_mb_in_slice = device_video_exp_golomb_decode(
        file->data + offset, (int)(file->size - (size_t)offset), &bit_offset);
    if ((prev_first_mb_in_slice > first_mb_in_slice) ||
        (prev_first_mb_in_slice == first_mb_in_slice &&
         prev_first_mb_in_slice == 0)) {
      *frame_end = nal_start - 1;
      *is_key_frame = local_is_key_frame;
      return DEVICE_VIDEO_OK;
    }

    prev_first_mb_in_slice = first_mb_in_slice;
    search_offset = nal_end + 1;
  }
}

long device_video_source_file_size(const char *path) {
  FILE *fp;
  long size;

  if (path == NULL || path[0] == '\0') {
    return -1;
  }

  fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }

  size = ftell(fp);
  fclose(fp);
  return size;
}

int device_video_source_file_validate(const device_video_config_t *config) {
  long size;

  if (config == NULL || config->input_path[0] == '\0') {
    return DEVICE_VIDEO_ERR_INVALID_ARG;
  }

  size = device_video_source_file_size(config->input_path);
  if (size < 0) {
    return DEVICE_VIDEO_ERR_IO;
  }

  return DEVICE_VIDEO_OK;
}

int device_video_source_file_open(const char *path,
                                  device_video_h264_file_t *file) {
  FILE *fp;
  int rc;

  if (path == NULL || path[0] == '\0' || file == NULL) {
    return DEVICE_VIDEO_ERR_INVALID_ARG;
  }

  memset(file, 0, sizeof(*file));
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return DEVICE_VIDEO_ERR_IO;
  }

  file->fp = fp;
  rc = device_video_source_file_reserve(file,
                                        DEVICE_VIDEO_H264_BUFFER_INIT_BYTES);
  if (rc != DEVICE_VIDEO_OK) {
    device_video_source_file_close(file);
    return rc;
  }

  return DEVICE_VIDEO_OK;
}

void device_video_source_file_reset(device_video_h264_file_t *file) {
  if (file == NULL) {
    return;
  }

  if (file->fp != NULL) {
    clearerr(file->fp);
    if (fseek(file->fp, 0, SEEK_SET) != 0) {
      file->size = 0;
      file->consumed_bytes = 0;
      file->file_end_reached = true;
      return;
    }
  }

  file->size = 0;
  file->consumed_bytes = 0;
  file->file_end_reached = false;
}

void device_video_source_file_close(device_video_h264_file_t *file) {
  if (file == NULL) {
    return;
  }

  if (file->fp != NULL) {
    fclose(file->fp);
  }

  free(file->data);
  memset(file, 0, sizeof(*file));
}

int device_video_source_file_next_frame(device_video_h264_file_t *file,
                                        const uint8_t **data_ptr,
                                        size_t *data_len, int *is_key_frame) {
  int frame_start;
  int frame_end;
  int local_is_key_frame;
  int rc;

  if (file == NULL || data_ptr == NULL || data_len == NULL ||
      is_key_frame == NULL) {
    return DEVICE_VIDEO_ERR_INVALID_ARG;
  }
  if (file->fp == NULL || file->data == NULL) {
    return DEVICE_VIDEO_ERR_IO;
  }

  device_video_source_file_compact(file);

  while (1) {
    if (file->size == 0 && !file->file_end_reached) {
      rc = device_video_source_file_fill(file);
      if (rc != DEVICE_VIDEO_OK) {
        return rc;
      }
    }
    if (file->size == 0 && file->file_end_reached) {
      return DEVICE_VIDEO_ERR_EOF;
    }

    rc = device_video_source_file_find_frame(file, &frame_start, &frame_end,
                                             &local_is_key_frame);
    if (rc == DEVICE_VIDEO_OK) {
      if (frame_end < frame_start) {
        return DEVICE_VIDEO_ERR_IO;
      }

      *data_ptr = file->data + frame_start;
      *data_len = (size_t)(frame_end - frame_start + 1);
      *is_key_frame = local_is_key_frame;
      file->consumed_bytes = (size_t)(frame_end + 1);
      return DEVICE_VIDEO_OK;
    }

    if (rc == DEVICE_VIDEO_PARSE_NEED_MORE) {
      rc = device_video_source_file_fill(file);
      if (rc != DEVICE_VIDEO_OK) {
        return rc;
      }
      continue;
    }

    return rc;
  }
}
