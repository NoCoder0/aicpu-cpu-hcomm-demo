#!/bin/bash
set -eu
cd "$(dirname "$0")"
CANN=${CANN:-/usr/local/Ascend/cann-9.0.0/aarch64-linux}
g++ -std=c++14 -O2 -Wall -Wextra npu_reader.cpp -o npu_reader \
    -I../vendor/include -I"$CANN/include" -I"$CANN/pkg_inc" -I"$CANN/pkg_inc/runtime" \
    -I"$CANN/pkg_inc/profiling" -I"$CANN/pkg_inc/runtime/runtime" \
    -L"$CANN/lib64" -Wl,-rpath,"$CANN/lib64" -lascendcl -lra -lruntime -ltsdclient -pthread
