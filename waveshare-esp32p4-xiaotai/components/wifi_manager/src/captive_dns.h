#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

#include "esp_err.h"
#include "esp_netif.h"

/** Start one wildcard IPv4 DNS responder for the SoftAP; repeated calls are safe. */
esp_err_t captive_dns_start(esp_netif_t *ap_netif);
/* Start/stop are serialized by the Wi-Fi lifecycle owner. The worker closes
 * its socket before acknowledging stop; the owner then frees its PSRAM task.
 * A stop timeout retains ownership for a later stop, never forcing deletion. */
esp_err_t captive_dns_stop(void);

#endif
