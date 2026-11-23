// SPDX-License-Identifier: MIT
#pragma once

typedef void *dw_gdma_channel_handle_t;
enum { DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS, DW_GDMA_ROLE_MEM, DW_GDMA_HANDSHAKE_SW, DW_GDMA_FLOW_CTRL_SELF };
typedef struct {
    unsigned block_transfer_type, role, handshake_type, num_outstanding_requests;
} csi_test_dma_end_t;
typedef struct {
    csi_test_dma_end_t src, dst;
    unsigned flow_controller;
} dw_gdma_channel_alloc_config_t;

int dw_gdma_new_channel(const dw_gdma_channel_alloc_config_t *cfg, dw_gdma_channel_handle_t *channel);
