/* SPDX-License-Identifier: MIT
 * Processes raw records or text puzzles; see host/README.md.
 * Copies input and updates IDs for the specified duration, counting results without reading them.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#include "acceldoku_ioctl.h"

/**
 * @brief Device fd and per-direction buffer mappings used by the tool.
 * @details setup() allocates and maps the requested buffers, indexed by queue direction
 * and buffer number. At shutdown, STREAMOFF is followed by releasing all mappings and
 * arrays and closing the fd.
 */
struct acceldoku_device {
    int fd;
    unsigned buffer_counts[2];
    uint8_t **mapped_buffers[2];
};

static void fail(const char *what) {
    perror(what);
    exit(EXIT_FAILURE); /* Releasing the fd and VMAs stops DMA and frees the buffers. */
}

static void control(int fd, unsigned long cmd) {
    if (ioctl(fd, cmd) < 0) {
        fail("control ioctl");
    }
}

static unsigned number(const char *text) {
    char *end;
    errno = 0;
    unsigned long count = strtoul(text, &end, 10);
    if (errno || !*text || *end || !count || count > ACCELDOKU_DMA_MAX_BUFFERS) {
        fprintf(stderr, "Buffer count must be 1..%u\n", ACCELDOKU_DMA_MAX_BUFFERS);
        exit(EXIT_FAILURE);
    }
    return (unsigned)count;
}

static double now(void) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp)) {
        fail("clock_gettime");
    }
    return (double)timestamp.tv_sec + (double)timestamp.tv_nsec * 1e-9;
}

static void setup(struct acceldoku_device *device) {
    device->fd = open("/dev/acceldoku", O_RDWR | O_CLOEXEC);
    if (device->fd < 0) {
        fail("open /dev/acceldoku");
    }
    struct acceldoku_dma_info info = {0};
    if (ioctl(device->fd, ACCELDOKUIOC_DMA_INFO, &info) < 0) {
        fail("DMA_INFO");
    }
    if (info.abi_version != ACCELDOKU_DMA_ABI_VERSION || info.slot_bytes != ACCELDOKU_DMA_SLOT_BYTES) {
        fprintf(stderr, "Incompatible ACCELDOKU ABI\n");
        exit(EXIT_FAILURE);
    }
    struct acceldoku_dma_config cfg = {
        .tx_buffers = device->buffer_counts[ACCELDOKU_QUEUE_TX],
        .rx_buffers = device->buffer_counts[ACCELDOKU_QUEUE_RX],
    };
    if (ioctl(device->fd, ACCELDOKUIOC_DMA_CONFIG, &cfg) < 0) {
        fail("DMA_CONFIG");
    }
    for (enum acceldoku_queue direction = ACCELDOKU_QUEUE_TX; direction <= ACCELDOKU_QUEUE_RX; direction++) {
        device->mapped_buffers[direction] =
            calloc(device->buffer_counts[direction], sizeof(*device->mapped_buffers[direction]));
        if (!device->mapped_buffers[direction]) {
            fail("calloc");
        }
        for (unsigned index = 0; index < device->buffer_counts[direction]; index++) {
            struct acceldoku_buffer_info buffer = {.queue = direction, .index = index};
            if (ioctl(device->fd, ACCELDOKUIOC_QUERYBUF, &buffer) < 0) {
                fail("QUERYBUF");
            }
            device->mapped_buffers[direction][index] =
                mmap(NULL, buffer.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, device->fd,
                     (off_t)buffer.mmap_offset);
            if (device->mapped_buffers[direction][index] == MAP_FAILED) {
                fail("mmap");
            }
        }
    }
}

static void queue(struct acceldoku_device *device, enum acceldoku_queue direction, unsigned index,
                  unsigned records) {
    struct acceldoku_buffer buffer = {.queue = direction, .index = index, .records = records};
    if (ioctl(device->fd, ACCELDOKUIOC_QBUF, &buffer) < 0) {
        fail("QBUF");
    }
}

static int try_dequeue(struct acceldoku_device *device, enum acceldoku_queue direction,
                       struct acceldoku_buffer *buffer) {
    *buffer = (struct acceldoku_buffer){.queue = direction};
    if (ioctl(device->fd, ACCELDOKUIOC_DQBUF, buffer) == 0) {
        return 1;
    }
    if (errno == EAGAIN || errno == EINTR) {
        return 0;
    }
    fail("DQBUF");
    return 0;
}

static void wait_ready(struct acceldoku_device *device, short events) {
    struct pollfd descriptor = {.fd = device->fd, .events = events};
    int ret;
    do {
        ret = poll(&descriptor, 1, 10000);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        fail("poll");
    }
    if (!ret || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        fprintf(stderr, "DMA timeout/error: revents=%x\n", descriptor.revents);
        exit(EXIT_FAILURE);
    }
}

