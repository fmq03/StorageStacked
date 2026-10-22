#!/bin/bash
set -e

TRACE_ARG=""
RUN_TIME=10

while [[ $# -gt 0 ]]; do
  case $1 in
    --trace)
      TRACE_ARG="--trace isp_cam_trace.fst"
      echo "Tracing enabled. Waveform will be saved to isp_cam_trace.fst"
      shift
      ;;
    --run_time)
      RUN_TIME="$2"
      echo "Run time set to ${RUN_TIME}s"
      shift 2
      ;;
    *)
      echo "Unknown argument: $1"
      exit 1
      ;;
  esac
done

echo "Building ISP Camera software test..."
bazel build //fpga:isp_cam_test
ELF_PATH=$(bazel cquery //fpga:isp_cam_test --output=files | grep ".elf$")
EXEC_ROOT=$(bazel info execution_root)
cp -f "${EXEC_ROOT}/${ELF_PATH}" ./isp_cam_test.elf

echo "Generating test pattern..."
python3 fpga/ip/ispyocto/dv/camera_model/gen_test_pattern.py
mv test_pattern_320x240.raw grey_bars_320x240.raw

echo "Building simulation model..."
bazel build //fpga:build_chip_verilator

echo "Running simulation..."
bazel build //utils/coralnpu_soc_loader:run_simulation
./bazel-bin/utils/coralnpu_soc_loader/run_simulation --elf_file ./isp_cam_test.elf --run_time $RUN_TIME $TRACE_ARG

echo "Simulation Finished."
