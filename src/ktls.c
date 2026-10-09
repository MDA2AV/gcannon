#include "ktls.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <linux/tls.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#ifndef SOL_TLS
#define SOL_TLS 282
#endif
#ifndef TCP_ULP
#define TCP_ULP 31
#endif

#define CLIENT_LABEL "CLIENT_TRAFFIC_SECRET_0 "
#define SERVER_LABEL "SERVER_TRAFFIC_SECRET_0 "

ktls_suite_t ktls_suite_from_name(const char *name)
{
    if (!name) return KTLS_SUITE_NONE;
    if (strcmp(name, "TLS_AES_128_GCM_SHA256") == 0)       return KTLS_AES_128_GCM_SHA256;
    if (strcmp(name, "TLS_AES_256_GCM_SHA384") == 0)       return KTLS_AES_256_GCM_SHA384;
    if (strcmp(name, "TLS_CHACHA20_POLY1305_SHA256") == 0) return KTLS_CHACHA20_POLY1305_SHA256;
    return KTLS_SUITE_NONE;
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int ktls_keylog_line(ktls_secrets_t *s, const char *line)
{
    /* "<LABEL> <client random, hex> <secret, hex>" */
    uint8_t *dst, *dst_len;
    const char *rest;
    if (strncmp(line, CLIENT_LABEL, sizeof(CLIENT_LABEL) - 1) == 0) {
        dst = s->client;
        dst_len = &s->client_len;
        rest = line + sizeof(CLIENT_LABEL) - 1;
    } else if (strncmp(line, SERVER_LABEL, sizeof(SERVER_LABEL) - 1) == 0) {
        dst = s->server;
        dst_len = &s->server_len;
        rest = line + sizeof(SERVER_LABEL) - 1;
    } else {
        return 0;
    }

    const char *p = strchr(rest, ' ');
    if (!p) return 0;
    p++;

    uint8_t secret[KTLS_SECRET_MAX];
    size_t n = 0;
    while (p[0] && p[1] && n < sizeof(secret)) {
        const int hi = hex_digit(p[0]), lo = hex_digit(p[1]);
        if (hi < 0 || lo < 0) break;
        secret[n++] = (uint8_t)(hi << 4 | lo);
        p += 2;
    }
    /* Whole line consumed, and a SHA-256 or SHA-384 sized secret */
    const int ok = *p == '\0' && (n == 32 || n == 48);
    if (ok) {
        memcpy(dst, secret, n);
        *dst_len = (uint8_t)n;
    }
    OPENSSL_cleanse(secret, sizeof(secret));
    return ok;
}

/* HKDF-Expand-Label(secret, label, "", len) from RFC 8446 section 7.1. Every
 * output here is no longer than the hash, so HKDF-Expand is a single HMAC
 * block: T(1) = HMAC(secret, HkdfLabel || 0x01). */
static int expand_label(const EVP_MD *md, const uint8_t *secret, size_t secret_len,
                        const char *label, uint8_t *out, size_t len)
{
    uint8_t info[2 + 1 + 6 + 8 + 1 + 1];
    const size_t label_len = strlen(label);
    if (label_len > 8 || len > (size_t)EVP_MD_size(md)) return -1;

    size_t n = 0;
    info[n++] = (uint8_t)(len >> 8);
    info[n++] = (uint8_t)len;
    info[n++] = (uint8_t)(6 + label_len);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, label_len);
    n += label_len;
    info[n++] = 0;   /* empty context */
    info[n++] = 1;   /* HKDF-Expand block counter */

    uint8_t block[EVP_MAX_MD_SIZE];
    unsigned int block_len = 0;
    const int ok = HMAC(md, secret, (int)secret_len, info, n, block, &block_len) != NULL
                && block_len >= len;
    if (ok) memcpy(out, block, len);
    OPENSSL_cleanse(block, sizeof(block));
    return ok ? 0 : -1;
}

int ktls_derive(ktls_suite_t suite, const uint8_t *secret, size_t secret_len,
                uint8_t key[32], uint8_t iv[12])
{
    const EVP_MD *md;
    size_t key_len;
    switch (suite) {
    case KTLS_AES_128_GCM_SHA256:       md = EVP_sha256(); key_len = 16; break;
    case KTLS_AES_256_GCM_SHA384:       md = EVP_sha384(); key_len = 32; break;
    case KTLS_CHACHA20_POLY1305_SHA256: md = EVP_sha256(); key_len = 32; break;
    default: return -1;
    }
    if (secret_len != (size_t)EVP_MD_size(md)) return -1;

    if (expand_label(md, secret, secret_len, "key", key, key_len) < 0 ||
        expand_label(md, secret, secret_len, "iv",  iv,  12)      < 0)
        return -1;
    return (int)key_len;
}

