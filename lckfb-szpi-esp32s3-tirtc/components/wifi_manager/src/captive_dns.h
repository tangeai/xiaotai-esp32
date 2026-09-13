#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

#include "esp_err.h"
#include "esp_netif.h"

/** Start one wildcard IPv4 DNS responder for the SoftAP; repeated calls are safe. */
esp_err_t captive_dns_start(esp_netif_t *ap_netif);
/* Start/stop are serialized by the Wi-Fi lifecycle owner. The worker closes
 * its own socket; stop never deletes another task or closes a reused fd. */
esp_err_t captive_dns_stop(void);

#endif
