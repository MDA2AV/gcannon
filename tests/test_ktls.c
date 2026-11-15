/*
 * Unit tests for the kTLS key setup in ktls.c.
 *
 * Key derivation is checked against the TLS 1.3 traces in RFC 8448 (which
 * only use TLS_AES_128_GCM_SHA256) and, for the SHA-384 and ChaCha20 suites,
 * against vectors from an independent Python HKDF implementation that
 * reproduces the RFC 8448 values. A wrong key or IV wouldn't show up until a
 * server rejected every record, so the derivation is pinned byte for byte.
 *
 * The install tests then push data through the kernel over loopback. Keying
 * both ends with the same code only proves the code agrees with itself, so
 * one test keys just the client and has the peer build and open records in
 * userspace per RFC 8446; that is what catches a key or IV landing in the
 * wrong field of the kernel's crypto_info. These tests need kernel TLS (the
 * tls module) and are skipped without it.
 *
 * Build:  make test-unit
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <openssl/evp.h>

#include "ktls.h"

/* ── harness ───────────────────────────────────────────────────────── */

static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("    FAIL  ");                                   \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

static void begin(const char *name)
{
    printf("  %s\n", name);
}

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
}

/* ── vectors ───────────────────────────────────────────────────────── */

/* RFC 8448 section 3, "derive write traffic keys for application data" */
static const uint8_t rfc_client_secret[32] = {
    0x9e, 0x40, 0x64, 0x6c, 0xe7, 0x9a, 0x7f, 0x9d, 0xc0, 0x5a, 0xf8, 0x88,
    0x9b, 0xce, 0x65, 0x52, 0x87, 0x5a, 0xfa, 0x0b, 0x06, 0xdf, 0x00, 0x87,
    0xf7, 0x92, 0xeb, 0xb7, 0xc1, 0x75, 0x04, 0xa5 };
static const uint8_t rfc_client_key[16] = {
    0x17, 0x42, 0x2d, 0xda, 0x59, 0x6e, 0xd5, 0xd9, 0xac, 0xd8, 0x90, 0xe3,
    0xc6, 0x3f, 0x50, 0x51 };
static const uint8_t rfc_client_iv[12] = {
    0x5b, 0x78, 0x92, 0x3d, 0xee, 0x08, 0x57, 0x90, 0x33, 0xe5, 0x23, 0xd9 };

static const uint8_t rfc_server_secret[32] = {
    0xa1, 0x1a, 0xf9, 0xf0, 0x55, 0x31, 0xf8, 0x56, 0xad, 0x47, 0x11, 0x6b,
    0x45, 0xa9, 0x50, 0x32, 0x82, 0x04, 0xb4, 0xf4, 0x4b, 0xfb, 0x6b, 0x3a,
    0x4b, 0x4f, 0x1f, 0x3f, 0xcb, 0x63, 0x16, 0x43 };
static const uint8_t rfc_server_key[16] = {
    0x9f, 0x02, 0x28, 0x3b, 0x6c, 0x9c, 0x07, 0xef, 0xc2, 0x6b, 0xb9, 0xf2,
    0xac, 0x92, 0xe3, 0x56 };
static const uint8_t rfc_server_iv[12] = {
    0xcf, 0x78, 0x2b, 0x88, 0xdd, 0x83, 0x54, 0x9a, 0xad, 0xf1, 0xe9, 0x84 };

/* Python reference: secret = bytes(range(0x40, 0x70)), SHA-384 */
static const uint8_t sha384_key[32] = {
    0x32, 0xf5, 0x84, 0x23, 0x0a, 0xd7, 0xbd, 0x6e, 0xa0, 0x25, 0xe6, 0xe6,
    0xcc, 0x16, 0xe7, 0x57, 0x35, 0x8c, 0x96, 0x2e, 0xbc, 0x81, 0x96, 0xf5,
    0x43, 0x3a, 0xca, 0xe9, 0x83, 0xfb, 0x66, 0x8b };
static const uint8_t sha384_iv[12] = {
    0x0e, 0x7c, 0x7d, 0x30, 0x27, 0x2b, 0xc7, 0x4a, 0x0c, 0x98, 0x33, 0xb2 };

/* Python reference: secret = bytes(range(0x80, 0xa0)), SHA-256, 32-byte key */
static const uint8_t chacha_key[32] = {
    0x5c, 0x96, 0x66, 0x1b, 0x96, 0xc4, 0x2c, 0x1b, 0x52, 0x89, 0xd3, 0xc3,
    0xb2, 0x1a, 0x19, 0xa6, 0xb8, 0x72, 0xf1, 0x1d, 0x08, 0x36, 0x3b, 0x5e,
    0x93, 0xd1, 0x33, 0x73, 0x84, 0x1c, 0x8e, 0x7b };
