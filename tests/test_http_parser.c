/*
 * Unit tests for http_parse_responses().
 *
 * The interesting cases here are recv-boundary splits. gcannon feeds the
 * parser whatever a single multishot recv happened to deliver, so every byte
 * offset in a response stream is a legal place for one call to end and the
 * next to begin. A parser that only works when a response arrives whole is a
 * parser that works until the server flushes differently.
 *
 * Build:  make test-unit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"

/* ── harness ───────────────────────────────────────────────────────── */

static int checks = 0;
static int failures = 0;
static const char *current_test = "";

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
    current_test = name;
    printf("  %s\n", name);
}

/* Render CR/LF visibly so failure messages show where a split landed. */
static const char *esc(const char *s, int len, char *out, int out_sz)
{
    int o = 0;
    for (int i = 0; i < len && o < out_sz - 6; i++) {
        const unsigned char c = (unsigned char)s[i];
        if      (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c >= 32 && c < 127) out[o++] = (char)c;
        else o += snprintf(out + o, out_sz - o, "\\x%02x", c);
    }
    out[o] = '\0';
    return out;
}

/*
 * Feed `s` to `p` in pieces, cutting at each offset in `cuts`. Returns the
 * total number of completed responses; sets *out_err if any call reported an
 * unrecoverable parse failure (which is what makes the worker reconnect).
 */
static int feed_pieces(http_parser_t *p, const char *s, int len,
                       const int *cuts, int ncuts, int *out_err)
{
    int total = 0, off = 0;
    *out_err = 0;

    for (int i = 0; i <= ncuts; i++) {
        const int end = (i < ncuts) ? cuts[i] : len;
        if (end > off) {
            const int r = http_parse_responses(p, (const uint8_t *)s + off, end - off);
            if (r < 0) { *out_err = 1; return total; }
            total += r;
        }
        off = end;
    }
    return total;
}

/* Same, but in fixed-size pieces — models a recv buffer of that size. */
static int feed_fixed(http_parser_t *p, const char *s, int len,
                      int piece, int *out_err)
{
    int total = 0;
    *out_err = 0;

    for (int off = 0; off < len; off += piece) {
        const int n = (len - off < piece) ? len - off : piece;
        const int r = http_parse_responses(p, (const uint8_t *)s + off, n);
        if (r < 0) { *out_err = 1; return total; }
        total += r;
    }
    return total;
}

/*
 * After a response has been fully consumed the parser must be back at its
 * starting state. Leftover bytes in header_buf are the root cause of the
 * chunked bug: they are invisible until the *next* response is parsed on top
 * of them, at which point picohttpparser rejects the whole thing.
 */
static int parser_is_pristine(const http_parser_t *p)
{
    return p->state == 0 &&
           p->header_buf_len == 0 &&
           p->chunk_state == 0 &&
           p->chunk_line_len == 0 &&
           p->crlf_seen == 0 &&
           p->parse_error == 0;
}

/* ── response builders ─────────────────────────────────────────────── */

static int build_cl(char *out, int status, const char *body)
{
    return sprintf(out, "HTTP/1.1 %d OK\r\nContent-Length: %zu\r\n\r\n%s",
                   status, strlen(body), body);
}

/* One data chunk, then the terminating "0\r\n\r\n". */
static int build_chunked(char *out, int status, const char *body)
{
    return sprintf(out,
        "HTTP/1.1 %d OK\r\nTransfer-Encoding: chunked\r\n\r\n%zx\r\n%s\r\n0\r\n\r\n",
        status, strlen(body), body);
}

/* Several data chunks, to exercise the chunk_state 0/1/2 transitions. */
static int build_chunked_multi(char *out, int status,
                               const char **parts, int nparts)
{
    int n = sprintf(out, "HTTP/1.1 %d OK\r\nTransfer-Encoding: chunked\r\n\r\n", status);
    for (int i = 0; i < nparts; i++)
        n += sprintf(out + n, "%zx\r\n%s\r\n", strlen(parts[i]), parts[i]);
    n += sprintf(out + n, "0\r\n\r\n");
    return n;
}

/* ── tests: content-length ─────────────────────────────────────────── */