/* For streaming benchmarks: copy each record once, replacing its ID before writing.
 * This avoids rescanning the uncached TX region with strided accesses to update IDs. */
static void copy_records(uint8_t *dst, const uint8_t *src, unsigned count, uint64_t first) {
    for (unsigned index = 0; index < count; index++) {
        const uint8_t *source_record = src + (size_t)index * 64;
        if (index + 8 < count) {
            __builtin_prefetch(source_record + 512, 0, 3);
        }
#ifdef __aarch64__
        uint8x16_t v0 = vld1q_u8(source_record);
        uint8x16_t v1 = vld1q_u8(source_record + 16);
        uint8x16_t v2 = vld1q_u8(source_record + 32);
        uint8x16_t v3 = vld1q_u8(source_record + 48);
        v0 = vreinterpretq_u8_u64(vsetq_lane_u64(first + index, vreinterpretq_u64_u8(v0), 0));
        vst1q_u8(dst, v0);
        vst1q_u8(dst + 16, v1);
        vst1q_u8(dst + 32, v2);
        vst1q_u8(dst + 48, v3);
#else
        uint8_t record[64];
        memcpy(record, source_record, 64);
        for (unsigned byte = 0; byte < 8; byte++) {
            record[byte] = (uint8_t)((first + index) >> (8 * byte));
        }
        memcpy(dst, record, 64);
#endif
        dst += 64;
    }
}

static void fill(uint8_t *dst, const uint8_t *src, size_t records, size_t *position,
                 uint64_t first) {
    unsigned remaining = ACCELDOKU_RECORDS_PER_SLOT;
    while (remaining) {
        unsigned count =
            records - *position < remaining ? (unsigned)(records - *position) : remaining;
        copy_records(dst, src + *position * 64, count, first);
        dst += (size_t)count * 64;
        first += count;
        remaining -= count;
        *position += count;
        if (*position == records) {
            *position = 0;
        }
    }
}

static uint8_t *load_input(const char *path, size_t *records) {
    FILE *in = fopen(path, "rb");
    struct stat st;
    if (!in || fstat(fileno(in), &st)) {
        fail("input");
    }
    if (st.st_size <= 0 || st.st_size % 64 || (uint64_t)st.st_size > SIZE_MAX) {
        fprintf(stderr, "Input must contain complete 64-byte records\n");
        exit(EXIT_FAILURE);
    }
    size_t bytes = (size_t)st.st_size;
    uint8_t *source = malloc(bytes);
    if (!source) {
        fail("malloc input");
    }
    if (fread(source, 1, bytes, in) != bytes) {
        fail("read input");
    }
    if (fclose(in)) {
        fail("close input");
    }
    *records = bytes / 64;
    return source;
}

static uint8_t *load_text(const char *path, size_t *records) {
    FILE *in = fopen(path, "rb");
    if (!in) {
        fail("input");
    }
    char *line = NULL;
    size_t line_capacity = 0, count = 0, capacity = 0, line_number = 0;
    uint8_t *source = NULL;
    ssize_t length;
    while ((length = getline(&line, &line_capacity, in)) >= 0) {
        line_number++;
        size_t begin = 0, end = (size_t)length;
        while (begin < end && (line[begin] == ' ' || line[begin] == '\t' || line[begin] == '\r' || line[begin] == '\n')) begin++;
        while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r' || line[end - 1] == '\n')) end--;
        if (begin == end || line[begin] == '#') continue;
        if (end - begin != 81) {
            fprintf(stderr, "Invalid puzzle length at line %zu\n", line_number);
            exit(EXIT_FAILURE);
        }
        uint8_t record[64] = {0};
        for (size_t i = 0; i < 81; i++) {
            char c = line[begin + i];
            if (c != '.' && (c < '0' || c > '9')) {
                fprintf(stderr, "Invalid puzzle character at line %zu\n", line_number);
                exit(EXIT_FAILURE);
            }
            unsigned value = c == '.' ? 0 : (unsigned)(c - '0');
            record[9 + i / 2] |= (uint8_t)(value << (4 * (i % 2)));
        }
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2 / 64) {
                fprintf(stderr, "Too many puzzles\n");
                exit(EXIT_FAILURE);
            }
            capacity = capacity ? capacity * 2 : 256;
            uint8_t *grown = realloc(source, capacity * 64);
            if (!grown) fail("malloc input");
            source = grown;
        }
        memcpy(source + count++ * 64, record, 64);
    }
    if (ferror(in)) fail("read input");
    free(line);
    if (fclose(in)) fail("close input");
    if (!count) {
        fprintf(stderr, "Empty puzzle file\n");
        exit(EXIT_FAILURE);
    }
    *records = count;
    return source;
}

