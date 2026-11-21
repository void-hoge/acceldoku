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

### PS clocks and boot firmware

The specified operating rates assume a PS IOPLL output of approximately 1.5 GHz: the core clock is IOPLL / 13 and the DMA clock is IOPLL / 8.
The Device Tree overlay requests these rates, but the actual rates depend on the clock configuration established by the boot firmware.
With a 1 GHz IOPLL, the same requests can instead produce approximately 111.111 MHz and 166.667 MHz.

After loading Acceldoku and its driver, check the actual clocks on the board:

```sh
grep -E ' iopll +| pl[01]_ref +' /sys/kernel/debug/clk/clk_summary
dmesg | grep 'actual clocks'
```

One way to configure the required PS clocks is to install K26 Starter Kit BootFW 1.04, based on AMD's Yocto 2024.2 configuration.
Its standard PS initialization sets IOPLL to approximately 1.5 GHz and configures the shared PS peripheral clock dividers.
The following procedure was used on a KV260 revB running Ubuntu 24.04; it does not require rebuilding the Acceldoku bitstream.

On the development machine, install the [Yocto host packages](https://docs.yoctoproject.org/brief-yoctoprojectqs/index.html#build-host-packages) and `repo`, then build the boot firmware:

```sh
mkdir kria-yocto
cd kria-yocto
repo init -u https://github.com/Xilinx/yocto-manifests.git -b rel-v2024.2 --depth=1
repo sync -c -j4 --no-tags
source ./setupsdk
MACHINE=k26-smk bitbake xilinx-bootbin
```

The resulting `BOOT-k26-smk-*.bin` is placed in `build/tmp/deploy/images/k26-smk/`.
See AMD's [BOOT.BIN generation guide](https://xilinx.github.io/kria-apps-docs/bootfw/build/html/docs/bootfw_boot.bin_generation.html) for details.

Transfer the generated file to the board and run the following as root, replacing the path with the actual file location.
Keep serial console and power-cycle access available during the update.

```sh
xmutil bootfw_status
xmutil bootfw_update -i /path/to/BOOT-k26-smk.bin
xmutil bootfw_status
systemctl reboot
```

The updater writes the inactive A/B slot and requests a trial boot.
After Linux starts, check `xmutil bootfw_status` and the IOPLL and peripheral clocks, then load Acceldoku and verify its actual clock rates and operation.
Run the board tests as described in the [main README](../../README.md#tests).
Once the new firmware is verified, mark the trial image bootable **before the next reboot**:

```sh
xmutil bootfw_update -v
xmutil bootfw_status
```

If the trial image is not validated, the next restart falls back to the previously bootable image.
If boot stops at the U-Boot prompt, `run bootcmd` continues the normal boot.
For recovery and the full update procedure, see AMD's [Kria SOM Boot Firmware Update guide](https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/3020685316/Kria+SOM+Boot+Firmware+Update).

On the tested board, BootFW 1.04 retained the 1.5 GHz IOPLL across a Linux software reboot; a cold boot after power-off was not tested.
The first trial boot required `run bootcmd`, while the subsequent software reboot completed automatically.

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
