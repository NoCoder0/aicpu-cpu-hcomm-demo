#!/bin/bash
set -eo pipefail
test -f /.dockerenv
cd /workspace/nocoder
source /usr/local/Ascend/cann-9.1.0/set_env.sh
export ASCEND_PROCESS_LOG_PATH="$PWD/logs"
export ASCEND_SLOG_PRINT_TO_STDOUT=1
export ASCEND_GLOBAL_LOG_LEVEL=1
export HCCL_CONNECT_TIMEOUT=120
# Both socket implementations must exchange the same whitelist acknowledgement.
# The patched Host endpoint enables its RA switch before PEER initialization;
# keep the outer HCCL configuration consistent with the A3 HCCP service too.
export HCCL_WHITELIST_DISABLE=0
mkdir -p "$ASCEND_PROCESS_LOG_PATH"
role=${1:?usage: run_nocoder.sh host|a3 [--probe-endpoint|--probe-thread]}
shift
if [ "$role" = host ]; then
    export HCOMM_HOST_ONLY=1
    export HCOMM_FORCE_HOST_NIC_PLUGIN=1
    exec ./host_server "$@"
fi
test "$role" = a3
export HCOMM_HOST_ONLY=0
unset HCOMM_FORCE_HOST_NIC_PLUGIN
if [ "$#" -gt 0 ]; then exec ./npu_reader "$@"; fi
exec ./npu_reader "$PWD/standard_read.json"
