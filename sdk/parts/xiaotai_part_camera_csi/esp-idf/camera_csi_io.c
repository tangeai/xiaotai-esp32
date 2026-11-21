// SPDX-License-Identifier: MIT
#include "camera_csi_io.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_private/dw_gdma.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "xiaotai/port/log.h"

static const void *video_owner;
static bool video_ready;
static dw_gdma_channel_handle_t dma_guard;
static i2c_master_dev_handle_t sensor;

xiaotai_err_t camera_csi_io_alignment(size_t *bytes)
{
    return esp_cache_get_alignment(MALLOC_CAP_SPIRAM, bytes);
}

int camera_csi_io_errno(void) { return errno; }

static xiaotai_err_t hold_dma_clock(void)
{
    if (dma_guard) return XIAOTAI_OK;
    /* Checked in IDF 5.5.4 and 6.0.3: channel_destroy() still drops the
     * group/clock before esp_intr_free(). Keep the CSI group live until boot
     * ends so an ISR cannot read its status with the clock gated. */
    const dw_gdma_channel_alloc_config_t guard = {
        .src = {.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS, .role = DW_GDMA_ROLE_MEM,
                .handshake_type = DW_GDMA_HANDSHAKE_SW, .num_outstanding_requests = 1},
        .dst = {.block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS, .role = DW_GDMA_ROLE_MEM,
                .handshake_type = DW_GDMA_HANDSHAKE_SW, .num_outstanding_requests = 1},
        .flow_controller = DW_GDMA_FLOW_CTRL_SELF,
    };
    return dw_gdma_new_channel(&guard, &dma_guard);
}

xiaotai_err_t camera_csi_io_start(const void *owner, const xiaotai_camera_csi_config_t *config, uint32_t hz)
{
    if (video_owner && video_owner != owner) return XIAOTAI_ERR_INVALID_STATE;
    i2c_master_bus_handle_t bus = xiaotai_i2c_native(config->sccb);
    if (!bus) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = hold_dma_clock();
    if (err != XIAOTAI_OK) return err;
    video_owner = owner;
    if (video_ready) return XIAOTAI_OK;
    if (!sensor) {
        const i2c_device_config_t i2c = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                        .device_address = config->sensor_address, .scl_speed_hz = hz};
        err = i2c_master_bus_add_device(bus, &i2c, &sensor);
        if (err != XIAOTAI_OK) return err;
    }
    const esp_video_init_csi_config_t csi = {
        .sccb_config = {.init_sccb = false, .i2c_handle = bus, .freq = hz}, .reset_pin = -1, .pwdn_pin = -1,
    };
    const esp_video_init_config_t video = {.csi = &csi};
    err = esp_video_init(&video);
    if (err == XIAOTAI_OK) {
        video_ready = true;
    } else {
        xiaotai_err_t cleanup = i2c_master_bus_rm_device(sensor);
        if (cleanup == XIAOTAI_OK) sensor = NULL;
        else XIAOTAI_LOGW("camera.csi", "SCCB detach failed: 0x%x", (unsigned)cleanup);
    }
    return err;
}

xiaotai_err_t camera_csi_io_read(const void *owner, uint16_t reg, uint8_t *value)
{
    if (owner != video_owner || !video_ready) return XIAOTAI_ERR_INVALID_STATE;
    const uint8_t address[] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_master_transmit_receive(sensor, address, sizeof(address), value, 1, 20);
}

xiaotai_err_t camera_csi_io_write(const void *owner, uint16_t reg, uint8_t value)
{
    if (owner != video_owner || !video_ready) return XIAOTAI_ERR_INVALID_STATE;
    const uint8_t data[] = {(uint8_t)(reg >> 8), (uint8_t)reg, value};
    return i2c_master_transmit(sensor, data, sizeof(data), 20);
}

