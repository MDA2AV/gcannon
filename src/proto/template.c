#include "proto/template.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "core/constants.h"

/* ── GET from the URL ──────────────────────────────────────────────── */

/* Build a pipelined GET request buffer.
 * Returns total length of pipeline_depth copies concatenated.
 * Caller must free *out_buf. */
static int http_build_pipeline(const char *host, int port, const char *path,
                               int pipeline_depth, char **out_buf)
{
    /* Measure the single request length first */
    int single_len;
    if (port == 80 || port == 443)
        single_len = snprintf(NULL, 0,
            "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n",
            path, host);
    else
        single_len = snprintf(NULL, 0,
            "GET %s HTTP/1.1\r\nHost: %s:%d\r\n\r\n",
            path, host, port);

    int total_len = single_len * pipeline_depth;
    char *buf = malloc(total_len + 1); /* +1 for snprintf null terminator */

    /* Format into the first slot, then replicate */
    if (port == 80 || port == 443)
        snprintf(buf, single_len + 1,
            "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n",
            path, host);
    else
        snprintf(buf, single_len + 1,
            "GET %s HTTP/1.1\r\nHost: %s:%d\r\n\r\n",
            path, host, port);

    for (int i = 1; i < pipeline_depth; i++)
        memcpy(buf + i * single_len, buf, single_len);

    *out_buf = buf;
    return total_len;
}

/* ── placeholder scanning for {RAND:min:max} and {SEQ:start} ──────── */

static int count_digits(uint64_t n)
{
    if (n == 0) return 1;
    int d = 0;
    while (n > 0) { d++; n /= 10; }
    return d;
}

/*
 * Scan raw template bytes for a single {RAND:min:max} or {SEQ:start}
 * placeholder. If found, replace it in-place with zero-padded digits
 * and populate ph. Returns 0 on success, -1 on parse error.
 * The buffer may be resized if the placeholder text and the padded
 * width differ — caller must use the returned buf/len.
 */
static int scan_placeholder(char **buf, int *len, placeholder_t *ph)
{
    memset(ph, 0, sizeof(*ph));
    ph->type = SUB_NONE;

    /* find {RAND: or {SEQ: */
    char *p = NULL;
    for (int i = 0; i < *len - 5; i++) {
        if ((*buf)[i] == '{' &&
            (memcmp(*buf + i, "{RAND:", 6) == 0 ||
             memcmp(*buf + i, "{SEQ:",  5) == 0)) {
            p = *buf + i;
            break;
        }
    }
    if (!p) return 0; /* no placeholder */

    /* find closing } */
    char *end = memchr(p, '}', *len - (int)(p - *buf));
    if (!end) {
        fprintf(stderr, "Error: unclosed placeholder in raw template\n");
        return -1;
    }

    int ph_start = (int)(p - *buf);
    int ph_len   = (int)(end - p) + 1; /* includes { and } */

    /* parse type and params */
    char tmp[128];
    int tlen = ph_len < (int)sizeof(tmp) ? ph_len : (int)sizeof(tmp) - 1;
    memcpy(tmp, p, tlen);
    tmp[tlen] = '\0';

    if (strncmp(tmp, "{RAND:", 6) == 0) {
        ph->type = SUB_RAND;
        uint64_t mn = 0, mx = 0;
        if (sscanf(tmp, "{RAND:%lu:%lu}", &mn, &mx) != 2 || mx < mn) {
            fprintf(stderr, "Error: bad {RAND:min:max} syntax: %s\n", tmp);
            return -1;
        }
        ph->min = mn;
        ph->max = mx;
        ph->width = count_digits(mx);
    } else if (strncmp(tmp, "{SEQ:", 5) == 0) {
        ph->type = SUB_SEQ;
        uint64_t start = 0;
        if (sscanf(tmp, "{SEQ:%lu}", &start) != 1) {
            fprintf(stderr, "Error: bad {SEQ:start} syntax: %s\n", tmp);
            return -1;
        }
        ph->min = start;
        ph->max = 0;
        atomic_store(&ph->seq, start);
        /* Width matches digits(start) so the first emitted value has no
         * leading zeros. JSON number fields reject leading zeros per
         * RFC 8259, so anything else silently corrupts POST bodies.
         * Users who need the counter to grow past 10^digits(start)
         * must pick a start with enough digit headroom — e.g.
         * {SEQ:10000001} covers ~89M increments with 8-digit width. */
        ph->width = count_digits(start);
    } else {
        return 0;
    }

    ph->offset = ph_start;

    /* replace placeholder text with zero-padded digits in the buffer.
     * If ph_len != ph->width, we need to shift the rest of the buffer. */
    int delta = ph->width - ph_len;
    int new_len = *len + delta;
    if (delta != 0) {
        char *new_buf = malloc(new_len);
        if (!new_buf) return -1;
        memcpy(new_buf, *buf, ph_start);
        memset(new_buf + ph_start, '0', ph->width);
        memcpy(new_buf + ph_start + ph->width,
               *buf + ph_start + ph_len,
               *len - ph_start - ph_len);
        free(*buf);
        *buf = new_buf;
        *len = new_len;
    } else {
        memset(*buf + ph_start, '0', ph->width);
    }

    return 0;
}

/* ── raw request file loading ──────────────────────────────────────── */

