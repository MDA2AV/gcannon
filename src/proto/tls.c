#include "proto/tls.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

/* Key log callback: the only public way OpenSSL 3.0 hands out the secrets */
static void tls_keylog(const SSL *ssl, const char *line)
{
    ktls_secrets_t *k = SSL_get_app_data(ssl);
    if (k) ktls_keylog_line(k, line);
}

SSL_CTX *tls_ctx_new(void)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    /* Kernel TLS carries TLS 1.3 records in these three suites, so offer
       nothing it can't take. AES-128-GCM first, the order browsers use. */
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) ||
        !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256:"
                                       "TLS_AES_256_GCM_SHA384:"
                                       "TLS_CHACHA20_POLY1305_SHA256")) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    /* Benchmark targets mostly run with self-signed or test certificates */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    /* The receive path drops session tickets, so there is nothing to cache */
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_keylog_callback(ctx, tls_keylog);
#ifdef SSL_OP_ENABLE_KTLS
    /* Both directions are installed below. OpenSSL's own kTLS, if a system
       config turns it on, would claim the transmit side first. */
    SSL_CTX_clear_options(ctx, SSL_OP_ENABLE_KTLS);
#endif
    return ctx;
}

void tls_ctx_free(SSL_CTX *ctx)
{
    SSL_CTX_free(ctx);
}

/* The first TLS failure of a run goes to stderr, so a target that won't do
   TLS 1.3 says why instead of only piling up connect errors. */
void tls_report(const char *what, const char *why)
{
    static atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&reported))
        fprintf(stderr, "TLS %s failed: %s\n", what, why);
}

static tls_status_t tls_finish(const int fd, SSL **ssl, ktls_secrets_t *k)
{
    const char *name = SSL_CIPHER_get_name(SSL_get_current_cipher(*ssl));
    const ktls_suite_t suite = ktls_suite_from_name(name);
    char why[128];
    int rc = -EPROTO;

    if (!suite) {
        snprintf(why, sizeof(why), "kernel TLS can't carry %s", name ? name : "the suite");
        tls_report("setup", why);
    } else if (!k->client_len || k->client_len != k->server_len) {
        tls_report("setup", "the handshake produced no traffic secrets");
    } else if ((rc = ktls_install(fd, suite, k->client, k->server, k->client_len)) < 0) {
        snprintf(why, sizeof(why), "installing kernel TLS keys: %s", strerror(-rc));
        tls_report("setup", why);
    }

    OPENSSL_cleanse(k, sizeof(*k));
    SSL_free(*ssl);   /* SSL_set_fd doesn't own the fd, so it stays open */
    *ssl = NULL;

    return rc < 0 ? TLS_FAILED : TLS_DONE;
}

/* Drive the handshake as far as the socket allows */
tls_status_t tls_step(const int fd, SSL **ssl, ktls_secrets_t *secrets)
{
    ERR_clear_error();   /* SSL_get_error reads this thread's error queue */
    const int r = SSL_do_handshake(*ssl);
    if (r == 1)
        return tls_finish(fd, ssl, secrets);
    const int err = SSL_get_error(*ssl, r);
    if (err == SSL_ERROR_WANT_READ)
        return TLS_WANT_READ;
    if (err == SSL_ERROR_WANT_WRITE)
        return TLS_WANT_WRITE;

    char why[256];
    const unsigned long e = ERR_peek_last_error();
    const char *reason = e ? ERR_reason_error_string(e) : NULL;
    if (reason)
        snprintf(why, sizeof(why), "%s", reason);
    else if (e)
        ERR_error_string_n(e, why, sizeof(why));
    else if (err == SSL_ERROR_SYSCALL && errno)
        snprintf(why, sizeof(why), "%s", strerror(errno));
    else
        snprintf(why, sizeof(why), "server closed the connection");
    tls_report("handshake", why);
    ERR_clear_error();
    return TLS_FAILED;
}

tls_status_t tls_start(SSL_CTX *ctx, const char *sni, const int fd,
                       SSL **ssl, ktls_secrets_t *secrets)
{
    memset(secrets, 0, sizeof(*secrets));
    /* OpenSSL reads one whole record at a time (no read-ahead), so when the
       handshake ends nothing past the server's Finished has left the socket:
       kernel TLS starts at the first application record, sequence 0. */
    *ssl = SSL_new(ctx);
    if (!*ssl || !SSL_set_fd(*ssl, fd)) {
        tls_report("setup", "out of memory");
        return TLS_FAILED;
    }
    SSL_set_app_data(*ssl, secrets);
    if (sni)
        SSL_set_tlsext_host_name(*ssl, sni);
    SSL_set_connect_state(*ssl);
    return tls_step(fd, ssl, secrets);
}

void tls_free(SSL **ssl)
{
    if (*ssl) {
        SSL_free(*ssl);
        *ssl = NULL;
    }
}
