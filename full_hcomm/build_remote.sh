#!/bin/bash
set -euo pipefail
work=/data1/z00502111/rdma_sgl_host_to_hbm/full_hcomm
trap 'rc=$?; printf "%s\n" "$rc" > "$work/build.exit"' EXIT
python3 -m pip install --target "$work/python_deps" wheel==0.37.1
export PYTHONPATH="$work/python_deps:${PYTHONPATH:-}"
cd "$work/source"
python3 - <<'PY'
from pathlib import Path
for path in Path('.').rglob('*.sh'):
    data = path.read_bytes()
    if b'\r\n' in data:
        path.write_bytes(data.replace(b'\r\n', b'\n'))
PY
printf 'running\n' > "$work/build.exit"
source /usr/local/Ascend/ascend-toolkit/set_env.sh
bash build.sh --pkg --full -j16 --version 9.0.0 -p /usr/local/Ascend/cann-9.0.0
