// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/modules/contacts.h"

xiaotai_err_t companion_dial_contact(const xiaotai_contact_t *contact, bool video);
bool companion_contact_video_supported(const xiaotai_contact_t *contact);
