#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <acceldoku_ioctl.h>
#include <optional>
#include <span>
#include <vector>

namespace acceldoku {

/**
 * @brief A 64-byte hardware input or output record.
 * @details Bytes 0..7 contain a little-endian ID, byte 8 the status, and bytes
 * 9..49 the 81 packed cells. The upper nibble of byte 49 and bytes 50..63 are reserved.
 */
using PuzzleRecord = std::array<std::uint8_t, 64>;

static_assert(sizeof(PuzzleRecord) == 64);

/** @brief 81 row-major cells: 0 for empty, 1..9 for digits. */
using Puzzle = std::array<std::uint8_t, 81>;

/** @brief Named cumulative statistics from the ioctl ABI. */
using SolverStats = ::acceldoku_solver_stats;

/**
 * @brief Encode a puzzle in the hardware input format.
 * @param id Record ID.
 * @param puzzle Puzzle with cells in 0..9.
 * @return Record with zero status and reserved bits.
 * @throws std::invalid_argument If a cell exceeds 9.
 */
PuzzleRecord encode(std::uint64_t id, const Puzzle &puzzle);

/**
 * @brief Decode the little-endian record ID.
 * @param record Input or output record.
 * @return Record ID.
 */
std::uint64_t record_id(const PuzzleRecord &record) noexcept;

/**
 * @brief Read the output status.
 * @param record Output record.
 * @return 0 for solved, 1 for unsolvable, 2 for an invalid cell.
 */
std::uint8_t record_status(const PuzzleRecord &record) noexcept;

/**
 * @brief Unpack the 4-bit cells in row-major order.
 * @param record Input or output record.
 * @return Decoded puzzle; each cell retains its original nibble value.
 */
Puzzle decode_cells(const PuzzleRecord &record) noexcept;

/** @brief Completion directions to wait for; combine with operator|. */
enum class WaitFor : unsigned { tx = 1, rx = 2 };

/**
 * @brief Combine wait directions.
 * @param left First direction.
 * @param right Second direction.
 * @return Combined directions.
 */
constexpr WaitFor operator|(WaitFor left, WaitFor right) noexcept {
    return static_cast<WaitFor>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}

/**
 * @brief Open a streaming device exclusively and manage DMA buffer mappings.
 * @details TX and RX may each be handled by one thread. Synchronize start, stop,
 * and destruction with buffer operations. After stopping, wait for polling threads
 * to observe the stop and exit before restarting. Buffer indices are independent
 * per direction and accessible only while borrowed. The caller must prevent reuse
 * after return and concurrent access to a borrowed buffer. System call failures throw
 * std::system_error, out-of-range indices throw std::out_of_range, and operations on
 * buffers that are not borrowed throw std::logic_error.
 */
class StreamDevice {
  public:
    /**
     * @brief Open the device, configure and map TX/RX buffers.
     * @param path Device path.
     * @param tx_buffers TX buffer count, from 1 to the device limit.
     * @param rx_buffers RX buffer count, from 1 to the device limit.
     * @throws std::invalid_argument If a buffer count is invalid.
     * @throws std::runtime_error If the ABI or mapping information is incompatible.
     */
    StreamDevice(const char *path, std::uint32_t tx_buffers, std::uint32_t rx_buffers);
    /** @brief Attempt to stop the device, then release mappings and file descriptor. */
    ~StreamDevice();
    StreamDevice(const StreamDevice &) = delete;
    StreamDevice &operator=(const StreamDevice &) = delete;

    /**
     * @brief Get the record capacity of a TX or RX buffer.
     * @return Maximum number of records per buffer.
     */
    static constexpr std::size_t batch_capacity() noexcept {
        return ACCELDOKU_RECORDS_PER_SLOT;
    }

    /**
     * @brief Enqueue available RX buffers and start streaming.
     * @details Borrowed RX buffers remain CPU-owned until returned.
     * @throws std::logic_error If the stream is already running.
     */
    void stream_on();

    /**
     * @brief Stop streaming and discard pending transfers.
     * @details Borrowed buffers remain borrowed and borrowed RX contents remain readable.
     */
    void stream_off();

    /**
     * @brief Borrow an available TX buffer without waiting.
     * @return Buffer index, or std::nullopt if none is available.
     */
    std::optional<std::uint32_t> try_dequeue_tx();

    /**
     * @brief Borrow a completed RX buffer without waiting.
     * @return Buffer index, or std::nullopt if none has completed.
     */
    std::optional<std::uint32_t> try_dequeue_rx();

