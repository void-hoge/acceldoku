# Host interface

The host interface provides a kernel driver for accessing `/dev/acceldoku` from Linux on the KV260 processing system (PS). It includes a C ioctl ABI, a C++ library, benchmarks, and entry points for testing on the board.

## Execution model

Input from the CPU to the FPGA is called TX; output in the opposite direction is called RX. Each direction has a buffer queue in which multiple fixed-size buffers circulate independently. Each buffer is 1 MiB and holds up to 16,384 puzzle records of 64 bytes each. DMA buffers are allocated individually and need not be contiguous as a group. Each direction can be configured with 1–4096 buffers, subject to the DMA memory available on the system.

To submit puzzles, the CPU writes them into a CPU-owned TX buffer and queues it for DMA. TX completion means the buffer has been returned for reuse; it does not mean that the puzzles have been solved. When an empty RX buffer is queued, the device writes results into it and returns it when it is full or all submitted puzzles have completed, including a partially filled buffer. Results may arrive out of order and must be matched to inputs by ID.

Completed RX data is not overwritten until the CPU requeues the buffer. If no RX buffers are available, backpressure stops processing until an RX buffer is requeued. Each direction has a 32-entry DMA ring, independent of the number of buffers.

## C ioctl API

The C API is defined in `host/driver/include/acceldoku_ioctl.h`. Open `/dev/acceldoku` and use `ioctl`, `mmap`, and `poll` to operate it. A typical sequence is:

1. Open `/dev/acceldoku`.
2. Use `ACCELDOKUIOC_DMA_INFO` to check the ABI version, buffer size, and maximum buffer count.
3. Specify the TX and RX buffer counts with `ACCELDOKUIOC_DMA_CONFIG`.
4. For each buffer in each direction, use `ACCELDOKUIOC_QUERYBUF` to get its offset and size, then map it individually with `MAP_SHARED`.
5. All RX and TX buffers are initially CPU-owned. Submit empty RX buffers and TX buffers containing input with `ACCELDOKUIOC_QBUF`.
6. Start processing with `ACCELDOKUIOC_STREAMON`.
7. Use `poll` and `ACCELDOKUIOC_DQBUF` to collect completed buffers and requeue them as needed.
8. If needed, retrieve statistics with `ACCELDOKUIOC_SOLVER_STATS` after draining all results.
9. Stop with `ACCELDOKUIOC_STREAMOFF`, unmap each buffer with `munmap`, and close the file descriptor.

See `host/tools/acceldoku_run.c` for an example.

## C++ streaming API

The C++ library is defined in `host/lib/include/acceldoku/stream_device.hpp`. Its `acceldoku::StreamDevice` wraps the C ioctl API and follows the same execution model. A typical sequence is:

1. Construct `StreamDevice(path, tx_buffers, rx_buffers)`. It opens the device, checks the ABI, configures the buffers, and maps them automatically.
2. Borrow a TX buffer with `try_dequeue_tx()`, write input records with `write_tx()`, and submit it with `enqueue_tx()`.
3. Start processing with `stream_on()`.
4. Wait for buffer completions with `wait(WaitFor::tx)`, `wait(WaitFor::rx)`, or both directions combined.
5. Obtain available buffers with `try_dequeue_tx()` or `try_dequeue_rx()`. They return `std::nullopt` if none is available.
6. Read a borrowed RX buffer with `read_rx()` and return it with `enqueue_rx()`. Write and resubmit borrowed TX buffers as needed.
7. If needed, call `stats()` after draining all results.
8. Stop with `stream_off()`. The destructor attempts to stop the device and releases the mappings and file descriptor.

Buffer indices are independent for TX and RX and may only be used while borrowed. The read-only view returned by `read_rx()` becomes invalid when the RX buffer is successfully returned or the device is destroyed. `stream_off()` discards active and pending transfers but preserves borrowed buffers and RX views.

If no records have been written or the last write contained zero records, `enqueue_tx()` returns the TX buffer without submitting it to DMA. Initial TX buffers and TX buffers returned without sending do not generate `wait()` notifications, so acquire them with `try_dequeue_tx()` before waiting. TX and RX can each be handled by a separate thread. Synchronize start, stop, and destruction with buffer operations, and wait for waiting threads to exit after stopping before restarting. See `host/tools/cpp/benchmark/benchmark.cpp` for an example.

