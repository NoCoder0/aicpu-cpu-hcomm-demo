#!/bin/bash
set -eo pipefail
cd "$(dirname "$0")"
work="$PWD"
trap 'rc=$?; printf "%s\n" "$rc" > "$work/build.exit"' EXIT
printf 'running\n' > build.exit
CANN=${CANN:-/usr/local/Ascend/cann-9.1.0}
source "$CANN/set_env.sh"
cd source
python3 - <<'PY'
from pathlib import Path
for path in Path('.').rglob('*.sh'):
    data = path.read_bytes()
    path.write_bytes(data.replace(b'\r\n', b'\n'))
PY
# Host-only RoCE 使用上游 experimental NIC 插件；NPU 使用上游 DEVICE RoCE。
bash build.sh --pkg --full --experimental --build-type=Debug --noclean -j32 -p "$CANN"
cmake --install build --prefix "$work/deploy"
