#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# 必须指向与固定 hcomm 源码配套的 CANN，以及该源码的实际构建产物。
# 找不到新库就失败；不允许静默链接上一版含自定义 READ 的库。
: "${CANN:?set CANN to the matching Toolkit installation}"
HCOMM_SOURCE=${HCOMM_SOURCE:-$PWD/source}
HCOMM_HOST_LIB=${HCOMM_HOST_LIB:-$PWD/deploy/aarch64-linux/lib64}
: "${HCOMM_DEVICE_LIB:?set directory containing source-built device libccl_kernel.so}"
test -f "$HCOMM_HOST_LIB/libhcomm.so"
test -f "$HCOMM_DEVICE_LIB/libccl_kernel.so"
flags=(-std=c++14 -D_GLIBCXX_USE_CXX11_ABI=0 -O2 -Wall -Wextra -Werror
       -I"$HCOMM_SOURCE/include" -I"$CANN/include")
libs=(-L"$HCOMM_HOST_LIB" -L"$CANN/lib64"
      -Wl,-rpath,"$HCOMM_HOST_LIB" -Wl,-rpath-link,"$HCOMM_HOST_LIB" -Wl,-rpath-link,"$CANN/lib64")
g++ "${flags[@]}" host_server.cpp "${libs[@]}" -lhcomm -lc_sec -ldl -pthread -o host_server
g++ "${flags[@]}" npu_reader.cpp "${libs[@]}" -lhcomm -lascendcl -lc_sec -ldl -pthread -o npu_reader

# 设备 kernel 用 AICPU 工具链构建。它只调用标准 READ/Drain，不包含 RA 或 verbs。
# 这里生成用户 kernel DSO；deploy_in_container.sh 将其加入同版本设备包并更新校验清单。
# 链接、设备包加载与实际 READ 是不同验证阶段，最终以两端数据检查结果为准。
"$CANN/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++" "${flags[@]}" \
    -DSTANDARD_HCOMM_AICPU -shared -fPIC npu_reader.cpp -L"$HCOMM_DEVICE_LIB" \
    -Wl,-z,defs -Wl,-rpath,'$ORIGIN' -lccl_kernel -o libstandard_read_kernel.so
