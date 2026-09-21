#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
source /usr/local/Ascend/ascend-toolkit/set_env.sh
CANN=/usr/local/Ascend/cann-9.0.0
oldlib="$PWD/../full_hcomm/deploy/hcomm/lib64"
g++ -std=c++14 -Wall -Wextra -Werror probe_endpoint.cpp -Isource/include -I"$CANN/include" \
    -L"$oldlib" -L"$CANN/lib64" -Wl,-rpath-link,"$oldlib" \
    -lhcomm -lascendcl -lc_sec -ldl -o probe_endpoint
# 这是明确标为 v9 的能力探测，不是新版 demo 的运行脚本。
export LD_LIBRARY_PATH="$oldlib:$LD_LIBRARY_PATH"
exec ./probe_endpoint
