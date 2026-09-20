#!/bin/bash
set -euo pipefail
source /usr/local/Ascend/ascend-toolkit/set_env.sh
cd "$(dirname "$0")"
export LD_LIBRARY_PATH="$PWD/deploy/hcomm/lib64:$LD_LIBRARY_PATH"
export HCCL_RDMA_PCIE_DIRECT_POST_NOSTRICT=TRUE
export HCCL_CONNECT_TIMEOUT=120
export HCCL_EXEC_TIMEOUT=15
exec ./npu_reader
