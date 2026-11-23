// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

/* Select the pinned Mini retirement contract before starting the product. */
void product_ai_prepare_retirement_backend(void);
uint32_t product_ai_link(uint32_t session);
void product_ai_wait_disconnect(uint32_t previous);

/* Drive the real module from the owner; split request and connection so the
 * presentation test can observe the real CONNECTING event in between. */
void product_ai_prepare(void);
uint32_t product_ai_request(void);
uint32_t product_ai_pending_request(void);
void product_ai_connect(uint32_t session);
/* Run after the actual receive route is armed but before admitting playout. */
void product_ai_connect_armed(uint32_t session, void (*observe)(uintptr_t peer));
void product_ai_fixed_receive(void);
void product_ai_stop(uint32_t session);
