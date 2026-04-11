#pragma once

#include <stdint.h>

#define GCANNON_VERSION "0.5.0"

/* ── User-data packing (64-bit: [kind:16][gen:16][conn_idx:32]) ───── */

typedef enum { UD_CONNECT = 1, UD_RECV = 2, UD_SEND = 3, UD_CANCEL = 4 } ud_kind_t;

#define PACK_UD(kind, gen, idx)  (((uint64_t)(kind) << 48) | ((uint64_t)((gen) & 0xFFFF) << 32) | (uint32_t)(idx))
#define UD_KIND(ud)              ((ud_kind_t)((ud) >> 48))
#define UD_GEN(ud)               ((uint16_t)(((ud) >> 32) & 0xFFFF))
#define UD_IDX(ud)               ((int)((ud) & 0xFFFFFFFF))

/* ── IOU_PBUF_RING_INC (incremental buffer consumption, kernel 6.10+) */
#ifndef IOU_PBUF_RING_INC
#define IOU_PBUF_RING_INC 2
#endif

/* ── Tunables ─────────────────────────────────────────────────────── */

#define RING_ENTRIES          4096
#define BUF_RING_ENTRIES      4096
#define RECV_BUF_SIZE_DEFAULT 4096
#define BATCH_CQES            2048
#define PIPELINE_DEPTH_MAX    64
#define MAX_CONNS_PER_WORKER  16384
#define BUFFER_RING_BGID      1
#define MAX_TEMPLATES         64
