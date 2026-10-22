# Reference and Frozen ML Operations Tests

This directory contains a suite of fully frozen, self-contained test targets used as stable benchmarks for **power analysis** and **gate-level netlist simulation**.

## Motivation

Standard regression tests are designed to be runtime-configurable to cover various matrix dimensions. However, this flexibility poses risks for performance and power analysis:

- A change to test shapes or parameters will alter simulation cycles and power profiles, silently invalidating baseline measurements.
- Modifications to generic runner logic or compiler optimization flags will impact instruction scheduling and execution metrics.

To prevent this, tests inside this folder are locked down:

- Runner binaries hardcode shape dimensions as constants and statically allocate memory.
- Inputs are generated using deterministic, pseudo-random seeds.
- File names explicitly include the test dimensions (e.g., `float_matmul_16x48x16`).

> [!WARNING]
> Do NOT modify the code, matrix dimensions, or compilation options for existing test cases in this directory. If a new shape or configuration is needed, create new source files alongside the existing ones, rename them to reflect the dimensions, and define a new testcase in the BUILD targets.
