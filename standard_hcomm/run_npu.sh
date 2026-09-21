#!/bin/bash
set -eo pipefail
cd /workspace
source /usr/local/Ascend/cann-9.1.0/set_env.sh
export ASCEND_PROCESS_LOG_PATH=/workspace/logs
export ASCEND_GLOBAL_LOG_LEVEL=1
export HCCL_CONNECT_TIMEOUT=120
mkdir -p "$ASCEND_PROCESS_LOG_PATH"
if [ "$#" -gt 0 ]; then
    exec ./npu_reader "$@"
fi
exec ./npu_reader /workspace/standard_read.json
