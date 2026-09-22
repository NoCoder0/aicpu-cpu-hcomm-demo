#!/bin/bash
set -eo pipefail
test -f /.dockerenv
cd "$(dirname "$(readlink -f "$0")")"
export CANN=${CANN:-/usr/local/Ascend/cann-9.1.0}
source "$CANN/set_env.sh"
export ASCEND_PROCESS_LOG_PATH="$PWD/build/logs"
export ASCEND_SLOG_PRINT_TO_STDOUT=${ASCEND_SLOG_PRINT_TO_STDOUT:-0}
export ASCEND_GLOBAL_LOG_LEVEL=${ASCEND_GLOBAL_LOG_LEVEL:-3}
export HCCL_CONNECT_TIMEOUT=120
# Both socket implementations must exchange the same whitelist acknowledgement.
# The patched Host endpoint enables its RA switch before PEER initialization;
# keep the outer HCCL configuration consistent with the A3 HCCP service too.
export HCCL_WHITELIST_DISABLE=0
mkdir -p "$ASCEND_PROCESS_LOG_PATH"
role=${1:?usage: run.sh host|a3}
if [ "$role" = host ]; then
    export HCOMM_HOST_ONLY=1
    export HCOMM_FORCE_HOST_NIC_PLUGIN=1
    exec ./build/host_server
fi
test "$role" = a3
export HCOMM_HOST_ONLY=0
unset HCOMM_FORCE_HOST_NIC_PLUGIN
exec ./build/npu_reader "$PWD/standard_read.json" "${2:-100}" "${3:-10}"
