#!/bin/bash
set -euo pipefail
work=/data1/z00502111/rdma_sgl_host_to_hbm/full_hcomm
trap 'rc=$?; printf "%s\n" "$rc" > "$work/incremental.exit"' EXIT
printf 'running\n' > "$work/incremental.exit"
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export PYTHONPATH="$work/python_deps:${PYTHONPATH:-}"
cd "$work/source"
cmake --build build --target package -j16
cmake --install build --prefix "$work/deploy"
