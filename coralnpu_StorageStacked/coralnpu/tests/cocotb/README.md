# CoralNPU Cocotb Verification Environment

This directory contains the Cocotb verification environment and tests for the CoralNPU processor subsystems.

## Running Tests

Cocotb tests are run using Bazel. The tests are grouped into suites defined in the `BUILD` file.

To run the standard test suite:

```bash
bazel test //tests/cocotb:core_mini_axi_sim_cocotb
```

---

## VCS Simulation Flows & Parity Guard

We support two distinct flows for running VCS simulations with Cocotb:

1. **Regular Flow (`vcs_cocotb_test`)**: Compiles and runs the simulation in a single target (used for standard RTL tests).
2. **Split Flow (`vcs_simulation_split_test`)**: Splits compilation and execution into separate targets (required for gate-level power analysis where waveforms must be cached build outputs).

To prevent these two flows from drifting out of sync (which would lead to broken netlist tests), we employ a multi-layered verification system:

### 1. Load-Time Smoke Test

We define a smoke test target `vcs_flow_smoke_test` at the end of [tests/cocotb/BUILD](BUILD).
This target instantiates both the regular and split flows with a comprehensive set of common and test-specific arguments. Because Bazel evaluates macro targets during package loading, any signature or forwarding mismatch will immediately break package loading (e.g. during a `bazel query` or build command), alert the developer, and prevent the build.
These smoke targets are tagged with `"manual"` so they are never executed during wildcard test runs (`bazel test //...`).

### 2. AST Macro Signature Checker (Presubmit)

To proactively prevent divergence (e.g., if a developer adds a new argument to one flow but forgets to add it to the smoke test), we use a Python static analysis checker:
[utils/check_macro_signatures.py](../../utils/check_macro_signatures.py)

This tool parses the Starlark macro definitions in [rules/coco_tb.bzl](../../rules/coco_tb.bzl) using Python's `ast` library and compares their formal signatures and popped `kwargs` keys.

It is integrated into the pre-upload hooks ([PREUPLOAD.cfg](../../PREUPLOAD.cfg)) and will automatically block commits that introduce unsynchronized parameters.
