# RTL and reference implementation verification

CPU reference comparisons use fastdoku through the data conversion and validation in `reference/sudoku.hpp`.
Directed tests for the core and deduction engine verify contradiction detection and search with arbitrary candidate masks.

## Running tests

The XPM test uses Vivado 2026.1 xsim.
The number of cores in the integration test can be changed with, for example, `-DACCELDOKU_CORES=1` (default: 6).

The XPM test is not registered if `xvlog` is absent from `PATH` and `$XILINX_VIVADO/bin`.
It can be specified explicitly with `-DXVLOG_EXECUTABLE=/tools/Xilinx/2026.1/Vivado/bin/xvlog`.
The puzzle corpus can be changed with `-DACCELDOKU_DATASET=/absolute/path/to/dataset.txt`.

## Tests

| Directory / label  | Tests                                                                                      | Coverage                                                                                                                                                                                 |
|--------------------|--------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `deduction_engine` | `deduction-engine`, `formal-deduction-engine`, `formal-row-changes`, `formal-locked-masks` | Singles, Locked Candidates, deduction steps, and equivalence of deduction, row changes, and shared masks to independent specifications                                                   |
| `branch_generator` | `branch-generator`, `formal-branch-generator`                                              | Directed candidate-splitting tests and equivalence to an independent sequential specification                                                                                            |
| `acceldoku_core`   | `acceldoku-core`, `synth-core`, `lint-acceldoku_core`                                      | Single-core CPU comparisons, search, backtracking, ordering, stalls, queue wraparound, reset, cycle counts, structural synthesis, and lint                                               |
| `acceldoku`        | `acceldoku`, `lint-acceldoku`                                                              | Integration of `ACCELDOKU_CORES` cores with dispatch, ID management, collection, and asynchronous queues; lint                                                                           |
| `acceldoku_axi`    | `axi`, `lint-acceldoku_axi`, `lint-acceldoku_kv260`                                        | Independent AXI-Lite AW/W acceptance, WSTRB, response stalls, configuration protection, snapshots, zero counters for unimplemented cores, and lint of the AXI boundary and board wrapper |
| `async_queue`      | `async-queue-xpm`                                                                          | Full and count signals, wraparound, stalls, and reset with the Xilinx asynchronous FIFO model (Vivado xsim)                                                                              |
| `reference`        | `reference-dataset`                                                                        | Basic CPU reference and input format tests, and existence and uniqueness of solutions across the corpus                                                                                  |

The core, integration, and CPU reference tests use 49,158 puzzles by default; the core test also uses 4,096 stress inputs.
The integration test does not cover the entire board, including AXI-Lite control, PS, and DMA.
It also tests unsolvable puzzles, invalid cell values, full queues, output stalls, and resets with partial inputs or uncollected outputs.
The Verilator queue is a 64-entry abstract model and does not reproduce the XPM FWFT stage or initialization latency cycle by cycle.
The properties of the XPM-based queue are therefore also tested with xsim.

The `lint`, `synth`, `formal`, and `fifo-test` build targets can also be used to run the corresponding checks.

## Output and cycle regression

Artifacts are placed in `sim/<test-directory>/` under the selected build directory.
The core test generates `results.csv` and `results.csv.totals.json` in `sim/acceldoku_core/`.
The CSV records completion intervals for individual puzzles; the totals file records overall values through collection of the final output.

```sh
ctest --preset dev -L '^acceldoku_core$'
cmake --build --preset dev --target core-summary

cmake --preset dev -DACCELDOKU_BASELINE=/absolute/path/to/saved/results.csv
cmake --build --preset dev --target regression
```

`core-summary` generates `summary.json` from the core test CSV.
`regression` is enabled when a baseline is specified and checks for an exact match of the CSV and totals for 49,158 puzzles plus 4,096 stress inputs.
Save both files for the baseline.

## Puzzle corpora

Each puzzle occupies one line of 81 characters, with cells in row-major order. Empty cells are represented by `.`, and clues by `1`-`9`.

| File                        | Puzzles | Purpose                                  |
|-----------------------------|--------:|------------------------------------------|
| `17puz49158.txt`            |  49,158 | Default input for tests and measurements |
| `HardestDatabase110626.txt` |     375 | Hard puzzle corpus                       |
