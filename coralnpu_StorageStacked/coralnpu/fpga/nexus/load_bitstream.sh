#!/bin/bash
# Copyright 2026 Google LLC
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

set -eu -o pipefail

usage() {
    echo "Usage:"
    echo "  load_bitsream.sh --bitstream chip_nexus.bin --host nexus01 --mcu_uart /dev/ttyUSB5 [options]"
    echo ""
    echo "Options:"
    echo "  -b, --bitstream  Path to bitstream file to load"
    echo "  --host           Hostname or IP of Nexus SOM"
    echo "  --mcu_uart       Path to device for Nexus MCU UART"
    echo "  -h, --help       Display this help message and exit"
    echo ""
    exit 1
}

while [[ "$#" -gt 0 ]]; do
    case "${1}" in
        -b|--bitstream)
            BITSTREAM_PATH="$2"; shift 2 ;;
        --host)
            NEXUS_HOST="$2"; shift 2 ;;
        --mcu_uart)
            MCU_UART="$2"; shift 2 ;;
        -h|--help)
            usage ;;
        *)
            echo "Unknown option: $1"; usage ;;
    esac
done

# cd into project and find root via git
cd "$(dirname -- "${BASH_SOURCE[0]}")" || exit 1
# Verify Git history availability in project
if ! git rev-parse --is-inside-work-tree >/dev/null; then
    echo "Error: git not found or git history not available."
    exit 1
fi
PROJECT_ROOT=$(git rev-parse --show-toplevel)
cd "${PROJECT_ROOT}"

if [[ ! -v BITSTREAM_PATH ]]; then
    echo "[INFO] --bitstream not specified, searching fpga/bitstreams/"
    BITSTREAM_PATH=$(find -name 'chip_nexus.bin' -printf "%P\n" -quit) || true
    if [[ -z "${BITSTREAM_PATH}" ]]; then
        echo "[ERROR] Unable to find bitstream file"
        exit 1
    fi
    echo "[INFO] Found ${BITSTREAM_PATH}"
fi

if [[ ! -f "${BITSTREAM_PATH}" ]]; then
    echo "[ERROR] bitstream file not found: ${BITSTREAM_PATH}"
fi

if [[ ! -v MCU_UART ]]; then
    if [[ -v NEXUS_ID ]]; then
        MCU_UART="/dev/Nexus-FTDI-${NEXUS_ID}-MCU-UART"
    fi
    echo "[ERROR] specify path to MCU UART with --mcu_uart, or set NEXUS_ID"
    exit 1
fi

if [[ ! -v NEXUS_HOST ]]; then
    if [[ -v NEXUS_ID ]]; then
        NEXUS_HOST="nexus${NEXUS_ID}"
    fi
    echo "[ERROR] specify hostname of Nexus SOM with --host, or set NEXUS_ID"
    exit 1
fi

# Check that tio is installed, issue error otherwise
if ! command -v tio > /dev/null; then
  echo "[ERROR] tio command not found.  Install with: sudo apt-get install tio"
  exit 1
fi

# This needs error handling:
function nexus_mcu_write() {
    # Send command to the Nexus MCU REPL
    echo "Clearing MCU UART input buffer..."
    echo -e '\r' | tio ${MCU_UART} --script "expect('Unrecognized command: \r', 1000)" --mute
    echo "Sending nexus MCU the command: $@"
    echo -ne "$@\r" | tio ${MCU_UART} --script "expect('> ', 1000)" --mute
    echo
}

BITSTREAM=$(basename "${BITSTREAM_PATH}")
LOCAL_SHA=$(sha256sum "${BITSTREAM_PATH}" | cut -f1 -d' ')
REMOTE_SHA=$(ssh root@$NEXUS_HOST "sha256sum /mnt/mmcp1/$BITSTREAM | cut -f1 -d' '")

if [[ "${LOCAL_SHA}" != "${REMOTE_SHA}" ]]; then
    scp "${BITSTREAM_PATH}" root@${NEXUS_HOST}:/mnt/mmcp1/
fi

# Loading a bitstream with the camera powered on can wedge it
nexus_mcu_write "camera_powerdown"

# ssh to nexus and run zturn, zturn returns status 1 on success
ssh root@${NEXUS_HOST} "/mnt/mmcp1/zturn -d a /mnt/mmcp1/${BITSTREAM}" || zturn_status=$?
if [[ zturn_status -ne "1" ]]; then
    echo "zturn failed, exiting"
    exit "${zturn_status}"
fi

# enable camera
nexus_mcu_write "camera_powerup"
