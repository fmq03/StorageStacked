#!/usr/bin/env python3
# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import logging
import os
import signal
import socket
import subprocess
import sys
import threading
import time

from bazel_tools.tools.python.runfiles import runfiles


def find_free_port():
    """Finds a free TCP port on the system."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("", 0))
        return s.getsockname()[1]


def stream_reader(pipe, prefix, ready_event=None, ready_line=None):
    """Reads and prints lines from a subprocess pipe."""
    try:
        for line in iter(pipe.readline, ""):
            logging.warning(f"[{prefix}] {line.strip()}")
            if ready_event and ready_line and ready_line in line:
                ready_event.set()
    finally:
        pipe.close()


def main():
    """The main entry point for the script."""
    parser = argparse.ArgumentParser(
        description="Run the CoralNPU SoC simulation and load an ELF binary."
    )
    parser.add_argument(
        "--elf_file",
        help=
        "Path to the ELF binary to load. If not provided, simulation runs without loading code."
    )
    parser.add_argument(
        "--command",
        help=
        "Optional: A command string to run once the simulation is ready. If provided, overrides --elf_file."
    )
    parser.add_argument(
        "--trace",
        nargs="?",
        const="sim_trace.fst",
        help=
        "Optional: Path to save a waveform trace file (.fst). Defaults to sim_trace.fst if no path is provided."
    )
    parser.add_argument(
        "--trace_file",
        help=
        "Deprecated: use --trace instead. Path to save a waveform trace file (.fst)."
    )
    parser.add_argument(
        "--run_time",
        type=int,
        default=10,
        help="Optional: Time in seconds to run simulation after loading."
    )
    parser.add_argument(
        "--spi_port", type=int, help="Optional: TCP port for SPI DPI server."
    )
    args = parser.parse_args()

    r = runfiles.Create()

    # Ensure required data files are in Current working directory
    raw_image_path = r.Rlocation(
        "coralnpu_hw/fpga/ip/ispyocto/grey_bars_320x240.raw"
    )
    linked_raw = False
    if raw_image_path and os.path.exists(raw_image_path):
        if not os.path.exists("grey_bars_320x240.raw"):
            try:
                os.symlink(raw_image_path, "grey_bars_320x240.raw")
                linked_raw = True
                logging.warning(
                    "RUNNER: Linked grey_bars_320x240.raw to current directory."
                )
            except OSError:
                pass
    # The genrule copies the binary to a predictable path.
    sim_bin_path = r.Rlocation("coralnpu_hw/fpga/Vchip_verilator")
    if not sim_bin_path or not os.path.exists(sim_bin_path):
        # As a fallback, let's try the longer path. This can happen if the
        # genrule is not correctly configured.
        long_path = "coralnpu_hw/fpga/build_chip_verilator/com.google.coralnpu_fpga_chip_verilator_0.1/sim-verilator/Vchip_verilator"
        sim_bin_path = r.Rlocation(long_path)
        if not sim_bin_path or not os.path.exists(sim_bin_path):
            raise FileNotFoundError(
                f"Could not find simulator binary in runfiles at default or fallback paths."
            )

    if args.spi_port:
        port = args.spi_port
    else:
        port = find_free_port()
    logging.warning(f"RUNNER: Using TCP port: {port}")

    sim_env = os.environ.copy()
    sim_env["SPI_DPI_PORT"] = str(port)

    sim_proc = None
    loader_proc = None
    threads = []

    try:
        sim_cmd = [sim_bin_path]
        trace_file = args.trace or args.trace_file
        if trace_file:
            # If relative, save it to the workspace directory to ensure it doesn't get buried in runfiles.
            if not os.path.isabs(
                    trace_file) and "BUILD_WORKSPACE_DIRECTORY" in os.environ:
                trace_file = os.path.join(
                    os.environ["BUILD_WORKSPACE_DIRECTORY"], trace_file
                )
            sim_cmd.append(f"--trace={trace_file}")
            logging.warning(
                f"RUNNER: Tracing enabled, waveform will be saved to {trace_file}"
            )

        logging.warning(f"RUNNER: Starting simulation: {' '.join(sim_cmd)}")
        sim_proc = subprocess.Popen(
            sim_cmd,
            env=sim_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        sim_ready_event = threading.Event()

        # Start threads to monitor simulator output
        threads.append(
            threading.Thread(
                target=stream_reader,
                args=(
                    sim_proc.stdout, "SIM", sim_ready_event,
                    f"DPI: Server listening on port {port}"
                )
            )
        )
        threads.append(
            threading.Thread(
                target=stream_reader, args=(sim_proc.stderr, "SIM_ERR")
            )
        )
        for t in threads:
            t.start()

        logging.warning("RUNNER: Waiting for simulation to be ready...")
        if not sim_ready_event.wait(timeout=60):
            raise RuntimeError(
                "Timeout waiting for simulator to become ready."
            )
        logging.warning("RUNNER: Simulation is ready.")

        if args.command:
            import shlex
            cmd = shlex.split(args.command)
            # Try to resolve first element as a runfile if it looks like a workspace path
            if cmd[0].startswith("coralnpu_hw/"):
                resolved = r.Rlocation(cmd[0])
                if resolved:
                    cmd[0] = resolved

            logging.warning(
                f"RUNNER: Starting custom command: {' '.join(cmd)}"
            )
            loader_proc = subprocess.Popen(
                cmd,
                env=sim_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            # Start threads for command output
            loader_stdout_thread = threading.Thread(
                target=stream_reader, args=(loader_proc.stdout, "CMD")
            )
            loader_stderr_thread = threading.Thread(
                target=stream_reader, args=(loader_proc.stderr, "CMD_ERR")
            )
            loader_stdout_thread.start()
            loader_stderr_thread.start()
            threads.extend([loader_stdout_thread, loader_stderr_thread])

            # Wait for command to complete
            loader_proc.wait(timeout=300)
            logging.warning(
                f"RUNNER: Command finished with status {loader_proc.returncode}. Running simulation for {args.run_time} seconds..."
            )

        elif args.elf_file:
            loader_script_path = r.Rlocation(
                "coralnpu_hw/utils/coralnpu_soc_loader/loader"
            )
            if not loader_script_path or not os.path.exists(loader_script_path
                                                            ):
                raise FileNotFoundError(
                    "Could not find loader binary in runfiles."
                )

            logging.warning(
                f"RUNNER: Starting ELF loader: {loader_script_path}"
            )
            loader_proc = subprocess.Popen(
                [loader_script_path, args.elf_file],
                env=sim_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            # Start threads for loader output
            loader_stdout_thread = threading.Thread(
                target=stream_reader, args=(loader_proc.stdout, "LOADER")
            )
            loader_stderr_thread = threading.Thread(
                target=stream_reader, args=(loader_proc.stderr, "LOADER_ERR")
            )
            loader_stdout_thread.start()
            loader_stderr_thread.start()
            threads.extend([loader_stdout_thread, loader_stderr_thread])

            # Wait for processes to complete
            loader_proc.wait(timeout=300)
            logging.warning(
                f"RUNNER: Loader finished. Running simulation for {args.run_time} seconds..."
            )
        else:
            logging.warning(
                f"RUNNER: No ELF file provided. Running simulation for {args.run_time} seconds..."
            )

        time.sleep(args.run_time)

        logging.warning(
            "RUNNER: Sending SIGINT to simulator for graceful shutdown..."
        )
        sim_proc.send_signal(signal.SIGINT)
        sim_proc.wait(timeout=10)
        logging.warning("RUNNER: Simulation finished.")

    except (subprocess.TimeoutExpired, RuntimeError) as e:
        logging.error(f"RUNNER: An error occurred: {e}", file=sys.stderr)
        if sim_proc:
            sim_proc.kill()
        if loader_proc:
            loader_proc.kill()
        sys.exit(1)
    finally:
        for t in threads:
            t.join()
        if 'linked_raw' in locals() and linked_raw:
            try:
                os.remove("grey_bars_320x240.raw")
                logging.warning("RUNNER: Cleaned up grey_bars_320x240.raw.")
            except OSError:
                pass
        logging.warning("RUNNER: All processes terminated.")

    if loader_proc and loader_proc.returncode != 0:
        logging.error(
            f"RUNNER: Loader exited with non-zero status: {loader_proc.returncode}"
        )
        sys.exit(loader_proc.returncode)

    if sim_proc and sim_proc.returncode != 0 and sim_proc.returncode != -15:  # -15 is SIGTERM
        logging.error(
            f"RUNNER: Simulator exited with non-zero status: {sim_proc.returncode}"
        )
        sys.exit(sim_proc.returncode)

    logging.warning("RUNNER: Simulation completed successfully.")


if __name__ == "__main__":
    main()
