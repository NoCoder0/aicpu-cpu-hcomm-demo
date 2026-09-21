#!/bin/bash
set -eo pipefail
test -f /.dockerenv
cd /workspace/nocoder
source /usr/local/Ascend/cann-9.1.0/set_env.sh
# 增量修复只更新宿主侧库，设备包与用户 READ kernel 保持同一次已验证构建的版本。
python3 - <<'PY'
from pathlib import Path
import hashlib, json, shutil, subprocess
root = Path('/workspace/nocoder')
path = root/'deployed_libraries.json'
manifest = json.loads(path.read_text())
for entry in manifest:
    src = Path(entry['source'])
    if src.name not in ('libhcomm.so', 'libhcomm_cpu_roce_plugin.so'):
        continue
    dst = Path(entry['destination'])
    shutil.copy2(src, dst)
    subprocess.run(['strip', '--strip-debug', str(dst)], check=True)
    entry['source_sha256'] = hashlib.sha256(src.read_bytes()).hexdigest()
    entry['deployed_sha256'] = hashlib.sha256(dst.read_bytes()).hexdigest()
path.write_text(json.dumps(manifest, indent=2)+'\n')
PY