static const uint8_t chacha_iv[12] = {
    0x6c, 0x8a, 0x8f, 0xa6, 0x34, 0x2c, 0x9d, 0x26, 0x3c, 0x73, 0x1f, 0x9c };

static void fill_seq(uint8_t *b, size_t n, uint8_t start)
{
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)(start + i);
}

/* ── suite names ───────────────────────────────────────────────────── */

static void test_suite_names(void)
{
    begin("suite names: the three TLS 1.3 suites map, everything else doesn't");
    CHECK(ktls_suite_from_name("TLS_AES_128_GCM_SHA256") == KTLS_AES_128_GCM_SHA256,
          "TLS_AES_128_GCM_SHA256");
    CHECK(ktls_suite_from_name("TLS_AES_256_GCM_SHA384") == KTLS_AES_256_GCM_SHA384,
          "TLS_AES_256_GCM_SHA384");
    CHECK(ktls_suite_from_name("TLS_CHACHA20_POLY1305_SHA256") == KTLS_CHACHA20_POLY1305_SHA256,
          "TLS_CHACHA20_POLY1305_SHA256");
    CHECK(ktls_suite_from_name("TLS_AES_128_CCM_SHA256") == KTLS_SUITE_NONE,
          "CCM must not map");
    CHECK(ktls_suite_from_name("ECDHE-RSA-AES128-GCM-SHA256") == KTLS_SUITE_NONE,
          "a TLS 1.2 suite must not map");
    CHECK(ktls_suite_from_name(NULL) == KTLS_SUITE_NONE, "NULL");
}

/* ── derivation ────────────────────────────────────────────────────── */

static void check_derive(const char *what, ktls_suite_t suite,
                         const uint8_t *secret, size_t secret_len,
                         const uint8_t *want_key, int want_key_len,
                         const uint8_t *want_iv)
{
    uint8_t key[32] = {0}, iv[12] = {0};
    const int n = ktls_derive(suite, secret, secret_len, key, iv);
    CHECK(n == want_key_len, "%s: key length %d, want %d", what, n, want_key_len);
    if (n == want_key_len) {
        char got[65], want[65];
        to_hex(key, (size_t)n, got);
        to_hex(want_key, (size_t)n, want);
        CHECK(memcmp(key, want_key, (size_t)n) == 0, "%s: key %s, want %s", what, got, want);
    }
    char got[25], want[25];
    to_hex(iv, 12, got);
    to_hex(want_iv, 12, want);
    CHECK(memcmp(iv, want_iv, 12) == 0, "%s: iv %s, want %s", what, got, want);
}

static void test_derive_rfc8448(void)
{
    begin("derive: RFC 8448 application traffic keys (AES-128-GCM-SHA256)");
    check_derive("client write", KTLS_AES_128_GCM_SHA256,
                 rfc_client_secret, sizeof(rfc_client_secret),
                 rfc_client_key, 16, rfc_client_iv);
    check_derive("server write", KTLS_AES_128_GCM_SHA256,
                 rfc_server_secret, sizeof(rfc_server_secret),
                 rfc_server_key, 16, rfc_server_iv);
}

static void test_derive_other_suites(void)
{
    begin("derive: AES-256-GCM-SHA384 and ChaCha20-Poly1305 keys");
    uint8_t s384[48], s256[32];
    fill_seq(s384, sizeof(s384), 0x40);
    fill_seq(s256, sizeof(s256), 0x80);
    check_derive("AES-256-GCM", KTLS_AES_256_GCM_SHA384, s384, sizeof(s384),
                 sha384_key, 32, sha384_iv);
    check_derive("ChaCha20", KTLS_CHACHA20_POLY1305_SHA256, s256, sizeof(s256),
                 chacha_key, 32, chacha_iv);
}

static void test_derive_rejects_bad_input(void)
{
    begin("derive: a secret of the wrong size or an unknown suite is refused");
    uint8_t key[32], iv[12], secret[48] = {0};
    CHECK(ktls_derive(KTLS_AES_256_GCM_SHA384, secret, 32, key, iv) == -1,
          "SHA-384 suite with a 32-byte secret");
    CHECK(ktls_derive(KTLS_AES_128_GCM_SHA256, secret, 48, key, iv) == -1,
          "SHA-256 suite with a 48-byte secret");
    CHECK(ktls_derive(KTLS_SUITE_NONE, secret, 32, key, iv) == -1,
          "KTLS_SUITE_NONE");
}

