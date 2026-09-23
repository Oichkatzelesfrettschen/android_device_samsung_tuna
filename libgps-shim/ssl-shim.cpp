/* SPDX-License-Identifier: Apache-2.0 */

#include <openssl/ssl.h>

/*
 * gps.omap4.so imports SSLv3_client_method() for its SUPL TLS client.
 * BoringSSL has no SSLv3; TLS_client_method() negotiates the highest
 * version both ends support. sirfgps.conf sets SSL_ENABLED=0, so the SiRF
 * library resolves the symbol at load time and does not call it.
 */
extern "C" const SSL_METHOD* SSLv3_client_method(void) {
    return TLS_client_method();
}
