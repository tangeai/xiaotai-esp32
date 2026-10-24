#include <stddef.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/netif.h"
#include "mbedtls/aes.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/des.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "tiRTC.h"
#if !defined(TIRTC_PENDING_CLOSE_CONTRACT) || TIRTC_PENDING_CLOSE_CONTRACT != 1
#error "Xmini requires the verified pending-connection retirement SDK contract"
#endif
#if !CONFIG_IDF_TARGET_ESP32C3 || CONFIG_FREERTOS_HZ != 1000
#error "TiRTC C3 archive requires ESP32-C3 and 1000 Hz ticks"
#endif
#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
#error "TiRTC C3 archive requires trace disabled"
#endif
_Static_assert(sizeof(StaticSemaphore_t) == 84, "TiRTC FreeRTOS ABI mismatch");
_Static_assert(TIRTC_VERSION_MAJOR == 2 && TIRTC_VERSION_MINOR == 5 &&
               TIRTC_VERSION_PATCH == 0, "Unexpected TiRTC headers");

/* The archive is compiled against its own sdkconfig, not this application's.
 * httpclt.o embeds these mbedTLS objects by value (My_malloc(892): conf+0,
 * entropy+140, ctr_drbg+560, net+636, ssl+640) and calls this application's
 * mbedTLS on them; crypto_wrapper.o keeps DES/AES contexts on its stack and
 * net_basic.o reads struct netif fields directly. Any option that changes these
 * layouts makes the application's code overwrite neighbouring SDK objects.
 * MBEDTLS_DHM_C did exactly that: dhm_min_bitlen landed on the entropy SHA-512
 * context pointer and leaked 216 B per SDK HTTPS connection. */
#if defined(CONFIG_MBEDTLS_DHM_C) || defined(MBEDTLS_DHM_C) || defined(MBEDTLS_THREADING_C)
#error "TiRTC C3 archive was built without MBEDTLS_DHM_C/THREADING_C; its mbedTLS object layout would differ"
#endif
typedef struct {
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_net_context net_ctx;
    mbedtls_ssl_context ssl;
} tirtc_httpclt_session_abi_t;
_Static_assert(sizeof(mbedtls_ssl_config) == 140, "TiRTC httpclt ABI: mbedtls_ssl_config");
_Static_assert(sizeof(mbedtls_entropy_context) == 420, "TiRTC httpclt ABI: entropy");
_Static_assert(sizeof(mbedtls_ctr_drbg_context) == 76, "TiRTC httpclt ABI: ctr_drbg");
_Static_assert(sizeof(mbedtls_net_context) == 4, "TiRTC httpclt ABI: net");
_Static_assert(sizeof(mbedtls_ssl_context) == 252, "TiRTC httpclt ABI: ssl");
_Static_assert(offsetof(tirtc_httpclt_session_abi_t, entropy) == 140 &&
               offsetof(tirtc_httpclt_session_abi_t, ctr_drbg) == 560 &&
               offsetof(tirtc_httpclt_session_abi_t, net_ctx) == 636 &&
               offsetof(tirtc_httpclt_session_abi_t, ssl) == 640 &&
               sizeof(tirtc_httpclt_session_abi_t) == 892, "TiRTC httpclt ABI: session layout");
_Static_assert(sizeof(mbedtls_des_context) == 128, "TiRTC crypto_wrapper ABI: des");
_Static_assert(sizeof(mbedtls_aes_context) == 34, "TiRTC crypto_wrapper ABI: esp_aes");
_Static_assert(offsetof(struct netif, next) == 0 && offsetof(struct netif, ip_addr) == 4 &&
               offsetof(struct netif, netmask) == 28 && offsetof(struct netif, name) == 228 &&
               offsetof(struct netif, num) == 230, "TiRTC net_basic ABI: struct netif");
