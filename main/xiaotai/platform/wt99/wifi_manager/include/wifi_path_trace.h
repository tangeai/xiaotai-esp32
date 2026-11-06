#ifndef WIFI_PATH_TRACE_H
#define WIFI_PATH_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Explicitly armed metadata-only diagnostic. No packet payload is retained. */
esp_err_t wifi_path_trace_start(void);
void wifi_path_trace_stop(void);
bool wifi_path_trace_active(void);
bool wifi_path_trace_transport_active(void);
esp_err_t wifi_path_trace_dump(void);
void wifi_path_trace_media(bool video);
void wifi_path_trace_transport(uint32_t media, uint32_t rtt,
                               uint32_t nack, uint32_t retx,
                               uint32_t send_loss, uint32_t receive_loss);

#endif
