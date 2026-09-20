#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
CANN=${CANN:-/usr/local/Ascend/cann-9.0.0/aarch64-linux}
HCOMM_LIB="$PWD/deploy/hcomm/lib64"
g++ -std=c++14 -D_GLIBCXX_USE_CXX11_ABI=0 -O2 -Wall -Wextra npu_reader.cpp -o npu_reader \
    -Isource/src/platform/typical -Isource/include -Isource/pkg_inc -I"$CANN/include" \
    -L"$HCOMM_LIB" -L"$CANN/lib64" -Wl,-rpath,"$HCOMM_LIB" -Wl,-rpath-link,"$HCOMM_LIB" \
    -Wl,--no-as-needed -lhcomm -lhccl_plf -lascendcl -Wl,--as-needed -ldl -pthread
