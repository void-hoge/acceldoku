# acceldoku rename validation

- KV260 revB, six solver cores; Vivado 2026.1, implementation and firmware package checks passed.
- Actual clocks reported by the driver: core 115,384,615 Hz, DMA 187,499,999 Hz.
- Linux 6.8.0-1036-xilinx, ABI v9, 1 MiB DMA slots; TX/RX 8/8 buffers.
- Host: all 21 CTest tests passed, including RTL lint, simulation and formal checks.
- Board: ABI and poll/STREAMOFF checks passed. Streaming checked 114,707 accumulated
  records and 422,835 mixed records, including backpressure, buffer reuse and restart.
- Loaded firmware SHA-256: bitstream `c68ba0d697bb13c5ca874ef9e53e6c84123011646c96b2404cc04a17985ede6c`,
  device tree `fe5bf4bc877648d280a02c9c42687bfbb97e4eeb15b4030115e7773be6425fef`.