/* ── key log parsing ───────────────────────────────────────────────── */

static void test_keylog_lines(void)
{
    begin("keylog: traffic secrets are captured, other lines ignored");
    static const char random_hex[] =
        "6c1e4ea4a2c3d0e7f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8";
    char hex[2 * KTLS_SECRET_MAX + 1], line[256];
    ktls_secrets_t s;
    memset(&s, 0, sizeof(s));

    to_hex(rfc_client_secret, 32, hex);
    snprintf(line, sizeof(line), "CLIENT_TRAFFIC_SECRET_0 %s %s", random_hex, hex);
    CHECK(ktls_keylog_line(&s, line) == 1, "client line not stored");
    CHECK(s.client_len == 32 && memcmp(s.client, rfc_client_secret, 32) == 0,
          "client secret mangled (len %d)", s.client_len);

    uint8_t s384[48];
    fill_seq(s384, sizeof(s384), 0x40);
    to_hex(s384, 48, hex);
    snprintf(line, sizeof(line), "SERVER_TRAFFIC_SECRET_0 %s %s", random_hex, hex);
    CHECK(ktls_keylog_line(&s, line) == 1, "48-byte server line not stored");
    CHECK(s.server_len == 48 && memcmp(s.server, s384, 48) == 0,
          "server secret mangled (len %d)", s.server_len);

    /* Uppercase hex is still hex */
    to_hex(rfc_server_secret, 32, hex);
    for (char *p = hex; *p; p++)
        if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 'a' + 'A');
    snprintf(line, sizeof(line), "SERVER_TRAFFIC_SECRET_0 %s %s", random_hex, hex);
    CHECK(ktls_keylog_line(&s, line) == 1 && s.server_len == 32 &&
          memcmp(s.server, rfc_server_secret, 32) == 0, "uppercase hex not accepted");

    /* Lines that must leave the stored secrets alone */
    const ktls_secrets_t before = s;
    to_hex(rfc_client_secret, 32, hex);
    const char *ignored[] = {
        "CLIENT_HANDSHAKE_TRAFFIC_SECRET 00 11",
        "SERVER_HANDSHAKE_TRAFFIC_SECRET 00 11",
        "EXPORTER_SECRET 00 11",
        "CLIENT_TRAFFIC_SECRET_1 00 11",
        "CLIENT_TRAFFIC_SECRET_0",
        "CLIENT_TRAFFIC_SECRET_0 nospace",
        "",
    };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
        CHECK(ktls_keylog_line(&s, ignored[i]) == 0, "line accepted: \"%s\"", ignored[i]);

    char bad[256];
    snprintf(bad, sizeof(bad), "CLIENT_TRAFFIC_SECRET_0 %s %.62s", random_hex, hex);
    CHECK(ktls_keylog_line(&s, bad) == 0, "31-byte secret accepted");
    snprintf(bad, sizeof(bad), "CLIENT_TRAFFIC_SECRET_0 %s %.63s", random_hex, hex);
    CHECK(ktls_keylog_line(&s, bad) == 0, "odd-length hex accepted");
    snprintf(bad, sizeof(bad), "CLIENT_TRAFFIC_SECRET_0 %s %sff", random_hex, hex);
    CHECK(ktls_keylog_line(&s, bad) == 0, "33-byte secret accepted");
    snprintf(bad, sizeof(bad), "CLIENT_TRAFFIC_SECRET_0 %s %.30szz%s", random_hex, hex, hex + 32);
    CHECK(ktls_keylog_line(&s, bad) == 0, "non-hex secret accepted");

    CHECK(memcmp(&s, &before, sizeof(s)) == 0, "a rejected line changed the stored secrets");
}

/* ── install (needs kernel TLS) ────────────────────────────────────── */

