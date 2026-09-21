#!/bin/bash
set -eo pipefail
# 两端均在各自独立容器中执行；与上一版 /workspace/source 分开保留产物。
test -f /.dockerenv
cd /workspace/nocoder
work=$PWD
trap 'echo $? > "$work/build.exit"' EXIT
echo running > "$work/build.exit"
export CANN=/usr/local/Ascend/cann-9.1.0
source "$CANN/set_env.sh"
cd source
python3 - <<'PY'
from pathlib import Path
for path in Path('.').rglob('*.sh'):
    path.write_bytes(path.read_bytes().replace(b'\r', b''))
PY
# 原分支的 experimental 插件需要导出的内部动态符号，采用 Debug 构建。
device_args=()
if [ "${1:-a3}" = a3 ]; then device_args=(--full); fi
bash build.sh --pkg "${device_args[@]}" --experimental --build-type=Debug --noclean -j48 -p "$CANN"
cmake --install build --prefix "$work/deploy"