int main(int argc, char **argv) {
    int text = argc > 1 && strcmp(argv[1], "--text") == 0;
    if (text) {
        argc--;
        argv++;
    }
    if (argc != 5) {
        fprintf(stderr, "Usage: acceldoku_run [--text] INPUT TX_BUFFERS RX_BUFFERS SECONDS\n");
        return EXIT_FAILURE;
    }
    char *end = NULL;
    double seconds = strtod(argv[4], &end);
    if (!*argv[4] || *end || !(seconds > 0 && seconds <= 3600)) {
        fprintf(stderr, "Duration must satisfy 0 < SECONDS <= 3600\n");
        return EXIT_FAILURE;
    }
    size_t records;
    uint8_t *source = text ? load_text(argv[1], &records) : load_input(argv[1], &records);

    struct acceldoku_device device = {
        .fd = -1,
        .buffer_counts = {[ACCELDOKU_QUEUE_TX] = number(argv[2]), [ACCELDOKU_QUEUE_RX] = number(argv[3])},
    };
    setup(&device);

    // Enqueue all output buffers.
    for (unsigned index = 0; index < device.buffer_counts[ACCELDOKU_QUEUE_RX]; index++) {
        queue(&device, ACCELDOKU_QUEUE_RX, index, 0);
    }

    uint64_t submitted = 0;
    uint64_t received = 0;
    size_t position = 0;
    // Fill and enqueue all initial input buffers.
    for (unsigned index = 0; index < device.buffer_counts[ACCELDOKU_QUEUE_TX]; index++) {
        fill(device.mapped_buffers[ACCELDOKU_QUEUE_TX][index], source, records, &position, submitted);
        queue(&device, ACCELDOKU_QUEUE_TX, index, ACCELDOKU_RECORDS_PER_SLOT);
        submitted += ACCELDOKU_RECORDS_PER_SLOT;
    }
    // Start timing and streaming.
    double begin = now();
    double deadline = begin + seconds;
    control(device.fd, ACCELDOKUIOC_STREAMON);
    int feeding = 1;
    while (feeding || received != submitted) {
        // After the timed interval, wait only for output.
        wait_ready(&device, POLLIN | (feeding ? POLLOUT : 0));
        if (feeding && now() >= deadline) {
            feeding = 0;
        }
        struct acceldoku_buffer buffer;
        // During the timed interval, dequeue, fill and enqueue input buffers.
        if (feeding && try_dequeue(&device, ACCELDOKU_QUEUE_TX, &buffer)) {
            fill(device.mapped_buffers[ACCELDOKU_QUEUE_TX][buffer.index], source, records, &position,
                 submitted);
            queue(&device, ACCELDOKU_QUEUE_TX, buffer.index, ACCELDOKU_RECORDS_PER_SLOT);
            submitted += ACCELDOKU_RECORDS_PER_SLOT;
        }
        if (try_dequeue(&device, ACCELDOKU_QUEUE_RX, &buffer)) {
            received += buffer.records;
            queue(&device, ACCELDOKU_QUEUE_RX, buffer.index, 0);
        }
    }
    // Stop timing.
    double elapsed = now() - begin;
    // Check statistics.
    struct acceldoku_solver_stats stats;
    if (ioctl(device.fd, ACCELDOKUIOC_SOLVER_STATS, &stats) < 0) {
        fail("SOLVER_STATS");
    }
    if (stats.accepted != submitted || stats.emitted != submitted ||
        stats.dispatched != submitted || stats.collected != submitted) {
        fprintf(stderr,
                "Stage totals mismatch: accepted=%llu emitted=%llu dispatched=%llu "
                "collected=%llu expected=%" PRIu64 "\n",
                (unsigned long long)stats.accepted, (unsigned long long)stats.emitted,
                (unsigned long long)stats.dispatched, (unsigned long long)stats.collected,
                submitted);
        return EXIT_FAILURE;
    }
    // Stop the device.
    control(device.fd, ACCELDOKUIOC_STREAMOFF);
    printf("Stream (no output read): TX/RX=%u/%u, %" PRIu64 " records, %.6f s, %.6f Mproblem/s\n",
           device.buffer_counts[ACCELDOKU_QUEUE_TX], device.buffer_counts[ACCELDOKU_QUEUE_RX], received,
           elapsed, received / elapsed / 1e6);
    // Clean up.
    for (enum acceldoku_queue direction = ACCELDOKU_QUEUE_TX; direction <= ACCELDOKU_QUEUE_RX; direction++) {
        for (unsigned index = 0; index < device.buffer_counts[direction]; index++) {
            if (munmap(device.mapped_buffers[direction][index], ACCELDOKU_DMA_SLOT_BYTES)) {
                fail("munmap");
            }
        }
        free(device.mapped_buffers[direction]);
    }
    close(device.fd);
    free(source);
    return EXIT_SUCCESS;
}
