#!/bin/bash
set -eo pipefail
cd /workspace
work="$PWD"
trap 'echo $? > "$work/build.exit"' EXIT
echo running > "$work/build.exit"
export CANN=/usr/local/Ascend/cann-9.1.0
source "$CANN/set_env.sh"
cd source
python3 - <<'PY'
from pathlib import Path
for path in Path('.').rglob('*.sh'):
 data=path.read_bytes()
 path.write_bytes(data.replace(b'\r\n',b'\n'))
PY
# 上游 experimental NIC 插件依赖内部动态符号；Release 的符号隐藏会使其链接失败。
# 使用上游支持的 Debug 构建模式，保留同一套主机/设备产物和调试符号。
bash build.sh --pkg --full --experimental --build-type=Debug --noclean -j64 -p "$CANN"
cmake --install build --prefix /workspace/deploy
