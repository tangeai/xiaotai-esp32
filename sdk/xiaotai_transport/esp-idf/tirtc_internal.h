// SPDX-License-Identifier: MIT
#pragma once

#include "transport_backend.h"
#include "tirtc/tiRTC.h"

#if !defined(TIRTC_SDK_SSL_SUPPORT) || (TIRTC_SDK_SSL_SUPPORT != 0 && TIRTC_SDK_SSL_SUPPORT != 1)
#error TiRTC TLS capability must come from the selected archive contract
#endif

#if __has_include("tirtc/tgtrp.h")
#include "tirtc/tgtrp.h"
#define XT_TIRTC_TGTRP 1
#else
#define XT_TIRTC_TGTRP 0
#endif

#if defined(TIRTC_PENDING_CLOSE_CONTRACT)
#if TIRTC_PENDING_CLOSE_CONTRACT != 1
#error Unknown TiRTC pending-close ABI; recheck the locked Mini package
#endif
#define XT_TIRTC_CANCEL 1
#else
#define XT_TIRTC_CANCEL 0
#endif
#if defined(XIAOTAI_TIRTC_EXPECT_PENDING_CLOSE) && !XT_TIRTC_CANCEL
#error Selected TiRTC Mini archive/header pending-close contract mismatch
#endif

/* The locked Mini archive exports TGTRP logging but not the media-enable
 * function listed in its shared TGTRP header. */
#define XT_TIRTC_MEDIA_TRANSPORT (XT_TIRTC_TGTRP && !XT_TIRTC_CANCEL)

extern const TIRTCCALLBACKS xt_tirtc_callbacks;
#if defined(XIAOTAI_TIRTC_IDLE_STACK_POLICY)
int xt_tirtc_start_with_idle_stack(const char *id, const TIRTCCALLBACKS *callbacks, uint32_t stack_bytes);
#endif
xiaotai_err_t xt_tirtc_status(int result);
void xt_tirtc_log_configure(uint8_t level);
void xt_tirtc_log_message(void);
