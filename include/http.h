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
    int  chunk_state;     /* 0 = size line, 1 = data, 2 = post-chunk CRLF,
                             3 = trailer section after the last chunk */
    char chunk_line[20];  /* accumulates hex size line across recv boundaries */
    int  chunk_line_len;  /* in state 3, counts bytes on the current trailer
                             line (saturating at 2) so an empty line is still
                             recognised when a recv splits it */
    int  crlf_seen;       /* bytes of the post-chunk CRLF already consumed;
                             a recv boundary may fall between CR and LF */
    int  parse_error;     /* 1 = unrecoverable (malformed response or headers
                             exceed header_buf) — caller must reconnect */
    /* Status codes of completed responses (filled by http_parse_responses) */
    int  completed_statuses[256];
    int  completed_count;
    /* Per-call status-class tallies. Unlike completed_statuses (capped at 256
       entries), these are unbounded, so status accounting stays correct when a
       single recv carries more than 256 pipelined responses. */
    int  cls_2xx, cls_3xx, cls_4xx, cls_5xx, cls_other;
} http_parser_t;

/* Feed data into the parser. Returns number of complete responses found,
 * or -1 on unrecoverable parse failure (caller must drop the connection).
 * Parser state is updated in-place for partial responses across buffers. */
int http_parse_responses(http_parser_t *p, const uint8_t *data, int len);

/* Reset parser to initial state */
void http_parser_reset(http_parser_t *p);
