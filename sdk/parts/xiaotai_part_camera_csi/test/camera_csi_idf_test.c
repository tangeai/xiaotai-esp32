// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_camera_frame_contract.py
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "camera_csi_io.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_private/dw_gdma.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"

static unsigned guard, init, added, removed, timeout, formats, closed;
static unsigned char sensor, dma;
static void *native_bus;
static int bus_speed;
static bool device_open;
static unsigned long failing_op;
static int op_errno;
static struct v4l2_format active = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
static uintptr_t userptr;

int esp_cache_get_alignment(uint32_t caps, size_t *bytes)
{
    assert(caps == MALLOC_CAP_SPIRAM);
    *bytes = 128U;
    return XIAOTAI_OK;
}

int dw_gdma_new_channel(const dw_gdma_channel_alloc_config_t *cfg, dw_gdma_channel_handle_t *channel)
{
    assert(++guard == 1 && cfg->src.num_outstanding_requests == 1 && cfg->dst.num_outstanding_requests == 1);
    assert(cfg->src.handshake_type == DW_GDMA_HANDSHAKE_SW && cfg->dst.handshake_type == DW_GDMA_HANDSHAKE_SW);
    *channel = &dma;
    return XIAOTAI_OK;
}

int i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                              i2c_master_dev_handle_t *dev)
{
    assert(guard == 1 && bus == native_bus && cfg->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    assert(cfg->device_address == 0x30 && cfg->scl_speed_hz == 400000);
    added++;
    bus_speed = cfg->scl_speed_hz;
    *dev = &sensor;
    return XIAOTAI_OK;
}

int i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
    assert(dev == &sensor && ++removed == 1);
    return XIAOTAI_OK;
}

int i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *cmd, size_t cmd_len,
                                uint8_t *out, size_t size, int wait)
{
    assert(dev == &sensor && cmd_len == 2 && size == 1 && wait == 20 && cmd[0] == 0x31 && cmd[1] == 0x07);
    *out = 0xcb;
    return XIAOTAI_OK;
}

int i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *cmd, size_t size, int wait)
{
    assert(dev == &sensor && size == 3 && wait == 20 && cmd[0] == 0x3a && cmd[1] == 0x00 && cmd[2] == 0x20);
    return XIAOTAI_OK;
}

int esp_video_init(const esp_video_init_config_t *video)
{
    const esp_video_init_csi_config_t *cfg = video->csi;
    assert(guard == 1 && added == init + 1 && cfg->reset_pin == -1 && cfg->pwdn_pin == -1);
    assert(!cfg->sccb_config.init_sccb && cfg->sccb_config.i2c_handle == native_bus);
    assert(cfg->sccb_config.freq == (uint32_t)bus_speed);
    return ++init == 1 ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
}

int csi_test_open(const char *path, int flags, ...)
{
    assert(strcmp(path, ESP_VIDEO_MIPI_CSI_DEVICE_NAME) == 0 && flags == (O_RDONLY | O_NONBLOCK));
    assert(!device_open);
    device_open = true;
    return 9;
}

int csi_test_close(int fd)
{
    assert(fd == 9 && device_open);
    device_open = false;
    ++closed;
    return 0;
}

