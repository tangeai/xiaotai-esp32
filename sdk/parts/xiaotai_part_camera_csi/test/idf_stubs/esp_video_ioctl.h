// SPDX-License-Identifier: MIT
#pragma once

#include <sys/ioctl.h>
#include <sys/time.h>
#include "linux/videodev2.h"

#define VIDIOC_S_DQBUF_TIMEOUT _IOWR('V', BASE_VIDIOC_PRIVATE + 6, struct timeval)