static xiaotai_err_t v4l_error(int error)
{
    switch (error) {
        case ENOMEM: return XIAOTAI_ERR_NO_MEM;
        case EINVAL: return XIAOTAI_ERR_INVALID_ARG;
        case EBUSY:
        case EBADF: return XIAOTAI_ERR_INVALID_STATE;
        case ENODEV: return XIAOTAI_ERR_NOT_FOUND;
        case ESRCH:
        case ENOTSUP: return XIAOTAI_ERR_NOT_SUPPORTED;
        case ETIMEDOUT: return XIAOTAI_ERR_TIMEOUT;
        default: return XIAOTAI_FAIL;
    }
}

xiaotai_err_t camera_csi_io_open(int *fd)
{
    *fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY | O_NONBLOCK);
    if (*fd < 0) return v4l_error(errno);
    struct v4l2_capability capability = {0};
    if (ioctl(*fd, VIDIOC_QUERYCAP, &capability) != 0) return v4l_error(errno);
    /* O_NONBLOCK is ignored by esp_video's VFS; DQBUF has its own timeout. */
    struct timeval timeout = {0};
    return ioctl(*fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout) == 0 ? XIAOTAI_OK : v4l_error(errno);
}

static bool native_layout(const struct v4l2_pix_format *pix, uint16_t width, uint16_t height)
{
    return pix->pixelformat == V4L2_PIX_FMT_YUV420 && pix->width == width && pix->height == height;
}

xiaotai_err_t camera_csi_io_format(int fd, uint16_t width, uint16_t height, bool full_range, size_t *bytes)
{
    struct v4l2_format active = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (ioctl(fd, VIDIOC_G_FMT, &active) != 0) return v4l_error(errno);
    struct v4l2_pix_format *pix = &active.fmt.pix;
    if (!native_layout(pix, width, height) ||
        (full_range && (pix->quantization != V4L2_QUANTIZATION_FULL_RANGE || pix->ycbcr_enc != V4L2_YCBCR_ENC_601))) {
        struct v4l2_format wanted = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .fmt.pix = {.width = width, .height = height, .pixelformat = V4L2_PIX_FMT_YUV420,
                        .field = V4L2_FIELD_NONE, .bytesperline = width,
                        .sizeimage = (size_t)width * 3U * (height / 2U),
                        .ycbcr_enc = full_range ? V4L2_YCBCR_ENC_601 : V4L2_YCBCR_ENC_DEFAULT,
                        .quantization = full_range ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_DEFAULT},
        };
        int rc = ioctl(fd, VIDIOC_S_FMT, &wanted);
        xiaotai_err_t set_error = rc == 0 ? XIAOTAI_OK : v4l_error(errno);
        if (rc != 0) XIAOTAI_LOGW("camera.csi", "set format refused: 0x%x", (unsigned)set_error);
        if (ioctl(fd, VIDIOC_G_FMT, &active) != 0) return v4l_error(errno);
        if (full_range && set_error != XIAOTAI_OK) return set_error;
    }
    if (pix->pixelformat != V4L2_PIX_FMT_YUV420) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!native_layout(pix, width, height) || (pix->bytesperline && pix->bytesperline != width)) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    if (full_range && ((pix->quantization != V4L2_QUANTIZATION_FULL_RANGE &&
                        pix->quantization != V4L2_QUANTIZATION_DEFAULT) ||
                       (pix->ycbcr_enc != V4L2_YCBCR_ENC_601 && pix->ycbcr_enc != V4L2_YCBCR_ENC_DEFAULT))) {
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    *bytes = pix->sizeimage ? pix->sizeimage : (size_t)width * 3U * (height / 2U);
    return XIAOTAI_OK;
}

xiaotai_err_t camera_csi_io_rate(int fd, uint8_t fps)
{
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    parm.parm.capture.timeperframe = (struct v4l2_fract){1, fps};
    return ioctl(fd, VIDIOC_S_PARM, &parm) == 0 ? XIAOTAI_OK : v4l_error(errno);
}

xiaotai_err_t camera_csi_io_request(int fd, unsigned count, unsigned *actual)
{
    struct v4l2_requestbuffers req = {
        .count = count, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) return v4l_error(errno);
    if (actual) *actual = req.count;
    return XIAOTAI_OK;
}

static struct v4l2_buffer user_buffer(unsigned index)
{
    return (struct v4l2_buffer){.index = index, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_USERPTR};
}

