#!/bin/bash
# A3 独立容器中运行；先在 Host3 启动 bash run.sh host。
set -o pipefail
test -f /.dockerenv
cd "$(dirname "$(readlink -f "$0")")"
if [ "${1:-}" = --application-run ]; then
    shift
    bash run.sh a3 "$@"
    rc=$?
    printf '%s\n' "$rc" > "$HCOMM_PROFILE_APP_EXIT_FILE"
    exit "$rc"
fi
set -e
export CANN=${CANN:-/usr/local/Ascend/cann-9.1.0}
source "$CANN/set_env.sh"
output=${1:?usage: profile.sh output_directory [rounds=100] [warmup=10]}
rounds=${2:-100}
warmup=${3:-10}
test ! -e "$output" # 每次独立输出，避免把旧采集误认为本轮成功。
mkdir -p "$output"
export HCOMM_PROFILE_APP_EXIT_FILE="$(readlink -f "$output")/application.exit"
# msprof 的 AICPU / task 数据用于算子时延；通信是否完成仍由程序同步和逐轮校验确认。
msprof --output="$output" --aicpu=on --task-time=on --runtime-api=on --ai-core=off \
    bash "$PWD/profile.sh" --application-run "$rounds" "$warmup"
# 本机 msprof 可在应用失败后返回0；必须另外检查被测应用退出码。
test -f "$HCOMM_PROFILE_APP_EXIT_FILE"
test "$(cat "$HCOMM_PROFILE_APP_EXIT_FILE")" = 0