static int loopback_pair(int *cfd, int *sfd)
{
    struct sockaddr_in sa = { .sin_family = AF_INET,
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(sa);
    const int lfd = socket(AF_INET, SOCK_STREAM, 0);
    *cfd = *sfd = -1;
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(lfd, 1) < 0 || getsockname(lfd, (struct sockaddr *)&sa, &len) < 0)
        goto fail;
    *cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (*cfd < 0 || connect(*cfd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        goto fail;
    *sfd = accept(lfd, NULL, NULL);
    if (*sfd < 0)
        goto fail;
    close(lfd);

    /* Never hang the suite on a recv that will not complete */
    const struct timeval tv = { .tv_sec = 2 };
    setsockopt(*cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(*sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return 0;
fail:
    if (*cfd >= 0) close(*cfd);
    if (lfd >= 0) close(lfd);
    return -1;
}

static int send_recv(int from, int to, const char *msg, char *got, size_t got_sz)
{
    const size_t len = strlen(msg);
    if (send(from, msg, len, MSG_NOSIGNAL) != (ssize_t)len) return -errno;
    const ssize_t r = recv(to, got, got_sz - 1, 0);
    if (r < 0) return -errno;
    got[r] = '\0';
    return (int)r;
}

static void test_install_roundtrip(void)
{
    begin("install: plaintext crosses a loopback connection keyed on both ends");
    const struct { ktls_suite_t suite; const char *name; size_t len; } cases[] = {
        { KTLS_AES_128_GCM_SHA256,       "AES-128-GCM", 32 },
        { KTLS_AES_256_GCM_SHA384,       "AES-256-GCM", 48 },
        { KTLS_CHACHA20_POLY1305_SHA256, "ChaCha20",    32 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t client[48], server[48];
        fill_seq(client, sizeof(client), 0x11);
        fill_seq(server, sizeof(server), 0xa7);

        int c, s;
        if (loopback_pair(&c, &s) < 0) {
            CHECK(0, "%s: loopback connection: %s", cases[i].name, strerror(errno));
            continue;
        }
        int rc = ktls_install(c, cases[i].suite, client, server, cases[i].len);
        CHECK(rc == 0, "%s: client install: %s", cases[i].name, strerror(-rc));
        rc = ktls_install(s, cases[i].suite, server, client, cases[i].len);
        CHECK(rc == 0, "%s: server install: %s", cases[i].name, strerror(-rc));

        char got[64];
        rc = send_recv(c, s, "ping", got, sizeof(got));
        CHECK(rc == 4 && strcmp(got, "ping") == 0, "%s: client to server: %s",
              cases[i].name, rc < 0 ? strerror(-rc) : got);
        rc = send_recv(s, c, "pong", got, sizeof(got));
        CHECK(rc == 4 && strcmp(got, "pong") == 0, "%s: server to client: %s",
              cases[i].name, rc < 0 ? strerror(-rc) : got);
        close(c);
        close(s);
    }
}

/* One-shot AEAD over a TLS 1.3 record body, done in userspace with OpenSSL so
 * it checks the kernel against RFC 8446 rather than against itself. */
static int aead(int enc, ktls_suite_t suite, const uint8_t *key, const uint8_t *nonce,
                const uint8_t *aad, int aad_len, const uint8_t *in, int in_len,
                uint8_t *out, uint8_t tag[16])
{
    const EVP_CIPHER *cipher = suite == KTLS_AES_128_GCM_SHA256 ? EVP_aes_128_gcm()
                             : suite == KTLS_AES_256_GCM_SHA384 ? EVP_aes_256_gcm()
                             : EVP_chacha20_poly1305();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0;
    const int ok = ctx
        && EVP_CipherInit_ex(ctx, cipher, NULL, NULL, NULL, enc)
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL)
        && EVP_CipherInit_ex(ctx, NULL, NULL, key, nonce, enc)
        && (enc || EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, tag))
        && EVP_CipherUpdate(ctx, NULL, &n, aad, aad_len)
        && EVP_CipherUpdate(ctx, out, &n, in, in_len)
        && EVP_CipherFinal_ex(ctx, out + n, &n)
        && (!enc || EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag));
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -1;
}

static void test_install_records_match_rfc8446(void)
{
    begin("install: kernel records match RFC 8446 framing done in userspace");
    /* Only the client socket gets kTLS; the peer reads and writes raw records.
       The first record each way has sequence number 0, so its nonce is the
       derived IV itself. */
    uint8_t s384[48], s256[32];
    fill_seq(s384, sizeof(s384), 0x40);
    fill_seq(s256, sizeof(s256), 0x80);
    const struct {
        ktls_suite_t suite; const char *name;
        const uint8_t *secret; size_t len; const uint8_t *key; const uint8_t *iv;
    } cases[] = {
        { KTLS_AES_128_GCM_SHA256,       "AES-128-GCM", rfc_client_secret, 32, rfc_client_key, rfc_client_iv },
        { KTLS_AES_256_GCM_SHA384,       "AES-256-GCM", s384, 48, sha384_key, sha384_iv },
        { KTLS_CHACHA20_POLY1305_SHA256, "ChaCha20",    s256, 32, chacha_key, chacha_iv },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int c, s;
        if (loopback_pair(&c, &s) < 0) {
            CHECK(0, "%s: loopback connection: %s", cases[i].name, strerror(errno));
            continue;
        }
        const int rc = ktls_install(c, cases[i].suite, cases[i].secret, cases[i].secret,
                                    cases[i].len);
        CHECK(rc == 0, "%s: install: %s", cases[i].name, strerror(-rc));

        /* kernel -> userspace: header, "ping" + inner type, 16-byte tag */
        uint8_t rec[64], plain[64];
        send(c, "ping", 4, MSG_NOSIGNAL);
        const ssize_t got = recv(s, rec, 5 + 4 + 1 + 16, MSG_WAITALL);
        CHECK(got == 26 && rec[0] == 0x17 && rec[1] == 0x03 && rec[2] == 0x03 &&
              rec[3] == 0 && rec[4] == 21,
              "%s: record header %02x %02x %02x %02x %02x (%zd bytes)", cases[i].name,
              rec[0], rec[1], rec[2], rec[3], rec[4], got);
        if (got == 26) {
            const int dec = aead(0, cases[i].suite, cases[i].key, cases[i].iv,
                                 rec, 5, rec + 5, 5, plain, rec + 10);
            CHECK(dec == 0 && memcmp(plain, "ping\x17", 5) == 0,
                  "%s: kernel's record doesn't decrypt with the RFC 8446 key and nonce",
                  cases[i].name);
        }

        /* userspace -> kernel */
        const uint8_t hdr[5] = { 0x17, 0x03, 0x03, 0x00, 21 };
        memcpy(rec, hdr, 5);
        aead(1, cases[i].suite, cases[i].key, cases[i].iv, hdr, 5,
             (const uint8_t *)"pong\x17", 5, rec + 5, rec + 10);
        send(s, rec, 26, MSG_NOSIGNAL);
        char text[64];
        const ssize_t r = recv(c, text, sizeof(text) - 1, 0);
        if (r >= 0) text[r] = '\0';
        CHECK(r == 4 && memcmp(text, "pong", 4) == 0,
              "%s: kernel didn't accept an RFC 8446 record: %s", cases[i].name,
              r < 0 ? strerror(errno) : text);
        close(c);
        close(s);
    }
}

static void test_install_mismatched_keys(void)
{
    begin("install: a receiver keyed with the wrong secret can't read the record");
    uint8_t right[32], wrong[32];
    fill_seq(right, sizeof(right), 0x01);
    fill_seq(wrong, sizeof(wrong), 0x02);

    int c, s;
    if (loopback_pair(&c, &s) < 0) {
        CHECK(0, "loopback connection: %s", strerror(errno));
        return;
    }
    CHECK(ktls_install(c, KTLS_AES_128_GCM_SHA256, right, right, 32) == 0, "client install");
    CHECK(ktls_install(s, KTLS_AES_128_GCM_SHA256, wrong, wrong, 32) == 0, "server install");
    char got[64];
    const int rc = send_recv(c, s, "ping", got, sizeof(got));
    CHECK(rc == -EBADMSG, "recv returned %s, want EBADMSG (were records encrypted at all?)",
          rc < 0 ? strerror(-rc) : got);
    close(c);
    close(s);

    begin("install: an unknown suite is refused");
    if (loopback_pair(&c, &s) < 0) {
        CHECK(0, "loopback connection: %s", strerror(errno));
        return;
    }
    CHECK(ktls_install(c, KTLS_SUITE_NONE, right, right, 32) == -EINVAL,
          "KTLS_SUITE_NONE was not refused");
    close(c);
    close(s);
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void)
{
    printf("\ngcannon kTLS key tests\n\n");

    test_suite_names();
    test_derive_rfc8448();
    test_derive_other_suites();
    test_derive_rejects_bad_input();
    test_keylog_lines();

    const int probe = ktls_probe();
    if (probe == 0) {
        test_install_roundtrip();
        test_install_records_match_rfc8446();
        test_install_mismatched_keys();
    } else {
        printf("  install tests\n    SKIP  kernel TLS unavailable here (%s)\n",
               strerror(-probe));
    }

    printf("\n%d checks, %d failed\n\n", checks, failures);
    return failures ? 1 : 0;
}
