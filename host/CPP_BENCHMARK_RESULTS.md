# C++ streaming benchmark

- KV260, kernel 6.8.0-1036-xilinx; loaded `fades` bitstream SHA-256 `1bc89fe1f9f3083061d277276cc28b6e7ceb0c5a59a733e2319df5677570fba8`.
- PS clock readings: pl0 187499999 Hz, pl1 115384615 Hz. The loaded bitstream's internal clock configuration was not independently identified.
- ABI v8; TX/RX 8/64 buffers, 1 MiB each; GCC `-O2`.
- Input: one already-solved 64-byte puzzle repeated with unique IDs; SHA-256 `616a1481e485c607b14d6f3534f774a3737745c8a4bca99f3ed30ab3534e5a89`.
- Five-second timed runs including input copying and ID assignment, queueing, DMA, solving and final drain. Outputs were counted without reading records. C++ additionally copies through a temporary batch; startup timing differs between implementations.

| Run | C Mproblem/s | C++ Mproblem/s |
| --- | ---: | ---: |
| 1 | 38.554115 | 14.2392 |
| 2 | 38.523749 | 14.4917 |

## 17-hint corpus, NEON copy with ID assignment

- Same board, bitstream, clock readings, buffer counts, compiler settings and five-second timed runs as above.
- Input: `sim/data/17puz49158.txt`, SHA-256 `58ef7d83e8cbac32495161f9745877fef82f5e8b3fe58e3cad4eb3fc004a81b9`.
- Both versions pre-encode the corpus and prequeue TX before timing; the timed section includes STREAMON, continuous copying and ID assignment, DMA, solving and final drain. Output records are not read.
- C reads the same corpus converted to raw records; C++ reads the text and converts it before timing.

| Run | C Mproblem/s | C++ Mproblem/s |
| --- | ---: | ---: |
| 1 | 27.610133 | 27.6096 |
| 2 | 27.610053 | 27.6099 |

After the buffer ownership and start/stop changes, the same C++ benchmark, bitstream,
clock readings and conditions produced 27.6078, 27.6078 and 27.6080 Mproblem/s.
RX buffers are now queued during STREAMON within the timed interval.

After replacing proxy operations with explicit index-based operations, the same
benchmark and conditions produced 27.6088 and 27.6094 Mproblem/s.

After changing `enqueue_tx` to use the last written count, a one-second functional
run under the same conditions produced 27.5502 Mproblem/s (27,672,576 records,
1.00444 s); it includes stream start, continuous input and final drain.

## Validation after PuzzleRecord array alias

KV260, kernel 6.8.0-1036-xilinx, ABI v8, loaded `fades` bitstream SHA-256
`1bc89fe1f9f3083061d277276cc28b6e7ceb0c5a59a733e2319df5677570fba8`;
PS pl0 187499999 Hz, pl1 115384615 Hz. The bitstream's internal clock
configuration was not independently identified. GCC C++20 `-O2`, TX/RX 8/64
buffers of 1 MiB each. Input: 49,158 17-hint puzzles, file SHA-256
`58ef7d83e8cbac32495161f9745877fef82f5e8b3fe58e3cad4eb3fc004a81b9`.
The corpus is encoded and initial TX buffers are queued before timing; timed
work includes STREAMON, continuous input copies and unique-ID assignment,
ioctls, DMA, solving, RX return and final drain. Output records are counted
without reading their contents.

| Run | Records | Elapsed s | Mproblem/s |
| --- | ---: | ---: | ---: |
| 1 | 138166272 | 5.00435 | 27.6092 |
| 2 | 138166272 | 5.00427 | 27.6097 |
| 3 | 138166272 | 5.00427 | 27.6097 |

Functional checks: C++ conversion and streaming smoke test; `check_abi.py`;
`check_poll_stop.py`; `check_stream.py --tx 1 --rx 1`. All passed.