static void test_cl_whole(void)
{
    begin("content-length: single response, delivered whole");

    char r[256];
    const int len = build_cl(r, 200, "hello");

    http_parser_t p;
    http_parser_reset(&p);
    int err = 0;
    const int got = feed_pieces(&p, r, len, NULL, 0, &err);

    CHECK(!err, "parser reported an unrecoverable error");
    CHECK(got == 1, "got %d completed responses, want 1", got);
    CHECK(p.cls_2xx == 1, "cls_2xx = %d, want 1", p.cls_2xx);
    CHECK(parser_is_pristine(&p),
          "parser not pristine after a complete response "
          "(state=%d header_buf_len=%d)", p.state, p.header_buf_len);
}

static void test_cl_split_sweep(void)
{
    begin("content-length: split at every byte offset");

    char r[256];
    const int len = build_cl(r, 200, "hello world");

    for (int cut = 1; cut < len; cut++) {
        http_parser_t p;
        http_parser_reset(&p);
        int err = 0;
        const int got = feed_pieces(&p, r, len, &cut, 1, &err);

        char a[128], b[128];
        CHECK(!err && got == 1 && parser_is_pristine(&p),
              "split at byte %d: got %d resp%s%s | piece1 tail=\"%s\" piece2 head=\"%s\"",
              cut, got, err ? ", PARSE ERROR" : "",
              parser_is_pristine(&p) ? "" : ", residue left in parser",
              esc(r + (cut > 8 ? cut - 8 : 0), cut > 8 ? 8 : cut, a, sizeof(a)),
              esc(r + cut, (len - cut) > 8 ? 8 : len - cut, b, sizeof(b)));
    }
}

/* ── tests: chunked ────────────────────────────────────────────────── */

static void test_chunked_whole(void)
{
    begin("chunked: single response, delivered whole");

    char r[256];
    const int len = build_chunked(r, 200, "hello");

    http_parser_t p;
    http_parser_reset(&p);
    int err = 0;
    const int got = feed_pieces(&p, r, len, NULL, 0, &err);

    CHECK(!err, "parser reported an unrecoverable error");
    CHECK(got == 1, "got %d completed responses, want 1", got);
    CHECK(p.cls_2xx == 1, "cls_2xx = %d, want 1", p.cls_2xx);
    CHECK(parser_is_pristine(&p),
          "parser not pristine after a complete response "
          "(state=%d header_buf_len=%d)", p.state, p.header_buf_len);
}

/*
 * Root-cause test. A complete chunked response must leave the parser clean no
 * matter where the recv boundary fell — including inside the terminating CRLF
 * that follows the final "0\r\n".
 */
static void test_chunked_split_sweep_single(void)
{
    begin("chunked: split at every byte offset, parser must end clean");

    char r[256];
    const int len = build_chunked(r, 200, "hello");

    for (int cut = 1; cut < len; cut++) {
        http_parser_t p;
        http_parser_reset(&p);
        int err = 0;
        const int got = feed_pieces(&p, r, len, &cut, 1, &err);

        char a[128], b[128];
        CHECK(!err && got == 1 && parser_is_pristine(&p),
              "split at byte %d/%d: got %d resp%s%s | piece1 tail=\"%s\" piece2=\"%s\"",
              cut, len, got, err ? ", PARSE ERROR" : "",
              parser_is_pristine(&p) ? "" : ", RESIDUE LEFT IN PARSER",
              esc(r + (cut > 8 ? cut - 8 : 0), cut > 8 ? 8 : cut, a, sizeof(a)),
              esc(r + cut, len - cut, b, sizeof(b)));
    }
}

/*
 * Observable symptom. Two responses back to back on one connection, split at
 * every offset. Residue left behind by response N is prepended to response
 * N+1's headers, and picohttpparser only tolerates a leading empty line for
 * requests, not responses — so it rejects the stream.
 */
static void test_chunked_split_sweep_pair(void)
{
    begin("chunked: two responses on one connection, split at every offset");

    char one[256];
    const int one_len = build_chunked(one, 200, "hello");

    char stream[512];
    memcpy(stream, one, one_len);
    memcpy(stream + one_len, one, one_len);
    const int len = one_len * 2;

    for (int cut = 1; cut < len; cut++) {
        http_parser_t p;
        http_parser_reset(&p);
        int err = 0;
        const int got = feed_pieces(&p, stream, len, &cut, 1, &err);

        char a[128], b[128];
        CHECK(!err && got == 2,
              "split at byte %d/%d: got %d resp%s | piece1 tail=\"%s\" piece2 head=\"%s\"",
              cut, len, got, err ? ", PARSE ERROR (worker would reconnect)" : "",
              esc(stream + (cut > 10 ? cut - 10 : 0), cut > 10 ? 10 : cut, a, sizeof(a)),
              esc(stream + cut, (len - cut) > 18 ? 18 : len - cut, b, sizeof(b)));
    }
}