## Data format

The device uses the following 64-byte record format, represented by `acceldoku::PuzzleRecord` in the C++ library:

| Bytes | Contents |
|-------|----------|
| 0–7 | 64-bit ID, little-endian |
| 8 | Status: 0 on input; 0 for solved, 1 for no solution, or 2 for an invalid cell value on output |
| 9–49 | 81 row-major cells packed as 4-bit values, with the earlier cell in the lower nibble; 0 denotes an empty cell |
| Upper 4 bits of 49, 50–63 | Reserved; zero on input and output |

The C++ library's `acceldoku::Puzzle` is an alias for `std::array<std::uint8_t, 81>`. Cells are in row-major order, with 0 for empty cells and 1–9 for digits.

`encode(id, puzzle)` converts a puzzle and ID into a `PuzzleRecord`. Use `record_id()` and `record_status()` to read its ID and status, and `decode_cells()` to convert its cells to a `Puzzle`.

## Entry points

Build from the repository root on the KV260:

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
```

The kernel module is generated at `build/kv260-ps/host/driver/src/acceldoku.ko`. Make `/dev/acceldoku` available before running the following commands.

Run the board tests with `ctest --preset kv260-ps` while no other program is using the device. They check DMA rings, exclusive driver ownership and mapping lifetime, poll on stop, the C++ API, and text input conversion.

```sh
# Continuous submission: TX buffer count, RX buffer count, and duration in seconds.
build/kv260-ps/host/tools/acceldoku_run sim/data/17puz49158.txt 8 64 10
build/kv260-ps/host/tools/cpp/acceldoku_benchmark sim/data/17puz49158.txt 8 64 10

# Verify against fastdoku.
build/kv260-ps/host/tools/cpp/acceldoku_verify sim/data/17puz49158.txt
```

`acceldoku_run` and `acceldoku_benchmark` repeatedly copy and submit their input, counting results without reading their contents.

`acceldoku_verify` first solves each puzzle with fastdoku and checks that it has a unique solution or no solution, then compares the board's output checksum against fastdoku's. It requires Rust / Cargo 1.83 or later and the fastdoku submodule. To use a prebuilt library, pass `-DACCELDOKU_FASTDOKU_ARCHIVE=/absolute/path` to CMake.

## Hardware counters

Acceldoku provides hardware counters for performance analysis. The type is `struct acceldoku_solver_stats` in C and its alias `acceldoku::SolverStats` in C++. Use `ACCELDOKUIOC_SOLVER_STATS` in C or `StreamDevice::stats()` in C++ to obtain a snapshot with named fields.

Statistics are initialized by `STREAMON` and reset by `STREAMOFF`. Retrieve final values after draining all results and before stopping. The core and DMA counters are sampled at different times, so stage counts do not necessarily agree while processing is in progress.

| Field | Contents |
|-------|----------|
| `accepted` / `emitted` | Input records accepted / output records emitted by hardware |
| `dispatched` / `collected` | Records dispatched to / collected from the cores |
| `processing_cycles` | Core cycles from the first dispatch to the snapshot, including time after draining |
| `dispatch_wait_cycles` | Cycles when a preprocessed puzzle is ready for dispatch but no core can accept it |
| `core_dispatched[6]` / `core_collected[6]` | Records dispatched to / collected from each core |
| `core_busy_cycles[6]` | Busy cycles per core |
| `core_output_wait_cycles[6]` | Output wait cycles per core |
| `core_input_wait_cycles[6]` | Cycles per core that is enabled, idle, and has no request |
| `core_queue_peak[6]` | Peak internal deque occupancy, including unprocessed puzzles and pending branches |
| `core_branches[6]` | Branch decisions per core |
| `core_max_pending[6]` | Maximum number of unexplored pending branches, not search depth |
| `core_evaluated[6]` | States reaching a branch, contradiction, or solution; completion by contradiction counts once |

After all results are drained, `core_branches / core_collected` gives the average number of branches per puzzle, and `core_busy_cycles / core_evaluated` gives the average busy cycles per state. The latter includes memory and output waits. A ratio cannot be calculated when its denominator is zero; the benchmark and verification tools display `N/A` in that case.
