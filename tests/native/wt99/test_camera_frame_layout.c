#include <assert.h>
#include <stdlib.h>
#include "../../../main/xiaotai/media/wt99/p4_hardware/native/drivers/camera/camera_frame_layout.h"

int main(void)
{
    const size_t visible = 1024U * 600U * 3U / 2U;
    const size_t padded = 1024U * 608U * 3U / 2U;
    const size_t pair = 1024U * 3U;
    uint8_t *frame = malloc(padded + 16U);
    assert(frame);
    memset(frame, 0xa5, padded + 16U);
    for (size_t i = 0; i < pair; ++i) frame[visible - pair + i] = (uint8_t)i;
    assert(camera_frame_pad_yuv420(frame, visible - 1U, padded, 1024, 600) == 0);
    assert(camera_frame_pad_yuv420(frame, visible, padded - 1U, 1024, 600) == 0);
    assert(frame[visible] == 0xa5);
    assert(camera_frame_pad_yuv420(frame, visible, padded, 1023, 600) == 0);
    assert(camera_frame_pad_yuv420(frame, visible, padded, 1024, 601) == 0);
    assert(camera_frame_pad_yuv420(NULL, visible, padded, 1024, 600) == 0);
    assert(camera_frame_pad_yuv420(frame, visible, padded, 1024, 600) == padded);
    for (size_t offset = visible; offset < padded; offset += pair)
        assert(memcmp(frame + offset, frame + visible - pair, pair) == 0);
    for (size_t i = 0; i < 16U; ++i) assert(frame[padded + i] == 0xa5);
    assert(camera_frame_pad_yuv420(frame, padded, padded, 1024, 608) == padded);
    free(frame);
    return 0;
}
