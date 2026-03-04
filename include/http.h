#pragma once

#include <stdint.h>

/* Build a pipelined GET request buffer.
 * Returns total length of pipeline_depth copies concatenated.
 * Caller must free *out_buf. */
int http_build_pipeline(const char *host, int port, const char *path,
                        int pipeline_depth, char **out_buf);

/* HTTP response parser state (per-connection) */
typedef struct http_parser {
    int  state;           /* 0 = scanning headers, 1 = reading body */
    int  content_length;
    int  body_received;
    char header_buf[512];
    int  header_buf_len;
    int  status_code;     /* last parsed status code */
} http_parser_t;

/* Feed data into the parser. Returns number of complete responses found.
 * Parser state is updated in-place for partial responses across buffers. */
int http_parse_responses(http_parser_t *p, const uint8_t *data, int len);

/* Reset parser to initial state */
void http_parser_reset(http_parser_t *p);