/*
 * The exact shape the integration test produces: a server that writes the body
 * and the terminating CRLF in two separate flushes, so the CRLF lands in its
 * own recv. This is what nginx/Go/many frameworks do under some buffering
 * configurations, and what tests/chunked_server.py reproduces deliberately.
 */
static void test_chunked_terminator_flushed_separately(void)
{
    begin("chunked: terminating CRLF arrives in its own recv");

    char one[256];
    const int one_len = build_chunked(one, 200, "hello");

    /* Stream of 3 responses; cut 2 bytes before the end of each. */
    char stream[768];
    int len = 0;
    for (int i = 0; i < 3; i++) { memcpy(stream + len, one, one_len); len += one_len; }

    /* Cut 2 bytes before the end of each response, so every terminating CRLF
       lands at the head of the following recv. */
    const int bounds[4] = { one_len - 2, 2 * one_len - 2, 3 * one_len - 2, len };

    /* Accumulated the way worker.c does it: cls_* are per-call tallies, so
       they are summed after each call rather than read once at the end. */
    http_parser_t p;
    http_parser_reset(&p);
    int got = 0, ok2 = 0, err = 0, off = 0;

    for (int i = 0; i < 4; i++) {
        const int r = http_parse_responses(&p, (const uint8_t *)stream + off,
                                           bounds[i] - off);
        if (r < 0) { err = 1; break; }
        got += r;
        ok2 += p.cls_2xx;
        off = bounds[i];
    }

    CHECK(!err, "parser reported an unrecoverable error — the worker would "
                "count a read error and reconnect on every response");
    CHECK(got == 3, "got %d completed responses, want 3", got);
    CHECK(ok2 == 3, "summed cls_2xx = %d, want 3", ok2);
}

static void test_chunked_multi_chunk_split_sweep(void)
{
    begin("chunked: multi-chunk body, split at every byte offset");

    const char *parts[] = { "abc", "defgh", "i" };
    char r[512];
    const int len = build_chunked_multi(r, 200, parts, 3);

    for (int cut = 1; cut < len; cut++) {
        http_parser_t p;
        http_parser_reset(&p);
        int err = 0;
        const int got = feed_pieces(&p, r, len, &cut, 1, &err);

        char a[128];
        CHECK(!err && got == 1 && parser_is_pristine(&p),
              "split at byte %d/%d: got %d resp%s%s | piece1 tail=\"%s\"",
              cut, len, got, err ? ", PARSE ERROR" : "",
              parser_is_pristine(&p) ? "" : ", RESIDUE LEFT IN PARSER",
              esc(r + (cut > 10 ? cut - 10 : 0), cut > 10 ? 10 : cut, a, sizeof(a)));
    }
}

/*
 * A separate defect from the terminator bug, in the same state machine: the
 * CRLF that follows each *data* chunk. chunk_state 2 skips two bytes
 * unconditionally as soon as two are available, without remembering how much
 * of that CRLF an earlier recv already consumed. Split the CRLF and it eats
 * the '\n' plus the first byte of the next chunk-size line, desynchronising
 * the body framing entirely.
 */
static void test_chunked_interior_crlf_split(void)
{
    begin("chunked: recv boundary inside a chunk's trailing CRLF");

    const char *parts[] = { "abc", "de" };
    char r[512];
    const int len = build_chunked_multi(r, 200, parts, 2);

    /* Cut between the '\r' and '\n' that terminate the first data chunk. */
    const int hdr = (int)strlen("HTTP/1.1 200 OK\r\n"
                                "Transfer-Encoding: chunked\r\n\r\n");
    const int cut = hdr + 3 /* "3\r\n" */ + 3 /* "abc" */ + 1 /* the '\r' */;

    http_parser_t p;
    http_parser_reset(&p);
    int err = 0;
    const int got = feed_pieces(&p, r, len, &cut, 1, &err);

    char a[128], b[128];
    CHECK(!err && got == 1 && parser_is_pristine(&p),
          "got %d resp%s%s | piece1 tail=\"%s\" piece2 head=\"%s\"",
          got, err ? ", PARSE ERROR" : "",
          parser_is_pristine(&p) ? "" : ", RESIDUE LEFT IN PARSER",
          esc(r + cut - 10, 10, a, sizeof(a)),
          esc(r + cut, (len - cut) > 12 ? 12 : len - cut, b, sizeof(b)));
}

