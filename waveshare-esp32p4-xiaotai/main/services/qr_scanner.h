#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "scan_preview.h"

#define QR_SCANNER_DEVICE_ID_MAX 64
#define QR_SCANNER_OPEN_ID_MAX   96
#define QR_SCANNER_PAIR_KEY_MAX  128
#define QR_SCANNER_PAYLOAD_MAX   640

typedef struct {
	char device_id[QR_SCANNER_DEVICE_ID_MAX];
	char open_id[QR_SCANNER_OPEN_ID_MAX];
	char pair_key[QR_SCANNER_PAIR_KEY_MAX];
	char raw_payload[QR_SCANNER_PAYLOAD_MAX];
} qr_scanner_contact_t;

typedef scan_preview_cb_t qr_scanner_preview_cb_t;
typedef void (*qr_scanner_result_cb_t)(esp_err_t result,
				       const qr_scanner_contact_t *contact,
				       void *ctx);

esp_err_t qr_scanner_scan_contact(qr_scanner_contact_t *contact);
esp_err_t qr_scanner_parse_contact_payload(const char *payload_text,
					   qr_scanner_contact_t *contact);
esp_err_t qr_scanner_start_contact(qr_scanner_preview_cb_t preview_cb,
				   qr_scanner_result_cb_t result_cb,
				   void *ctx);
esp_err_t qr_scanner_stop(void);
