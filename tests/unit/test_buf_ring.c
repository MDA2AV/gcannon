/* Exercise the private production helper without requiring an io_uring fd. */
#include "../../src/core/worker.c"

static int failures;

static void check_return_order(unsigned entries, uint16_t start)
{
    worker_t w = {0};
    w.recv_buf_size = 16;
    w.buf_mask = entries - 1;
    w.buf_ring = calloc(entries, sizeof(struct io_uring_buf));
    w.buf_slab = calloc(entries, w.recv_buf_size);
    if (!w.buf_ring || !w.buf_slab) {
        fprintf(stderr, "buffer-ring test allocation failed\n");
        exit(1);
    }
    for (unsigned i = 0; i < entries; i++)
        io_uring_buf_ring_add(w.buf_ring,
            w.buf_slab + (size_t)i * w.recv_buf_size,
            w.recv_buf_size, i, w.buf_mask, i);
    w.buf_ring->tail = start;
    w.buf_index = start;

    /* Hold the logical consumer until all three returns are published. */
    const uint16_t returned[] = {2, 0, 1};
    for (unsigned i = 0; i < 3; i++)
        return_buffer(&w, returned[i]);

    for (unsigned i = 0; i < 3; i++) {
        const struct io_uring_buf *b =
            &w.buf_ring->bufs[(start + i) & w.buf_mask];
        const uint64_t expected_addr =
            (uint64_t)(uintptr_t)(w.buf_slab +
                (size_t)returned[i] * w.recv_buf_size);
        if (b->bid != returned[i] || b->addr != expected_addr ||
            b->len != (unsigned)w.recv_buf_size) {
            fprintf(stderr,
                "entries=%u start=%u return=%u: expected bid=%u, got=%u\n",
                entries, start, i, returned[i], b->bid);
            failures++;
        }
    }
    if (w.buf_ring->tail != (uint16_t)(start + 3)) {
        fprintf(stderr, "unexpected published tail\n");
        failures++;
    }
    free(w.buf_slab);
    free(w.buf_ring);
}

int main(void)
{
    check_return_order(4, 4);
    check_return_order(4, UINT16_MAX - 1);
    check_return_order(BUF_RING_ENTRIES, BUF_RING_ENTRIES);
    check_return_order(BUF_RING_ENTRIES, UINT16_MAX - 1);
    printf("buffer-ring return order: %d failures\n", failures);
    return failures != 0;
}