xiaotai_err_t camera_csi_io_buffer_size(int fd, unsigned index, size_t *bytes)
{
    struct v4l2_buffer buf = user_buffer(index);
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) return v4l_error(errno);
    *bytes = buf.length;
    return XIAOTAI_OK;
}

xiaotai_err_t camera_csi_io_queue(int fd, unsigned index, void *data, size_t capacity)
{
    struct v4l2_buffer buf = user_buffer(index);
    buf.m.userptr = (unsigned long)(uintptr_t)data;
    buf.length = capacity;
    return ioctl(fd, VIDIOC_QBUF, &buf) == 0 ? XIAOTAI_OK : v4l_error(errno);
}

xiaotai_err_t camera_csi_io_dequeue(int fd, camera_csi_buffer_t *buffer)
{
    struct v4l2_buffer buf = user_buffer(0);
    if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
        int error = errno;
        return error == EAGAIN || error == ETIMEDOUT ? XIAOTAI_ERR_NOT_FOUND : v4l_error(error);
    }
    int64_t us = 0;
    if (buf.timestamp.tv_sec >= 0 && buf.timestamp.tv_usec >= 0 && buf.timestamp.tv_usec < 1000000 &&
        (uint64_t)buf.timestamp.tv_sec <= (INT64_MAX - (uint64_t)buf.timestamp.tv_usec) / 1000000U) {
        us = (int64_t)buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;
    }
    *buffer = (camera_csi_buffer_t){.index = buf.index, .sequence = buf.sequence, .bytes = buf.bytesused,
                                   .captured_us = us, .error = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0};
    return XIAOTAI_OK;
}

xiaotai_err_t camera_csi_io_stream(int fd, bool start)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return ioctl(fd, start ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type) == 0 ? XIAOTAI_OK : v4l_error(errno);
}

xiaotai_err_t camera_csi_io_close(int fd)
{
    return close(fd) == 0 ? XIAOTAI_OK : v4l_error(errno);
}

xiaotai_err_t camera_csi_io_quality(int fd, camera_csi_quality_t *quality)
{
    struct v4l2_ext_control values[2] = {{.id = V4L2_CID_EXPOSURE}, {.id = V4L2_CID_GAIN}};
    struct v4l2_ext_controls controls = {.ctrl_class = V4L2_CTRL_CLASS_USER, .count = 2, .controls = values};
    struct v4l2_query_ext_ctrl exposure = {.id = V4L2_CID_EXPOSURE}, gain = {.id = V4L2_CID_GAIN};
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) != 0 || ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &exposure) != 0 ||
        ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &gain) != 0) return v4l_error(errno);
    if (values[0].value < exposure.minimum || values[0].value > exposure.maximum ||
        values[1].value < gain.minimum || values[1].value > gain.maximum) return XIAOTAI_FAIL;
    *quality = (camera_csi_quality_t){.exposure = values[0].value, .gain = values[1].value,
                                     .exposure_min = exposure.minimum, .exposure_max = exposure.maximum,
                                     .gain_min = gain.minimum, .gain_max = gain.maximum};
    if (gain.type != V4L2_CTRL_TYPE_INTEGER_MENU) return XIAOTAI_OK;
    if (gain.minimum < 0 || gain.maximum > UINT32_MAX) return XIAOTAI_FAIL;
    struct v4l2_querymenu base = {.id = V4L2_CID_GAIN, .index = gain.minimum};
    struct v4l2_querymenu current = {.id = V4L2_CID_GAIN, .index = values[1].value};
    struct v4l2_querymenu maximum = {.id = V4L2_CID_GAIN, .index = gain.maximum};
    if (ioctl(fd, VIDIOC_QUERYMENU, &base) != 0 || ioctl(fd, VIDIOC_QUERYMENU, &current) != 0 ||
        ioctl(fd, VIDIOC_QUERYMENU, &maximum) != 0) return v4l_error(errno);
    quality->has_gain_menu = true;
    quality->gain_base = base.value;
    quality->gain_value = current.value;
    quality->gain_limit = maximum.value;
    return XIAOTAI_OK;
}
