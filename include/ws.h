#pragma once

#include <stdint.h>

/* WebSocket frame parser state (per-connection) */
typedef struct ws_parser {
    int      state;          /* 0=byte0, 1=byte1, 2=ext_len, 3=payload */
    int      opcode;
    int      fin;
    uint64_t payload_len;
    uint64_t payload_recv;
    int      ext_len_bytes;  /* 0, 2, or 8 */
    int      ext_len_pos;
    uint8_t  ext_buf[8];
    int      completed_count;
} ws_parser_t;

/* Parse incoming WebSocket frames. Returns number of complete frames. */
int ws_parse_frames(ws_parser_t *p, const uint8_t *data, int len);

/* Reset parser */
void ws_parser_reset(ws_parser_t *p);

/* Build a masked WebSocket text frame.
 * Returns total frame length. Caller provides out_buf (must be >= payload_len + 14). */
int ws_build_frame(uint8_t *out_buf, const uint8_t *payload, int payload_len);

/* Build the HTTP upgrade request.
 * Returns length written to out_buf. */
int ws_build_upgrade_request(char *out_buf, int buf_size,
                             const char *host, int port, const char *path);