static char *load_file(const char *path, int *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open raw request file '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz);
    if (!buf) {
        fprintf(stderr, "Error: out of memory reading '%s'\n", path);
        fclose(f);
        return NULL;
    }
    if ((long)fread(buf, 1, sz, f) != sz) {
        fprintf(stderr, "Error: failed to read '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (int)sz;
    return buf;
}

static int build_pipeline_from_raw(const char *raw, int raw_len,
                                   int pipeline_depth, request_tpl_t *tpl)
{
    tpl->request_len  = raw_len;
    tpl->pipeline_len = raw_len * pipeline_depth;
    tpl->pipeline_buf = malloc(tpl->pipeline_len);
    if (!tpl->pipeline_buf) {
        fprintf(stderr, "Error: out of memory building pipeline\n");
        return -1;
    }
    memcpy(tpl->pipeline_buf, raw, raw_len);
    for (int i = 1; i < pipeline_depth; i++)
        memcpy(tpl->pipeline_buf + i * raw_len, raw, raw_len);
    return 0;
}

/* ── the run's templates ───────────────────────────────────────────── */

int templates_build(const char *raw_files, const int have_url, const int pipeline_depth,
                    char *host, const int host_sz, int *port, const char *path,
                    struct sockaddr_in *addr,
                    request_tpl_t **out_templates, int *out_num_templates)
{
    request_tpl_t *templates = NULL;
    int num_templates = 0;

    if (raw_files) {
        /* Count commas to get number of files */
        int count = 1;
        for (const char *p = raw_files; *p; p++)
            if (*p == ',') count++;

        /* worker_stats_t indexes tpl_responses[]/tpl_responses_2xx[] by
           template, and both are fixed at MAX_TEMPLATES. Going past that
           writes over the adjacent tpl_latency pointer. */
        if (count > MAX_TEMPLATES) {
            fprintf(stderr, "Error: at most %d --raw templates supported "
                            "(got %d)\n", MAX_TEMPLATES, count);
            return -1;
        }

        templates = calloc(count, sizeof(request_tpl_t));
        if (!templates) { fprintf(stderr, "Error: out of memory\n"); return -1; }

        /* Parse comma-separated file list */
        char *files_dup = strdup(raw_files);
        if (!files_dup) { fprintf(stderr, "Error: out of memory\n"); return -1; }
        char *saveptr;
        char *tok = strtok_r(files_dup, ",", &saveptr);
        while (tok) {
            int raw_len;
            char *raw = load_file(tok, &raw_len);
            if (!raw) { free(files_dup); return -1; }

            /* If no URL was given, extract host:port from the first raw file's Host header */
            if (!have_url && num_templates == 0) {
                char *host_hdr = strstr(raw, "Host: ");
                if (!host_hdr) host_hdr = strstr(raw, "host: ");
                if (host_hdr) {
                    host_hdr += 6;
                    char *eol = strstr(host_hdr, "\r\n");
                    int hlen = eol ? (int)(eol - host_hdr) : (int)strlen(host_hdr);
                    if (hlen >= host_sz) hlen = host_sz - 1;
                    memcpy(host, host_hdr, hlen);
                    host[hlen] = '\0';
                    /* Parse host:port */
                    char *colon = strchr(host, ':');
                    if (colon) {
                        *colon = '\0';
                        *port = atoi(colon + 1);
                    }
                    addr->sin_port = htons(*port);
                    if (inet_pton(AF_INET, host, &addr->sin_addr) != 1) {
                        struct hostent *he = gethostbyname(host);
                        if (!he) {
                            fprintf(stderr, "Error: cannot resolve host '%s'\n", host);
                            free(files_dup); return -1;
                        }
                        memcpy(&addr->sin_addr, he->h_addr_list[0], he->h_length);
                    }
                }
            }

            /* Scan for {RAND:min:max} or {SEQ:start} placeholder.
             * Modifies raw in-place (may realloc) and populates .ph */
            placeholder_t ph;
            if (scan_placeholder(&raw, &raw_len, &ph) != 0) {
                free(raw); free(files_dup); return -1;
            }

            build_pipeline_from_raw(raw, raw_len, pipeline_depth, &templates[num_templates]);
            templates[num_templates].ph = ph;
            num_templates++;
            free(raw);
            tok = strtok_r(NULL, ",", &saveptr);
        }
        free(files_dup);

        /* strtok_r skips empty fields, so --raw "" and --raw ",,," both get
           here with nothing loaded. num_templates is a divisor in
           start_connect's template assignment, so zero is a SIGFPE. */
        if (num_templates == 0) {
            fprintf(stderr, "Error: --raw listed no template files\n");
            free(templates);
            return -1;
        }
    } else {
        /* Build GET request from URL */
        templates = calloc(1, sizeof(request_tpl_t));
        if (!templates) { fprintf(stderr, "Error: out of memory\n"); return -1; }
        num_templates = 1;

        char *single_buf;
        int request_len = http_build_pipeline(host, *port, path, 1, &single_buf);
        free(single_buf);

        char *pipeline_buf;
        http_build_pipeline(host, *port, path, pipeline_depth, &pipeline_buf);

        templates[0].pipeline_buf = pipeline_buf;
        templates[0].request_len  = request_len;
        templates[0].pipeline_len = request_len * pipeline_depth;
    }

    *out_templates = templates;
    *out_num_templates = num_templates;
    return 0;
}

void templates_free(request_tpl_t *templates, const int num_templates)
{
    for (int i = 0; i < num_templates; i++)
        free(templates[i].pipeline_buf);
    free(templates);
}
