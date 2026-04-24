#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "picohttpparser.h"

int http_build_pipeline(const char *host, int port, const char *path,
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

void http_parser_reset(http_parser_t *p)
{
    p->state = 0;
    p->content_length = -1;
    p->body_received = 0;
    p->header_buf_len = 0;
    p->status_code = 0;
    p->chunked = 0;
    p->chunk_remaining = 0;
    p->chunk_state = 0;
    p->chunk_line_len = 0;
}

/*
 * Accumulate header data and parse with picohttpparser.
 * Extracts status code, Content-Length, and Transfer-Encoding.
 * Returns pointer past end of headers, or NULL if incomplete.
 */
static const uint8_t *parse_headers(http_parser_t *p, const uint8_t *data, int len)
{
    /* Append to header_buf, look for \r\n\r\n */
    int space = (int)sizeof(p->header_buf) - p->header_buf_len - 1;
    int copy = len < space ? len : space;
    memcpy(p->header_buf + p->header_buf_len, data, copy);
    p->header_buf_len += copy;
    p->header_buf[p->header_buf_len] = '\0';

    int minor_version, status;
    const char *msg;
    size_t msg_len, num_headers = 16;
    struct phr_header headers[16];

    int ret = phr_parse_response(p->header_buf, (size_t)p->header_buf_len,
                                 &minor_version, &status, &msg, &msg_len,
                                 headers, &num_headers, 0);
    if (ret == -2)
        return NULL; /* incomplete */
    if (ret == -1)
        return NULL; /* parse error, treat as incomplete */

    int header_len = ret;
    p->status_code = status;

    /* Find Content-Length or Transfer-Encoding in parsed headers */
    p->content_length = 0;
    p->chunked = 0;
    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len == 14 &&
            strncasecmp(headers[i].name, "content-length", 14) == 0) {
            p->content_length = atoi(headers[i].value);
        } else if (headers[i].name_len == 17 &&
                   strncasecmp(headers[i].name, "transfer-encoding", 17) == 0) {
            /* Check if value contains "chunked" */
            for (size_t j = 0; j + 7 <= headers[i].value_len; j++) {
                if (strncasecmp(headers[i].value + j, "chunked", 7) == 0) {
                    p->chunked = 1;
                    break;
                }
            }
        }
    }

    /* How many bytes of `data` were consumed as headers vs body */
    int consumed_from_data = header_len - (p->header_buf_len - copy);
    if (consumed_from_data < 0) consumed_from_data = 0;
    if (consumed_from_data > len) consumed_from_data = len;

    if (p->chunked) {
        p->state = 2;
        p->chunk_state = 0;
        p->chunk_remaining = 0;
        p->chunk_line_len = 0;
    } else {
        p->state = 1;
    }
    p->body_received = 0;
    p->header_buf_len = 0;

    /* Return pointer into original data after the headers */
    return data + consumed_from_data;
}

int http_parse_responses(http_parser_t *p, const uint8_t *data, int len)
{
    int completed = 0;
    p->completed_count = 0;
    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    while (ptr < end) {
        /* Detect stuck iterations: if one full pass through the state blocks
         * neither advances ptr nor changes state, we need more data — bail
         * instead of spinning. E.g. chunked response splits the post-chunk
         * "\r\n" across recv boundaries: chunk_state==2 with 1 byte left
         * breaks out of the middle while but ptr<end keeps the outer loop
         * alive forever. */
        const uint8_t *iter_start_ptr = ptr;
        const int iter_start_state = p->state;
        const int iter_start_chunk_state = p->chunk_state;

        if (p->state == 0) {
            /* Scanning headers */
            const uint8_t *body_start = parse_headers(p, ptr, (int)(end - ptr));
            if (!body_start)
                break; /* Need more data */
            ptr = body_start;
        }

        if (p->state == 1) {
            /* Reading content-length body */
            int remaining = p->content_length - p->body_received;
            int available = (int)(end - ptr);
            int consume = available < remaining ? available : remaining;
            p->body_received += consume;
            ptr += consume;

            if (p->body_received >= p->content_length) {
                if (p->completed_count < 256)
                    p->completed_statuses[p->completed_count++] = p->status_code;
                completed++;
                p->state = 0;
                p->content_length = -1;
                p->body_received = 0;
                p->status_code = 0;
            }
        }

        if (p->state == 2) {
            /* Reading chunked body */
            while (ptr < end) {
                if (p->chunk_state == 0) {
                    /* Accumulate chunk-size line until \r\n */
                    while (ptr < end) {
                        char c = (char)*ptr++;
                        if (c == '\n' && p->chunk_line_len > 0 &&
                            p->chunk_line[p->chunk_line_len - 1] == '\r') {
                            p->chunk_line[p->chunk_line_len - 1] = '\0';
                            long size = strtol(p->chunk_line, NULL, 16);
                            p->chunk_line_len = 0;
                            if (size == 0) {
                                /* Final chunk — skip optional trailing CRLF */
                                if (ptr + 1 < end && ptr[0] == '\r' && ptr[1] == '\n')
                                    ptr += 2;
                                if (p->completed_count < 256)
                                    p->completed_statuses[p->completed_count++] = p->status_code;
                                completed++;
                                p->state = 0;
                                p->content_length = -1;
                                p->body_received = 0;
                                p->status_code = 0;
                                p->chunked = 0;
                                p->chunk_remaining = 0;
                                p->chunk_state = 0;
                                goto next_response;
                            }
                            p->chunk_remaining = (int)size;
                            p->chunk_state = 1;
                            break;
                        }
                        if (p->chunk_line_len < (int)sizeof(p->chunk_line) - 1)
                            p->chunk_line[p->chunk_line_len++] = c;
                    }
                    if (p->chunk_state == 0)
                        break; /* need more data for size line */
                }

                if (p->chunk_state == 1) {
                    /* Consume chunk data */
                    int available = (int)(end - ptr);
                    int consume = available < p->chunk_remaining ? available : p->chunk_remaining;
                    ptr += consume;
                    p->chunk_remaining -= consume;
                    if (p->chunk_remaining == 0)
                        p->chunk_state = 2;
                    else
                        break; /* need more data */
                }

                if (p->chunk_state == 2) {
                    /* Consume post-chunk CRLF */
                    int available = (int)(end - ptr);
                    if (available >= 2) {
                        ptr += 2; /* skip \r\n */
                        p->chunk_state = 0;
                    } else {
                        break; /* need more data for CRLF */
                    }
                }
            }
        }
next_response:
        if (ptr == iter_start_ptr &&
            p->state == iter_start_state &&
            p->chunk_state == iter_start_chunk_state) {
            break;
        }
    }

    return completed;
}