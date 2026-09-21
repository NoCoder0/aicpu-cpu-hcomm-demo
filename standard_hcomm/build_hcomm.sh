#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
work="$PWD"
trap 'rc=$?; printf "%s\n" "$rc" > "$work/build.exit"' EXIT
printf 'running\n' > build.exit
CANN=${CANN:-/usr/local/Ascend/cann-9.0.0}
source "$CANN/set_env.sh"
# 仅复用此前下载的 Python 打包依赖，不覆盖系统 CANN 或基线部署。
export PYTHONPATH="/data1/z00502111/rdma_sgl_host_to_hbm/full_hcomm/python_deps:${PYTHONPATH:-}"
cd source
python3 - <<'PY'
from pathlib import Path
for path in Path('.').rglob('*.sh'):
    data = path.read_bytes()
    path.write_bytes(data.replace(b'\r\n', b'\n'))
PY
# Host-only RoCE 使用上游 experimental NIC 插件；NPU 使用上游 DEVICE RoCE。
bash build.sh --pkg --full --experimental --noclean -j32 -p "$CANN"
cmake --install build --prefix "$work/deploy"