/* Install one direction. For TLS 1.3 the kernel builds each record nonce from
 * salt || iv XORed with the record sequence number, so the 12-byte IV is split
 * across the two fields (ChaCha20-Poly1305 has no salt and takes it whole). */
static int install_dir(int fd, int dir, ktls_suite_t suite,
                       const uint8_t *secret, size_t secret_len)
{
    union {
        struct tls12_crypto_info_aes_gcm_128      aes128;
        struct tls12_crypto_info_aes_gcm_256      aes256;
        struct tls12_crypto_info_chacha20_poly1305 chacha;
    } ci;
    uint8_t key[32], iv[12];
    socklen_t len = 0;
    int rc = -EINVAL;

    memset(&ci, 0, sizeof(ci));   /* rec_seq starts at zero */
    if (ktls_derive(suite, secret, secret_len, key, iv) < 0) goto out;

    switch (suite) {
    case KTLS_AES_128_GCM_SHA256:
        ci.aes128.info.version     = TLS_1_3_VERSION;
        ci.aes128.info.cipher_type = TLS_CIPHER_AES_GCM_128;
        memcpy(ci.aes128.key,  key, sizeof(ci.aes128.key));
        memcpy(ci.aes128.salt, iv,  sizeof(ci.aes128.salt));
        memcpy(ci.aes128.iv,   iv + sizeof(ci.aes128.salt), sizeof(ci.aes128.iv));
        len = sizeof(ci.aes128);
        break;
    case KTLS_AES_256_GCM_SHA384:
        ci.aes256.info.version     = TLS_1_3_VERSION;
        ci.aes256.info.cipher_type = TLS_CIPHER_AES_GCM_256;
        memcpy(ci.aes256.key,  key, sizeof(ci.aes256.key));
        memcpy(ci.aes256.salt, iv,  sizeof(ci.aes256.salt));
        memcpy(ci.aes256.iv,   iv + sizeof(ci.aes256.salt), sizeof(ci.aes256.iv));
        len = sizeof(ci.aes256);
        break;
    case KTLS_CHACHA20_POLY1305_SHA256:
        ci.chacha.info.version     = TLS_1_3_VERSION;
        ci.chacha.info.cipher_type = TLS_CIPHER_CHACHA20_POLY1305;
        memcpy(ci.chacha.key, key, sizeof(ci.chacha.key));
        memcpy(ci.chacha.iv,  iv,  sizeof(ci.chacha.iv));
        len = sizeof(ci.chacha);
        break;
    default:
        goto out;
    }
    rc = setsockopt(fd, SOL_TLS, dir, &ci, len) < 0 ? -errno : 0;

out:
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(iv, sizeof(iv));
    OPENSSL_cleanse(&ci, sizeof(ci));
    return rc;
}

int ktls_install(int fd, ktls_suite_t suite,
                 const uint8_t *tx_secret, const uint8_t *rx_secret,
                 size_t secret_len)
{
    /* A TLS library may have attached the ULP already — OpenSSL's socket BIO
       does so whenever it's built with KTLS — so EEXIST means it's in place. */
    if (setsockopt(fd, IPPROTO_TCP, TCP_ULP, "tls", sizeof("tls")) < 0 && errno != EEXIST)
        return -errno;

    const int rc = install_dir(fd, TLS_TX, suite, tx_secret, secret_len);
    if (rc < 0) return rc;
    return install_dir(fd, TLS_RX, suite, rx_secret, secret_len);
}

int ktls_probe(void)
{
    struct sockaddr_in sa = { .sin_family = AF_INET,
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t sa_len = sizeof(sa);
    int rc, cfd = -1;

    const int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(lfd, 1) < 0 || getsockname(lfd, (struct sockaddr *)&sa, &sa_len) < 0) {
        rc = -errno;
        goto out;
    }
    /* The ULP only attaches to an established socket; the kernel completes
       this connect without anyone calling accept(). */
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0 || connect(cfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        rc = -errno;
        goto out;
    }
    rc = setsockopt(cfd, IPPROTO_TCP, TCP_ULP, "tls", sizeof("tls")) < 0 ? -errno : 0;

out:
    if (cfd >= 0) close(cfd);
    if (lfd >= 0) close(lfd);
    return rc;
}
