#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
CANN=${CANN:-/usr/local/Ascend/cann-9.0.0}
HCOMM_SOURCE=${HCOMM_SOURCE:-$PWD/source}
# 只验证公开头文件/语法，不等同于链接成功、设备 kernel 打包成功或实机通过。
flags=(-std=c++14 -Wall -Wextra -Werror -fsyntax-only
       -I"$HCOMM_SOURCE/include" -I"$CANN/include")
g++ "${flags[@]}" host_server.cpp
g++ "${flags[@]}" npu_reader.cpp
g++ "${flags[@]}" -DSTANDARD_HCOMM_AICPU npu_reader.cpp
g++ "${flags[@]}" probe_endpoint.cpp
printf 'PASS: public-header syntax checks (Host, NPU control, AICPU kernel, probe)\n'