int csi_test_ioctl(int fd, unsigned long request, ...)
{
    assert(device_open && fd == 9);
    if (request == failing_op) {
        errno = op_errno;
        return -1;
    }
    va_list args;
    va_start(args, request);
    switch (request) {
        case VIDIOC_QUERYCAP: memset(va_arg(args, struct v4l2_capability *), 0, sizeof(struct v4l2_capability)); break;
        case VIDIOC_S_DQBUF_TIMEOUT: {
            const struct timeval *tv = va_arg(args, struct timeval *);
            assert(tv->tv_sec == 0 && tv->tv_usec == 0);
            ++timeout;
            break;
        }
        case VIDIOC_G_FMT: *va_arg(args, struct v4l2_format *) = active; break;
        case VIDIOC_S_FMT: {
            active = *va_arg(args, struct v4l2_format *);
            const struct v4l2_pix_format *pix = &active.fmt.pix;
            assert(pix->width == 1024 && pix->height == 600 && pix->pixelformat == V4L2_PIX_FMT_YUV420);
            assert(pix->quantization == V4L2_QUANTIZATION_FULL_RANGE && pix->ycbcr_enc == V4L2_YCBCR_ENC_601);
            assert(pix->bytesperline == 1024 && pix->sizeimage == 921600);
            ++formats;
            break;
        }
        case VIDIOC_S_PARM: {
            const struct v4l2_streamparm *parm = va_arg(args, struct v4l2_streamparm *);
            assert(parm->type == V4L2_BUF_TYPE_VIDEO_CAPTURE && parm->parm.capture.timeperframe.numerator == 1);
            assert(parm->parm.capture.timeperframe.denominator == 27);
            break;
        }
        case VIDIOC_REQBUFS: {
            const struct v4l2_requestbuffers *req = va_arg(args, struct v4l2_requestbuffers *);
            assert(req->count == 3 || req->count == 0);
            assert(req->type == V4L2_BUF_TYPE_VIDEO_CAPTURE && req->memory == V4L2_MEMORY_USERPTR);
            break;
        }
        case VIDIOC_QUERYBUF:
        case VIDIOC_QBUF:
        case VIDIOC_DQBUF: {
            struct v4l2_buffer *buffer = va_arg(args, struct v4l2_buffer *);
            assert(buffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE && buffer->memory == V4L2_MEMORY_USERPTR);
            if (request == VIDIOC_QUERYBUF) {
                assert(buffer->index == 1);
                buffer->length = 921600;
            } else if (request == VIDIOC_QBUF) {
                assert(buffer->index == 1 && buffer->length == 933888);
                if (buffer->m.userptr % 128U) {
                    va_end(args);
                    errno = EINVAL;
                    return -1;
                }
                userptr = buffer->m.userptr;
            } else {
                assert(timeout > 0);
                buffer->index = 1;
                buffer->bytesused = 921600;
                buffer->sequence = 13;
                buffer->timestamp = (struct timeval){.tv_sec = 1, .tv_usec = 17};
                buffer->flags = V4L2_BUF_FLAG_ERROR;
            }
            break;
        }
        case VIDIOC_STREAMON:
        case VIDIOC_STREAMOFF: assert(*va_arg(args, int *) == V4L2_BUF_TYPE_VIDEO_CAPTURE); break;
        case VIDIOC_G_EXT_CTRLS: {
            struct v4l2_ext_controls *c = va_arg(args, struct v4l2_ext_controls *);
            assert(c->ctrl_class == V4L2_CTRL_CLASS_USER && c->count == 2);
            assert(c->controls[0].id == V4L2_CID_EXPOSURE && c->controls[1].id == V4L2_CID_GAIN);
            c->controls[0].value = 145;
            c->controls[1].value = 5;
            break;
        }
        case VIDIOC_QUERY_EXT_CTRL: {
            struct v4l2_query_ext_ctrl *c = va_arg(args, struct v4l2_query_ext_ctrl *);
            bool gain = c->id == V4L2_CID_GAIN;
            assert(gain || c->id == V4L2_CID_EXPOSURE);
            c->minimum = 1;
            c->maximum = gain ? 7 : 900;
            c->type = gain ? V4L2_CTRL_TYPE_INTEGER_MENU : V4L2_CTRL_TYPE_INTEGER;
            break;
        }
        case VIDIOC_QUERYMENU: {
            struct v4l2_querymenu *c = va_arg(args, struct v4l2_querymenu *);
            assert(c->id == V4L2_CID_GAIN && (c->index == 1 || c->index == 5 || c->index == 7));
            c->value = 1024 * c->index;
            break;
        }
        default: assert(0);
    }
    va_end(args);
    return 0;
}

