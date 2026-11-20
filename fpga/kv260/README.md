# KV260 board implementation

## FPGA design

- The design targets the K26 on the KV260 revB (`xck26-sfvc784-2LV-c`) and uses the official SOM 1.4 preset.
- AXI DMA operates in Scatter-Gather mode. The memory and stream data widths are 128 bits, the maximum burst length is 64 beats, the address width is 40 bits, and the length field is 23 bits. Host packets are at most 1 MiB.
- PS port HP0 is used for MM2S and Scatter-Gather descriptors, and HP1 for S2MM. DMA is non-coherent, so the Device Tree does not specify `dma-coherent`.
- The DMA AXI-Lite address is `0xa0000000`, and the solver AXI-Lite address is `0xa0010000`.
- The MM2S and S2MM interrupts connect to `pl_ps_irq0[0]` and `pl_ps_irq0[1]`, respectively. They correspond to GIC SPI 89 and 90 and are both active high.
- For fan control, PS TTC0 wave[2] connects through EMIO to `fan_en_b` (A12). The pin is constrained to LVCMOS33, 4 mA drive, and SLOW slew rate.

## FPGA build

### Build configuration

Set up the Vivado environment before building so that the build can access the XilinxBoardStore bundled with Vivado.

`ACCELDOKU_CORES` specifies the number of solver cores, from 1 to 6; the default is 6.
The same implementation is used for a single core. The ABI always reserves counter space for six cores, and counters for unimplemented cores read as zero.

Clock settings are managed in `CMakeLists.txt` and applied to the PS configuration, timing constraints, and DTBO.
The operating frequencies are 115,384,615 Hz for the cores and 187,500,000 Hz for DMA.
The constrained periods are **8.666 ns** and **5.333 ns**, respectively, rounded down to the nearest 1 ps. They apply to both top-level synthesis and place and route. XPM CDC constraints are also evaluated using these periods.

`VIVADO_EXECUTABLE` and `DTC_EXECUTABLE` can be used to specify the tool paths.
`ACCELDOKU_VIVADO_JOBS` specifies Vivado's internal parallelism independently of CMake's `--parallel` option.

### Build workflow

The block design (BD) is synthesized as a whole rather than using out-of-context synthesis for each IP, so clock constraints also apply to the solver during synthesis.
After placement, physical optimization, and routing using the `Performance_ExplorePostRoutePhysOpt` strategy, the build runs `phys_opt_design -directive AggressiveExplore` before generating the bitstream.

| Target | Operation |
|--------|-----------|
| `kv260-bd` | Generate and validate the BD only (`bd/`) |
| `kv260-synth` | Synthesize the design (`synth/`) |
| `kv260-bitstream` | Place and route, generate the bitstream, and check the output (`implementation/`) |
| `kv260-package` | Generate the bitstream if needed and package it with the DTBO as firmware |

Each stage uses a separate Vivado project and is regenerated when sources, constraints, or the core count change.
The output checks cover setup, hold, pulse width, CDC, bus skew, and the fan pin. They create `build_complete` on success; firmware packaging requires this file.

### Artifacts

```text
build/kv260-pl/fpga/kv260/
  firmware/acceldoku/
    acceldoku.bit.bin
    acceldoku.dtbo
    shell.json
  implementation/
    timing_summary.rpt    Setup, hold, pulse width, and unconstrained paths
    utilization.rpt       LUT, FF, CLB, BRAM, and URAM utilization
    cdc.rpt               CDC report
    bus_skew.rpt          Held-bus and FIFO pointer skew
    io.rpt, drc.rpt       I/O and DRC reports
    verified_routed.dcp   Checked checkpoint
    acceldoku.xsa         Hardware definition
    project/              Vivado project and detailed runme.log
```

## Control registers

| Offset | Description |
|--------|-------------|
| 0x000 | Bit 0: reset (initial value 1); bit 1: enable |
| 0x004 | Output slot capacity N; configurable only during reset |
| 0x008 | Add to the registered submission count, including pending sends, before starting TX. Reads return the lower 32 bits of the cumulative count. |
| 0x00c | Bit 0: sticky framing error; bit 1: snapshot pending |
| 0x010 | Write 1 to bit 0 to request a counter snapshot |
| 0x014 | Identity `0x46414405` (statistics register layout v9) |
| 0x018 | Upper 32 bits of the cumulative registered submission count |
| 0x100–0x2df | 60 64-bit counters, lower 32 bits first |

See the comments in `host/driver/include/acceldoku_ioctl.h` for the counter order.
Counters in different clock domains are held and sampled using handshakes; counts are compared after all results have been drained.
