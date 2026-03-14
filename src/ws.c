#include "ws.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Parser ────────────────────────────────────────────────────────── */

void ws_parser_reset(ws_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

int ws_parse_frames(ws_parser_t *p, const uint8_t *data, int len)
{
    p->completed_count = 0;
    int pos = 0;

    while (pos < len) {
        switch (p->state) {
        case 0: /* byte 0: FIN + opcode */
            p->fin    = (data[pos] >> 7) & 1;
            p->opcode = data[pos] & 0x0F;
            p->state  = 1;
            pos++;
            break;

        case 1: { /* byte 1: MASK + payload length */
            int mask = (data[pos] >> 7) & 1;
            int len7 = data[pos] & 0x7F;
            (void)mask; /* server-to-client frames are not masked */
            if (len7 < 126) {
                p->payload_len = len7;
                p->ext_len_bytes = 0;
                p->payload_recv = 0;
                p->state = (p->payload_len > 0) ? 3 : 0;
                if (p->payload_len == 0)
                    p->completed_count++;
            } else if (len7 == 126) {
                p->ext_len_bytes = 2;
                p->ext_len_pos = 0;
                p->state = 2;
            } else { /* 127 */
                p->ext_len_bytes = 8;
                p->ext_len_pos = 0;
                p->state = 2;
            }
            pos++;
            break;
        }

        case 2: { /* extended length bytes */
            int need = p->ext_len_bytes - p->ext_len_pos;
            int avail = len - pos;
            int take = (avail < need) ? avail : need;
            memcpy(p->ext_buf + p->ext_len_pos, data + pos, take);
            p->ext_len_pos += take;
            pos += take;
            if (p->ext_len_pos >= p->ext_len_bytes) {
                if (p->ext_len_bytes == 2) {
                    p->payload_len = ((uint64_t)p->ext_buf[0] << 8) |
                                      (uint64_t)p->ext_buf[1];
                } else {
                    p->payload_len = 0;
                    for (int i = 0; i < 8; i++)
                        p->payload_len = (p->payload_len << 8) | p->ext_buf[i];
                }
                p->payload_recv = 0;
                p->state = (p->payload_len > 0) ? 3 : 0;
                if (p->payload_len == 0)
                    p->completed_count++;
            }
            break;
        }

        case 3: { /* payload data — skip it */
            uint64_t remaining = p->payload_len - p->payload_recv;
            uint64_t avail = len - pos;
            if (avail >= remaining) {
                pos += (int)remaining;
                p->payload_recv = p->payload_len;
                p->completed_count++;
                p->state = 0;
            } else {
                p->payload_recv += avail;
                pos = len;
            }
            break;
        }
        }
    }
    return p->completed_count;
}

/* ── Frame builder ─────────────────────────────────────────────────── */

int ws_build_frame(uint8_t *out, const uint8_t *payload, int payload_len)
{
    int pos = 0;

    /* byte 0: FIN=1, opcode=1 (text) */
    out[pos++] = 0x81;

    /* byte 1: MASK=1 + length */
    if (payload_len < 126) {
        out[pos++] = 0x80 | (uint8_t)payload_len;
    } else if (payload_len < 65536) {
        out[pos++] = 0x80 | 126;
        out[pos++] = (payload_len >> 8) & 0xFF;
        out[pos++] = payload_len & 0xFF;
    } else {
        out[pos++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            out[pos++] = ((uint64_t)payload_len >> (i * 8)) & 0xFF;
    }

    /* 4-byte masking key (fixed — doesn't need to be random for benchmarking) */
    uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    memcpy(out + pos, mask, 4);
    pos += 4;

    /* masked payload */
    for (int i = 0; i < payload_len; i++)
        out[pos + i] = payload[i] ^ mask[i & 3];
    pos += payload_len;

    return pos;
}

/* ── Upgrade request builder ───────────────────────────────────────── */

int ws_build_upgrade_request(char *out, int buf_size,
                             const char *host, int port, const char *path)
{
    /* Fixed Sec-WebSocket-Key (doesn't need to be random for benchmarking) */
    int n = snprintf(out, buf_size,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, port);
    return n;
}
