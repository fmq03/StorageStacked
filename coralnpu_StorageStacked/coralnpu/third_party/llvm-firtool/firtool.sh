#!/bin/bash

SCRIPT_DIR="$(dirname "$BASH_SOURCE")"
# The script lives at <runfiles>/_main/third_party/llvm-firtool/firtool.sh
# and the @llvm_firtool repo is at <runfiles>/llvm_firtool/.
# Walk three levels up to land at runfiles root, then find it.
ROOTDIR=${SCRIPT_DIR}/../../../

FIRTOOL_EXE=$(find ${ROOTDIR} -name 'firtool' -ipath '*linux-x64*' -print -quit)

${FIRTOOL_EXE} $*

exit $?
