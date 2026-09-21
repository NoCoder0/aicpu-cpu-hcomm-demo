#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# This script is copied into host_bundle. ASCEND_HOME_PATH selects its plugin
# directory; the CPU-only plugin owns HOST/ROCE resources without an NPU context.
export ASCEND_HOME_PATH="$PWD"
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export ASCEND_PROCESS_LOG_PATH="$PWD/logs"
export ASCEND_GLOBAL_LOG_LEVEL=1
# CPU-only hosts may not start the Ascend log daemon. Keep diagnostics in the
# caller's redirected log even when the driver's console-log query fails.
export ASCEND_SLOG_PRINT_TO_STDOUT=1
export HCCL_CONNECT_TIMEOUT=120
mkdir -p "$ASCEND_PROCESS_LOG_PATH"
exec ./host_server "$@"
