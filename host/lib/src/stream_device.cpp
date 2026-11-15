#include <fades/stream_device.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace fades {
namespace {
[[noreturn]] void fail(const char *what, int error = errno) {
    throw std::system_error(error, std::generic_category(), what);
}

int control(int fd, unsigned long cmd, void *arg = nullptr) {
    int result;
    do {
        result = ioctl(fd, cmd, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

void copy_records(void *destination, std::span<const PuzzleRecord> records, bool replace_id,
                  std::uint64_t first_id) {
    auto *dst = static_cast<std::uint8_t *>(destination);
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto *src = records[i].data();
        if (i + 8 < records.size()) {
            __builtin_prefetch(records[i + 8].data(), 0, 3);
        }
#ifdef __aarch64__
        auto v0 = vld1q_u8(src);
        auto v1 = vld1q_u8(src + 16);
        auto v2 = vld1q_u8(src + 32);
        auto v3 = vld1q_u8(src + 48);
        if (replace_id) {
            auto id = first_id + i;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            v0 = vreinterpretq_u8_u64(vsetq_lane_u64(id, vreinterpretq_u64_u8(v0), 0));
#else
            // The wire format is little endian even on a big-endian host.
            std::uint8_t bytes[8];
            for (unsigned b = 0; b < 8; ++b) {
                bytes[b] = static_cast<std::uint8_t>(id >> (8 * b));
            }
            v0 = vcombine_u8(vld1_u8(bytes), vget_high_u8(v0));
#endif
        }
        vst1q_u8(dst, v0);
        vst1q_u8(dst + 16, v1);
        vst1q_u8(dst + 32, v2);
        vst1q_u8(dst + 48, v3);
#else
        std::memcpy(dst, src, sizeof(PuzzleRecord));
        if (replace_id) {
            for (unsigned b = 0; b < 8; ++b) {
                dst[b] = static_cast<std::uint8_t>((first_id + i) >> (8 * b));
            }
        }
#endif
        dst += sizeof(PuzzleRecord);
    }
}
} // namespace

PuzzleRecord encode(std::uint64_t id, const Puzzle &puzzle) {
    PuzzleRecord result{};
    for (unsigned i = 0; i < 8; ++i) {
        result[i] = static_cast<std::uint8_t>(id >> (8 * i));
    }
    for (unsigned i = 0; i < puzzle.size(); ++i) {
        if (puzzle[i] > 9) {
            throw std::invalid_argument("invalid cell value");
        }
        result[9 + i / 2] |= puzzle[i] << (4 * (i % 2));
    }
    return result;
}

std::uint64_t record_id(const PuzzleRecord &record) noexcept {
    std::uint64_t id = 0;
    for (unsigned i = 0; i < 8; ++i) {
        id |= std::uint64_t(record[i]) << (8 * i);
    }
    return id;
}

std::uint8_t record_status(const PuzzleRecord &record) noexcept {
    return record[8];
}

Puzzle decode_cells(const PuzzleRecord &record) noexcept {
    Puzzle puzzle{};
    for (unsigned i = 0; i < puzzle.size(); ++i) {
        puzzle[i] = (record[9 + i / 2] >> (4 * (i % 2))) & 15;
    }
    return puzzle;
}

void StreamDevice::check_tx(std::uint32_t index) const {
    if (index >= tx_borrowed_.size()) {
        throw std::out_of_range("TX buffer index");
    }
    if (!tx_borrowed_[index]) {
        throw std::logic_error("TX buffer is not borrowed");
    }
}

void StreamDevice::check_rx(std::uint32_t index) const {
    if (index >= rx_states_.size()) {
        throw std::out_of_range("RX buffer index");
    }
    if (rx_states_[index] != RxState::borrowed) {
        throw std::logic_error("RX buffer is not borrowed");
    }
}

void StreamDevice::write_tx(std::uint32_t index, std::span<const PuzzleRecord> records) {
    check_tx(index);
    if (records.size() > batch_capacity()) {
        throw std::length_error("TX batch exceeds capacity");
    }
    copy_records(buffer_mappings_[FADES_QUEUE_TX][index], records, false, 0);
    tx_record_counts_[index] = records.size();
}

void StreamDevice::write_tx(std::uint32_t index, std::span<const PuzzleRecord> records,
                            std::uint64_t first_id) {
    check_tx(index);
    if (records.size() > batch_capacity()) {
        throw std::length_error("TX batch exceeds capacity");
    }
    if (!records.empty() && records.size() - 1 > UINT64_MAX - first_id) {
        throw std::overflow_error("record IDs overflow");
    }
    copy_records(buffer_mappings_[FADES_QUEUE_TX][index], records, true, first_id);
    tx_record_counts_[index] = records.size();
}

std::size_t StreamDevice::rx_size(std::uint32_t index) const {
    check_rx(index);
    return rx_records_[index];
}

std::span<const PuzzleRecord> StreamDevice::read_rx(std::uint32_t index) const {
    check_rx(index);
    return {static_cast<const PuzzleRecord *>(buffer_mappings_[FADES_QUEUE_RX][index]),
            rx_records_[index]};
}

void StreamDevice::enqueue(unsigned direction, unsigned index, unsigned count) {
    fades_buffer b{};
    b.queue = direction;
    b.index = index;
    b.records = count;
    if (control(fd_, FADESIOC_QBUF, &b) < 0) {
        fail("QBUF");
    }
}

void StreamDevice::enqueue_tx(std::uint32_t index) {
    check_tx(index);
    const auto count = tx_record_counts_[index];
    if (!count) {
        throw std::invalid_argument("invalid TX record count");
    }
    enqueue(FADES_QUEUE_TX, index, static_cast<unsigned>(count));
    tx_borrowed_[index] = false;
    tx_record_counts_[index] = 0;
}

void StreamDevice::release_tx(std::uint32_t index) {
    check_tx(index);
    free_tx_.push_back(index);
    tx_borrowed_[index] = false;
    tx_record_counts_[index] = 0;
}

void StreamDevice::enqueue_rx(std::uint32_t index) {
    check_rx(index);
    if (stream_state_ == StreamState::running) {
        enqueue(FADES_QUEUE_RX, index, 0);
        rx_states_[index] = RxState::queued;
    } else {
        rx_states_[index] = RxState::available;
    }
    rx_records_[index] = 0;
}

void StreamDevice::cleanup() noexcept {
    if (fd_ >= 0) {
        control(fd_, FADESIOC_STREAMOFF);
    }
    for (auto &queue : buffer_mappings_) {
        for (void *p : queue) {
            munmap(p, FADES_DMA_SLOT_BYTES);
        }
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

StreamDevice::StreamDevice(const char *path, std::uint32_t tx, std::uint32_t rx) {
    try {
        fd_ = open(path, O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            fail("open device");
        }
        fades_dma_info info{};
        if (control(fd_, FADESIOC_DMA_INFO, &info) < 0) {
            fail("DMA_INFO");
        }
        if (info.abi_version != FADES_DMA_ABI_VERSION || info.slot_bytes != FADES_DMA_SLOT_BYTES) {
            throw std::runtime_error("incompatible FADES ABI");
        }
        if (!tx || !rx || tx > info.max_buffers || rx > info.max_buffers) {
            throw std::invalid_argument("invalid buffer count");
        }
        fades_dma_config cfg{};
        cfg.tx_buffers = tx;
        cfg.rx_buffers = rx;
        if (control(fd_, FADESIOC_DMA_CONFIG, &cfg) < 0) {
            fail("DMA_CONFIG");
        }
        buffer_mappings_[0].reserve(tx);
        buffer_mappings_[1].reserve(rx);
        free_tx_.reserve(tx);
        tx_borrowed_.resize(tx, false);
        tx_record_counts_.resize(tx, 0);
        rx_states_.resize(rx, RxState::available);
        rx_records_.resize(rx, 0);
        for (unsigned q = 0; q < 2; ++q) {
            for (unsigned i = 0; i < (q == 0 ? tx : rx); ++i) {
                fades_buffer_info b{};
                b.queue = q;
                b.index = i;
                if (control(fd_, FADESIOC_QUERYBUF, &b) < 0) {
                    fail("QUERYBUF");
                }
                if (b.map_bytes != FADES_DMA_SLOT_BYTES ||
                    b.mmap_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
                    throw std::runtime_error("invalid mapping information");
                }
                void *p = mmap(nullptr, b.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                               static_cast<off_t>(b.mmap_offset));
                if (p == MAP_FAILED) {
                    fail("mmap");
                }
                buffer_mappings_[q].push_back(p);
                if (q == FADES_QUEUE_TX) {
                    free_tx_.push_back(i);
                }
            }
        }
    } catch (...) {
        cleanup();
        throw;
    }
}

StreamDevice::~StreamDevice() {
    cleanup();
}

void StreamDevice::stream_on() {
    if (stream_state_ == StreamState::running) {
        throw std::logic_error("stream already running");
    }
    for (unsigned i = 0; i < rx_states_.size(); ++i) {
        if (rx_states_[i] == RxState::available) {
            enqueue(FADES_QUEUE_RX, i, 0);
            rx_states_[i] = RxState::queued;
        }
    }
    if (control(fd_, FADESIOC_STREAMON) < 0) {
        fail("STREAMON");
    }
    stream_state_ = StreamState::running;
}

void StreamDevice::stream_off() {
    if (control(fd_, FADESIOC_STREAMOFF) < 0) {
        fail("STREAMOFF");
    }
    stream_state_ = StreamState::stopped;
    free_tx_.clear();
    for (unsigned i = 0; i < tx_borrowed_.size(); ++i) {
        if (!tx_borrowed_[i]) {
            free_tx_.push_back(i);
        }
    }
    for (auto &state : rx_states_) {
        if (state == RxState::queued) {
            state = RxState::available;
        }
    }
}

std::optional<std::uint32_t> StreamDevice::try_dequeue_tx() {
    unsigned index;
    if (!free_tx_.empty()) {
        index = free_tx_.back();
        free_tx_.pop_back();
    } else {
        fades_buffer b{};
        b.queue = FADES_QUEUE_TX;
        if (control(fd_, FADESIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) {
                return {};
            }
            fail("DQBUF TX");
        }
        index = b.index;
    }
    tx_borrowed_[index] = true;
    tx_record_counts_[index] = 0;
    return index;
}

std::optional<std::uint32_t> StreamDevice::try_dequeue_rx() {
    fades_buffer b{};
    b.queue = FADES_QUEUE_RX;
    if (control(fd_, FADESIOC_DQBUF, &b) < 0) {
        if (errno == EAGAIN) {
            return {};
        }
        fail("DQBUF RX");
    }
    rx_states_[b.index] = RxState::borrowed;
    rx_records_[b.index] = b.records;
    return b.index;
}

void StreamDevice::wait(WaitFor direction) {
    const auto flags = static_cast<unsigned>(direction);
    if (!flags || (flags & ~3U)) {
        throw std::invalid_argument("invalid wait directions");
    }
    pollfd p{fd_,
             static_cast<short>((flags & static_cast<unsigned>(WaitFor::rx) ? POLLIN : 0) |
                                (flags & static_cast<unsigned>(WaitFor::tx) ? POLLOUT : 0)),
             0};
    int result;
    do {
        result = poll(&p, 1, -1);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        fail("poll");
    }
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        fail("stream stopped or failed", EIO);
    }
}

SolverStats StreamDevice::stats() const {
    SolverStats result{};
    if (control(fd_, FADESIOC_SOLVER_STATS, &result) < 0) {
        fail("SOLVER_STATS");
    }
    return result;
}
} // namespace fades
