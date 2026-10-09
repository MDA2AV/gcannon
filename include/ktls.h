#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Kernel TLS (kTLS) for TLS 1.3 client connections.
 *
 * A TLS library runs the handshake. Once it completes, the two application
 * traffic secrets are turned into record keys here and handed to the kernel,
 * which from then on encrypts every send and decrypts every receive on the
 * plain socket fd. The io_uring send/recv path never touches TLS records.
 *
 * Only TLS 1.3 is supported, with the three cipher suites the kernel offers
 * for it.
 */

#define KTLS_SECRET_MAX 48   /* SHA-384, the largest TLS 1.3 hash */

typedef enum {
    KTLS_SUITE_NONE = 0,
    KTLS_AES_128_GCM_SHA256,
    KTLS_AES_256_GCM_SHA384,
    KTLS_CHACHA20_POLY1305_SHA256,
} ktls_suite_t;

/* Application traffic secrets of one connection, as captured from the TLS
 * library's key log callback. */
typedef struct {
    uint8_t client[KTLS_SECRET_MAX];
    uint8_t server[KTLS_SECRET_MAX];
    uint8_t client_len;
    uint8_t server_len;
} ktls_secrets_t;

/* Suite for an IANA TLS 1.3 suite name such as "TLS_AES_128_GCM_SHA256", or
 * KTLS_SUITE_NONE if the kernel can't take it. */
ktls_suite_t ktls_suite_from_name(const char *name);

/* Record one line of NSS key log output (the format SSL_CTX_set_keylog_callback
 * delivers). CLIENT_TRAFFIC_SECRET_0 and SERVER_TRAFFIC_SECRET_0 are stored;
 * every other line is ignored. Returns 1 if the line was stored, else 0. */
int ktls_keylog_line(ktls_secrets_t *s, const char *line);

/* Record key and IV for a traffic secret (RFC 8446 section 7.3). key must hold
 * 32 bytes. Returns the key length, or -1 on a bad suite or secret length. */
int ktls_derive(ktls_suite_t suite, const uint8_t *secret, size_t secret_len,
                uint8_t key[32], uint8_t iv[12]);

/* Attach the "tls" ULP to a connected TCP socket and install both directions.
 * Record sequence numbers start at zero, so call this before any application
 * record has been sent or read. Returns 0, or -errno. */
int ktls_install(int fd, ktls_suite_t suite,
                 const uint8_t *tx_secret, const uint8_t *rx_secret,
                 size_t secret_len);

/* Whether this kernel can do kTLS: attaches the ULP to a throwaway loopback
 * connection. Returns 0, or -errno (-ENOENT when the tls module isn't
 * loaded). */
int ktls_probe(void);