    /**
     * @brief Overwrite a borrowed TX buffer and set its record count.
     * @param index Borrowed TX buffer index.
     * @param records Records to copy; an empty span sets the count to zero.
     * @throws std::length_error If the record count exceeds batch_capacity().
     */
    void write_tx(std::uint32_t index, std::span<const PuzzleRecord> records);

    /**
     * @brief Overwrite a TX buffer, assigning consecutive IDs during the copy.
     * @param index Borrowed TX buffer index.
     * @param records Records to copy.
     * @param first_id ID assigned to the first record.
     * @throws std::length_error If the record count exceeds batch_capacity().
     * @throws std::overflow_error If the assigned IDs would overflow.
     */
    void write_tx(std::uint32_t index, std::span<const PuzzleRecord> records,
                  std::uint64_t first_id);

    /**
     * @brief Get the received record count.
     * @param index Borrowed RX buffer index.
     * @return Number of received records.
     */
    std::size_t rx_size(std::uint32_t index) const;

    /**
     * @brief View the received records in a borrowed RX buffer without copying.
     * @param index Borrowed RX buffer index.
     * @return Read-only view valid until successful enqueue_rx(index) or destruction.
     * @details The view remains valid across stream_off() and stream_on() while borrowed.
     * The caller must not access it after returning the buffer.
     */
    std::span<const PuzzleRecord> read_rx(std::uint32_t index) const;

    /**
     * @brief Submit the last successfully written TX records, or return an empty buffer.
     * @param index Borrowed TX buffer index.
     * @details If no records were written or the last write was empty, return the buffer
     * without submitting it to DMA. Such a return does not generate a poll notification.
     * @post On success the buffer is no longer borrowed; on failure it remains borrowed.
     */
    void enqueue_tx(std::uint32_t index);

    /**
     * @brief Return an RX buffer, enqueueing it immediately if streaming.
     * @param index Borrowed RX buffer index.
     * @post On success the buffer is no longer borrowed; on failure it remains borrowed.
     */
    void enqueue_rx(std::uint32_t index);

    /**
     * @brief Wait for a kernel completion notification in any selected direction.
     * @param direction TX, RX, or their bitwise combination.
     * @details Initial TX and unused returned TX are not reported by poll; acquire them before
     * waiting.
     * @throws std::invalid_argument If the directions are empty or invalid.
     * @throws std::system_error On poll error, stream stop, or device error.
     */
    void wait(WaitFor direction);

    /**
     * @brief Sample cumulative counters.
     * @return Named solver statistics snapshot.
     */
    SolverStats stats() const;

  private:
    /**
     * @brief Validate a TX buffer index and its borrowed state.
     * @param index TX buffer index to validate.
     * @throws std::out_of_range If the index is out of range.
     * @throws std::logic_error If the buffer is not borrowed.
     */
    void check_tx(std::uint32_t index) const;

    /**
     * @brief Validate an RX buffer index and its borrowed state.
     * @param index RX buffer index to validate.
     * @throws std::out_of_range If the index is out of range.
     * @throws std::logic_error If the buffer is not borrowed.
     */
    void check_rx(std::uint32_t index) const;

    /**
     * @brief Submit a buffer using the QBUF ioctl.
     * @param direction TX or RX queue number.
     * @param index Buffer index to submit.
     * @param count TX record count, or zero for RX.
     * @throws std::system_error If the ioctl fails.
     */
    void enqueue(unsigned direction, unsigned index, unsigned count);

    /** @brief Attempt to stop, then release mappings and the file descriptor. */
    void cleanup() noexcept;

    int fd_ = -1; ///< Device file descriptor, or -1 before open.

    /** @brief Stream start and stop state. */
    enum class StreamState {
        configured, ///< Configured but not yet started.
        running,    ///< Streaming.
        stopped     ///< Stopped.
    };

    /** @brief Library-side RX buffer state. */
    enum class RxState {
        available, ///< CPU-owned and available for submission on start.
        queued,    ///< Submitted to the driver, including completed buffers.
        borrowed   ///< Completion dequeued and buffer borrowed by the caller.
    };

    std::vector<void *>
        buffer_mappings_[2]; ///< Individual mappings indexed by direction and buffer.
    std::vector<std::uint32_t>
        free_tx_;                   ///< Initially free or returned unsent CPU-owned TX indices.
    std::vector<bool> tx_borrowed_; ///< Caller-borrowed state per TX buffer.
    std::vector<std::size_t> tx_record_counts_; ///< Last successfully written count per TX buffer.
    std::vector<RxState> rx_states_;            ///< Submission and borrowed state per RX buffer.
    std::vector<std::size_t> rx_records_;       ///< Received record count per borrowed RX buffer.
    StreamState stream_state_ = StreamState::configured; ///< Current stream state.
};
} // namespace acceldoku
