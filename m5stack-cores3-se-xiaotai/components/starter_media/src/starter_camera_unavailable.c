#include "starter_camera.h"
#include "starter_media.h"
#include <string.h>

/* CoreS3-SE has no camera. Report this hardware fact, never a fake frame. */
esp_err_t starter_camera_prepare(uint32_t generation)
{
    (void)generation;
    return ESP_ERR_NOT_SUPPORTED;
}

void starter_camera_set_session(uint32_t generation)
{
    (void)generation;
}

starter_camera_status_t starter_media_camera_status(void)
{
    return (starter_camera_status_t){0};
}
