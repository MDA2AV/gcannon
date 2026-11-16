#pragma once

#include "proto/ktls.h"

/*
 * https://: the TLS 1.3 handshake.
 *
 * OpenSSL runs the handshake directly on the socket, and all that comes out of
 * it are the TLS 1.3 traffic secrets. Those become kernel TLS keys
 * (proto/ktls.c) and the SSL object is freed, so from then on the connection
 * is the same plain fd as for http:// — the kernel just encrypts what goes
 * through it.
 *
 * The handshake is non-blocking. Each call drives it as far as the socket
 * allows and says what to do next; the caller owns the waiting and the
 * reconnecting.
 */

struct ssl_st;
struct ssl_ctx_st;

typedef enum {
    TLS_DONE,        /* keys are in kernel TLS and the SSL object is freed:
                        the connection is ready */
    TLS_WANT_READ,   /* call tls_step() once the socket is readable */
    TLS_WANT_WRITE,  /* ...once it is writable */
    TLS_FAILED,      /* the handshake or key setup failed; the first failure
                        of a run is on stderr */
} tls_status_t;

/* Client settings for every connection of a worker: TLS 1.3 only, the three
 * suites kernel TLS supports, no certificate check. NULL on failure. */
struct ssl_ctx_st *tls_ctx_new(void);
void tls_ctx_free(struct ssl_ctx_st *ctx);

/* Start the handshake on a connected socket. sni is the server name, or NULL
 * for an IP address. *ssl lives until the handshake ends either way. */
tls_status_t tls_start(struct ssl_ctx_st *ctx, const char *sni, int fd,
                       struct ssl_st **ssl, ktls_secrets_t *secrets);

/* Drive a handshake that was waiting for the socket */
tls_status_t tls_step(int fd, struct ssl_st **ssl, ktls_secrets_t *secrets);

/* Free a connection's SSL object, if the handshake left one */
void tls_free(struct ssl_st **ssl);

/* Report the first TLS failure of a run on stderr, so a target that won't do
 * TLS 1.3 says why instead of only piling up errors */
void tls_report(const char *what, const char *why);