int main(void)
{
    static xiaotai_i2c_bus_t *bus;
    const xiaotai_i2c_config_t config = {.port = 0, .sda_pin = 2, .scl_pin = 4, .hz = 400000};
    assert(xiaotai_i2c_create(&config, &bus) == XIAOTAI_OK);
    native_bus = xiaotai_i2c_native(bus);
    assert(native_bus);
    const xiaotai_camera_csi_config_t cfg = {.sccb = bus, .sensor_address = 0x30};
    assert(camera_csi_io_start(&guard, &cfg, 400000) == XIAOTAI_ERR_TIMEOUT);
    assert(camera_csi_io_start(&init, &cfg, 400000) == XIAOTAI_ERR_INVALID_STATE && removed == 1);
    assert(camera_csi_io_start(&guard, &cfg, 400000) == XIAOTAI_OK && init == 2 && added == 2);
    assert(camera_csi_io_start(&guard, &cfg, 400000) == XIAOTAI_OK && init == 2 && guard == 1);
    uint8_t pid;
    assert(camera_csi_io_read(&guard, 0x3107, &pid) == XIAOTAI_OK && pid == 0xcb);
    assert(camera_csi_io_write(&guard, 0x3a00, 0x20) == XIAOTAI_OK);
    assert(camera_csi_io_read(&init, 0x3107, &pid) == XIAOTAI_ERR_INVALID_STATE);
    int fd = -1;
    failing_op = VIDIOC_S_DQBUF_TIMEOUT;
    op_errno = EINVAL;
    assert(camera_csi_io_open(&fd) == XIAOTAI_ERR_INVALID_ARG && fd == 9);
    assert(camera_csi_io_errno() == EINVAL);
    assert(camera_csi_io_close(fd) == XIAOTAI_OK && closed == 1);
    failing_op = 0;
    assert(camera_csi_io_open(&fd) == XIAOTAI_OK && timeout == 1);
    size_t bytes;
    assert(camera_csi_io_format(fd, 1024, 600, true, &bytes) == XIAOTAI_OK && bytes == 921600 && formats == 1);
    assert(camera_csi_io_format(fd, 1024, 600, true, &bytes) == XIAOTAI_OK && formats == 1);
    assert(camera_csi_io_rate(fd, 27) == XIAOTAI_OK);
    unsigned actual;
    assert(camera_csi_io_request(fd, 3, &actual) == XIAOTAI_OK && actual == 3);
    assert(camera_csi_io_buffer_size(fd, 1, &bytes) == XIAOTAI_OK && bytes == 921600);
    size_t alignment = 0;
    assert(camera_csi_io_alignment(&alignment) == XIAOTAI_OK && alignment == 128U);
    _Alignas(128) unsigned char user_buffer[128];
    assert(camera_csi_io_queue(fd, 1, user_buffer + 64U, 933888) == XIAOTAI_ERR_INVALID_ARG);
    assert(camera_csi_io_errno() == EINVAL && !userptr);
    assert(camera_csi_io_queue(fd, 1, user_buffer, 933888) == XIAOTAI_OK && userptr == (uintptr_t)user_buffer);
    assert(camera_csi_io_stream(fd, true) == XIAOTAI_OK);
    camera_csi_buffer_t buffer;
    failing_op = VIDIOC_DQBUF;
    op_errno = EAGAIN;
    assert(camera_csi_io_dequeue(fd, &buffer) == XIAOTAI_ERR_NOT_FOUND);
    op_errno = ETIMEDOUT;
    assert(camera_csi_io_dequeue(fd, &buffer) == XIAOTAI_ERR_NOT_FOUND);
    op_errno = EBUSY;
    assert(camera_csi_io_dequeue(fd, &buffer) == XIAOTAI_ERR_INVALID_STATE);
    failing_op = 0;
    assert(camera_csi_io_dequeue(fd, &buffer) == XIAOTAI_OK && buffer.index == 1 && buffer.sequence == 13);
    assert(buffer.bytes == 921600 && buffer.error && buffer.captured_us == 1000017);
    camera_csi_quality_t q;
    assert(camera_csi_io_quality(fd, &q) == XIAOTAI_OK && q.exposure == 145 && q.has_gain_menu);
    assert(q.gain_value == 5120 && q.gain_base == 1024 && q.gain_limit == 7168);
    assert(camera_csi_io_stream(fd, false) == XIAOTAI_OK);
    assert(camera_csi_io_request(fd, 0, NULL) == XIAOTAI_OK);
    assert(camera_csi_io_close(fd) == XIAOTAI_OK && closed == 2);
}
