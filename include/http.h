#pragma once

#include <stdint.h>

/* Build a pipelined GET request buffer.
 * Returns total length of pipeline_depth copies concatenated.
 * Caller must free *out_buf. */
int http_build_pipeline(const char *host, int port, const char *path,
                        int pipeline_depth, char **out_buf);

/* HTTP response parser state (per-connection) */
typedef struct http_parser {
    int  state;           /* 0 = headers, 1 = content-length body, 2 = chunked body */
    int  content_length;
    int  body_received;
    char header_buf[1024];
    int  header_buf_len;
    int  status_code;     /* last parsed status code */
    int  chunked;         /* 1 if Transfer-Encoding: chunked */
    int  chunk_remaining; /* bytes left in current chunk */
    int  chunk_state;     /* 0 = reading size line, 1 = data, 2 = post-chunk CRLF */
    char chunk_line[20];  /* accumulates hex size line across recv boundaries */
    int  chunk_line_len;
    /* Status codes of completed responses (filled by http_parse_responses) */
    int  completed_statuses[256];
    int  completed_count;
} http_parser_t;

/* Feed data into the parser. Returns number of complete responses found.
 * Parser state is updated in-place for partial responses across buffers. */
int http_parse_responses(http_parser_t *p, const uint8_t *data, int len);

/* Reset parser to initial state */
void http_parser_reset(http_parser_t *p);
