#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int http_build_pipeline(const char *host, int port, const char *path,
                        int pipeline_depth, char **out_buf)
{
    char single[512];
    int single_len;

    if (port == 80)
        single_len = snprintf(single, sizeof(single),
            "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n",
            path, host);
    else
        single_len = snprintf(single, sizeof(single),
            "GET %s HTTP/1.1\r\nHost: %s:%d\r\n\r\n",
            path, host, port);

    int total_len = single_len * pipeline_depth;
    char *buf = malloc(total_len);
    for (int i = 0; i < pipeline_depth; i++)
        memcpy(buf + i * single_len, single, single_len);

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
}

/*
 * Scan for \r\n\r\n in the header accumulation buffer.
 * If found, extract Content-Length and status code.
 * Returns pointer past end of headers, or NULL if not yet complete.
 */
static const uint8_t *parse_headers(http_parser_t *p, const uint8_t *data, int len)
{
    /* Append to header_buf, look for \r\n\r\n */
    int space = (int)sizeof(p->header_buf) - p->header_buf_len - 1;
    int copy = len < space ? len : space;
    memcpy(p->header_buf + p->header_buf_len, data, copy);
    p->header_buf_len += copy;
    p->header_buf[p->header_buf_len] = '\0';

    /* Search for end of headers */
    char *end = strstr(p->header_buf, "\r\n\r\n");
    if (!end)
        return NULL;

    int header_len = (int)(end - p->header_buf) + 4;

    /* Parse status code from first line: "HTTP/1.1 200 OK\r\n" */
    char *sp = strchr(p->header_buf, ' ');
    if (sp)
        p->status_code = atoi(sp + 1);

    /* Parse Content-Length */
    p->content_length = 0;
    char *cl = strcasestr(p->header_buf, "Content-Length:");
    if (cl) {
        cl += 15; /* skip "Content-Length:" */
        while (*cl == ' ') cl++;
        p->content_length = atoi(cl);
    }

    /* How many bytes of `data` were consumed as headers vs body */
    int consumed_from_data = header_len - (p->header_buf_len - copy);
    if (consumed_from_data < 0) consumed_from_data = 0;
    if (consumed_from_data > len) consumed_from_data = len;

    p->state = 1;
    p->body_received = 0;
    p->header_buf_len = 0;

    /* Return pointer into original data after the headers */
    return data + consumed_from_data;
}

int http_parse_responses(http_parser_t *p, const uint8_t *data, int len)
{
    int completed = 0;
    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    while (ptr < end) {
        if (p->state == 0) {
            /* Scanning headers */
            const uint8_t *body_start = parse_headers(p, ptr, (int)(end - ptr));
            if (!body_start)
                break; /* Need more data */
            ptr = body_start;
        }

        if (p->state == 1) {
            /* Reading body */
            int remaining = p->content_length - p->body_received;
            int available = (int)(end - ptr);
            int consume = available < remaining ? available : remaining;
            p->body_received += consume;
            ptr += consume;

            if (p->body_received >= p->content_length) {
                completed++;
                /* Reset for next response */
                p->state = 0;
                p->content_length = -1;
                p->body_received = 0;
                p->status_code = 0;
            }
        }
    }

    return completed;
}