/*
 * The realistic form of both bugs. gcannon's default --recv-buf is 4096, so
 * any chunked body bigger than that arrives in several recvs whose boundaries
 * have no relationship to chunk framing. Sweeping the piece size stands in for
 * "whatever the kernel happened to hand us this time".
 */
static void test_chunked_large_body_arbitrary_recv_sizes(void)
{
    begin("chunked: large body fed at every recv size (default --recv-buf is 4096)");

    enum { CHUNK_SZ = 1500, NCHUNKS = 12 };

    char *body = malloc(CHUNK_SZ + 1);
    memset(body, 'x', CHUNK_SZ);
    body[CHUNK_SZ] = '\0';

    const char *parts[NCHUNKS];
    for (int i = 0; i < NCHUNKS; i++) parts[i] = body;

    char *r = malloc((size_t)(CHUNK_SZ + 16) * NCHUNKS + 256);
    const int len = build_chunked_multi(r, 200, parts, NCHUNKS);

    static const int sizes[] = {
          1,    2,    3,    5,    7,   13,   17,   64,  128,  256,
        512, 1000, 1024, 1500, 2048, 4000, 4096, 8192,
    };
    const int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    int bad = 0;
    char examples[256] = "";
    for (int i = 0; i < nsizes; i++) {
        http_parser_t p;
        http_parser_reset(&p);
        int err = 0;
        const int got = feed_fixed(&p, r, len, sizes[i], &err);

        if (err || got != 1 || !parser_is_pristine(&p)) {
            if (bad < 4)
                snprintf(examples + strlen(examples),
                         sizeof(examples) - strlen(examples),
                         "%s%d(%s)", bad ? ", " : "", sizes[i],
                         err ? "parse error" : (got != 1 ? "wrong count" : "residue"));
            bad++;
        }
    }

    CHECK(bad == 0,
          "%d/%d recv sizes mis-parsed a %d-byte chunked response: %s%s",
          bad, nsizes, len, examples, bad > 4 ? ", ..." : "");

    free(r);
    free(body);
}

/*
 * RFC 7230 allows trailer headers between the last chunk and the final empty
 * line. Consuming the trailer section is what makes the end of a chunked body
 * unambiguous, so it gets the same split sweep as everything else.
 */
static void test_chunked_with_trailers_split_sweep(void)
{
    begin("chunked: trailer headers after the last chunk, split at every offset");

    const char *r =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n"
        "0\r\n"
        "X-Checksum: deadbeef\r\n"
        "X-Elapsed: 5\r\n"
        "\r\n";
    const int one_len = (int)strlen(r);

    char stream[1024];
    memcpy(stream, r, one_len);
    memcpy(stream + one_len, r, one_len);
    const int len = one_len * 2;

    for (int cut = 1; cut < len; cut++) {
        http_parser_t p;
        http_parser_reset(&p);
        int err = 0;
        const int got = feed_pieces(&p, stream, len, &cut, 1, &err);

        char a[128];
        CHECK(!err && got == 2,
              "split at byte %d/%d: got %d resp%s | piece1 tail=\"%s\"",
              cut, len, got, err ? ", PARSE ERROR" : "",
              esc(stream + (cut > 12 ? cut - 12 : 0), cut > 12 ? 12 : cut,
                  a, sizeof(a)));
    }
}

/* ── tests: pipelining and status accounting ───────────────────────── */

