#pragma once

#include <netinet/in.h>
#include <stdatomic.h>
#include <stdint.h>

/*
 * Request templates: the bytes gcannon sends. Without --raw there is one, a
 * GET for the URL; with --raw there is one per file, sent byte for byte.
 * Each template is built once, as pipeline_depth copies back to back, so a
 * full pipeline goes out in a single send.
 */

/* Placeholder substitution types for {RAND:min:max} and {SEQ:start} in raw templates */
typedef enum { SUB_NONE = 0, SUB_RAND, SUB_SEQ } sub_type_t;

typedef struct {
    sub_type_t type;
    int        offset;      /* byte offset in the single-request buffer */
    int        width;       /* zero-padded digit width (e.g. 6 for 000001-999999) */
    uint64_t   min;         /* RAND lower bound (inclusive) */
    uint64_t   max;         /* RAND upper bound (inclusive) */
    _Atomic uint64_t seq;   /* SEQ: shared atomic counter across all threads */
} placeholder_t;

/* Request template — one per raw request file (or the auto-generated GET) */
typedef struct {
    char *pipeline_buf;   /* pipeline_depth copies concatenated */
    int   request_len;    /* single request length */
    int   pipeline_len;   /* pipeline_depth * request_len */
    placeholder_t ph;     /* {RAND} or {SEQ} placeholder (ph.type == SUB_NONE if none) */
} request_tpl_t;

/* Build the run's templates: one per file in raw_files (comma-separated, at
 * most MAX_TEMPLATES), or, when raw_files is NULL, a GET for host:port/path.
 * Without a URL (have_url == 0) the target comes from the first file's Host
 * header: host and *port are overwritten and *addr is resolved from them.
 * Returns 0, or -1 after printing why. */
int templates_build(const char *raw_files, int have_url, int pipeline_depth,
                    char *host, int host_sz, int *port, const char *path,
                    struct sockaddr_in *addr,
                    request_tpl_t **templates, int *num_templates);

void templates_free(request_tpl_t *templates, int num_templates);
