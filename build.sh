#!/bin/bash
set -eo pipefail
# 仅在本任务独立 CANN 容器内执行。源码位置不依赖固定的 /workspace 路径。
test -f /.dockerenv
cd "$(dirname "$(readlink -f "$0")")"
work=$PWD
role=${1:?usage: build.sh host|a3}
case "$role" in host|a3) ;; *) echo "Invalid role: $role" >&2; exit 2;; esac
export CANN=${CANN:-/usr/local/Ascend/cann-9.1.0}
source "$CANN/set_env.sh"
mkdir -p "$work/build"
trap 'echo $? > "$work/build/hcomm.exit"' EXIT
cd reference/hcomm
test -f include/hcomm_res.h
# Host 插件需要此分支 Debug 构建导出的内部符号。
# 不传 --noclean：每次按源码重新生成构建目录；下载缓存由上游脚本管理。
device_args=()
if [ "$role" = a3 ]; then device_args=(--full); fi
bash build.sh --pkg "${device_args[@]}" --experimental --build-type=Debug -j"${JOBS:-16}" -p "$CANN"