static void test_pipelined_batch(void)
{
    begin("pipelining: many responses in one recv");

    char one[256];
    const int one_len = build_cl(one, 200, "hi");

    char *stream = malloc((size_t)one_len * 300);
    for (int i = 0; i < 300; i++)
        memcpy(stream + (size_t)i * one_len, one, one_len);

    http_parser_t p;
    http_parser_reset(&p);
    const int got = http_parse_responses(&p, (const uint8_t *)stream, one_len * 300);

    CHECK(got == 300, "got %d completed responses, want 300", got);
    /* completed_statuses[] is capped at 256; the cls_* tallies are not, which
       is why the worker adds those in bulk rather than walking the array. */
    CHECK(p.cls_2xx == 300,
          "cls_2xx = %d, want 300 — status accounting drops responses past "
          "the completed_statuses[] cap", p.cls_2xx);
    CHECK(p.completed_count == 256,
          "completed_count = %d, want 256 (documented cap)", p.completed_count);

    free(stream);
}

static void test_status_classification(void)
{
    begin("status classes: 2xx/3xx/4xx/5xx/other tallied per call");

    char stream[1024];
    int len = 0;
    len += build_cl(stream + len, 200, "a");
    len += build_cl(stream + len, 301, "b");
    len += build_cl(stream + len, 404, "c");
    len += build_cl(stream + len, 503, "d");
    len += build_cl(stream + len, 101, "e");

    http_parser_t p;
    http_parser_reset(&p);
    const int got = http_parse_responses(&p, (const uint8_t *)stream, len);

    CHECK(got == 5, "got %d completed responses, want 5", got);
    CHECK(p.cls_2xx == 1, "cls_2xx = %d, want 1", p.cls_2xx);
    CHECK(p.cls_3xx == 1, "cls_3xx = %d, want 1", p.cls_3xx);
    CHECK(p.cls_4xx == 1, "cls_4xx = %d, want 1", p.cls_4xx);
    CHECK(p.cls_5xx == 1, "cls_5xx = %d, want 1", p.cls_5xx);
    CHECK(p.cls_other == 1, "cls_other = %d, want 1", p.cls_other);
}

static void test_class_tallies_reset_per_call(void)
{
    begin("status classes: tallies reset on every call, never accumulate");

    char r[256];
    const int len = build_cl(r, 200, "hello");

    http_parser_t p;
    http_parser_reset(&p);
    http_parse_responses(&p, (const uint8_t *)r, len);
    http_parse_responses(&p, (const uint8_t *)r, len);

    CHECK(p.cls_2xx == 1,
          "cls_2xx = %d after two calls, want 1 — stale tallies would be "
          "double-counted into worker stats", p.cls_2xx);
}

/* ── tests: unrecoverable failures ─────────────────────────────────── */

static void test_malformed_returns_error(void)
{
    begin("errors: malformed response status line is unrecoverable");

    const char *garbage = "NOTHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";

    http_parser_t p;
    http_parser_reset(&p);
    const int got = http_parse_responses(&p, (const uint8_t *)garbage,
                                         (int)strlen(garbage));

    CHECK(got == -1, "got %d, want -1 so the worker drops the connection", got);
}

static void test_oversized_headers_return_error(void)
{
    begin("errors: headers larger than header_buf are unrecoverable");

    char big[4096];
    int n = sprintf(big, "HTTP/1.1 200 OK\r\nX-Huge: ");
    memset(big + n, 'A', 3000);
    n += 3000;
    n += sprintf(big + n, "\r\nContent-Length: 0\r\n\r\n");

    http_parser_t p;
    http_parser_reset(&p);
    const int got = http_parse_responses(&p, (const uint8_t *)big, n);

    CHECK(got == -1,
          "got %d, want -1 — without this the connection wedges silently, "
          "since more data can never complete a parse into a full buffer", got);
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void)
{
    printf("\ngcannon http parser tests\n\n");

    test_cl_whole();
    test_cl_split_sweep();

    test_chunked_whole();
    test_chunked_split_sweep_single();
    test_chunked_split_sweep_pair();
    test_chunked_terminator_flushed_separately();
    test_chunked_multi_chunk_split_sweep();
    test_chunked_interior_crlf_split();
    test_chunked_large_body_arbitrary_recv_sizes();
    test_chunked_with_trailers_split_sweep();

    test_pipelined_batch();
    test_status_classification();
    test_class_tallies_reset_per_call();

    test_malformed_returns_error();
    test_oversized_headers_return_error();

    printf("\n%d checks, %d failed\n\n", checks, failures);
    (void)current_test;
    return failures ? 1 : 0;
}
