#!/bin/bash
set -eo pipefail
test -f /.dockerenv
cd /workspace/nocoder
role=${1:?usage: deploy_nocoder.sh host|a3}
export CANN=/usr/local/Ascend/cann-9.1.0
source "$CANN/set_env.sh"
export DEMO_ROLE=$role
python3 - <<'PY'
from pathlib import Path
import hashlib, json, os, shutil, subprocess
root = Path('/workspace/nocoder')
build = root / 'source/build'
cann = Path('/usr/local/Ascend/cann-9.1.0')
copies = [(p, cann/'lib64'/p.name) for p in (build/'src').rglob('*.so')]
copies += [(p, cann/'hcomm_plugin'/p.name)
           for p in (build/'experimental/base_comm/nic_plugin').glob('*.so')]
assert any(p.name == 'libhcomm.so' for p, _ in copies)
assert any(p.name == 'libhcomm_cpu_roce_plugin.so' for p, _ in copies)
manifest = []
for src, dst in copies:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    subprocess.run(['strip', '--strip-debug', str(dst)], check=True)
    manifest.append({'source': str(src), 'destination': str(dst),
                     'source_sha256': hashlib.sha256(src.read_bytes()).hexdigest(),
                     'deployed_sha256': hashlib.sha256(dst.read_bytes()).hexdigest(),
                     'transformation': 'strip debug sections only'})
(root/'deployed_libraries.json').write_text(json.dumps(manifest, indent=2)+'\n')
PY
flags=(-std=c++14 -D_GLIBCXX_USE_CXX11_ABI=0 -O2 -Wall -Wextra -Werror
       -I"$PWD/source/include" -I"$CANN/include")
libs=(-L"$CANN/lib64" -Wl,-rpath,"$CANN/lib64" -Wl,-rpath-link,"$CANN/lib64")
g++ "${flags[@]}" host_server.cpp "${libs[@]}" -lhcomm -lc_sec -ldl -pthread -o host_server
if [ "$role" = host ]; then exit 0; fi
test "$role" = a3
g++ "${flags[@]}" npu_reader.cpp "${libs[@]}" -lhcomm -lascendcl -lc_sec -ldl -pthread -o npu_reader
device="$PWD/source/build/device_build"
"$CANN/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++" "${flags[@]}" \
    -DSTANDARD_HCOMM_AICPU -shared -fPIC npu_reader.cpp \
    -L"$device/src/legacy/ascend910/framework" -Wl,-z,defs -Wl,-rpath,'$ORIGIN' \
    -lccl_kernel -o libstandard_read_kernel.so
python3 - <<'PY'
from pathlib import Path
import hashlib, json, shutil, subprocess, tarfile
root = Path('/workspace/nocoder')
cann = Path('/usr/local/Ascend/cann-9.1.0')
device = root/'source/build/device_build'
manifest_path = root/'deployed_libraries.json'
manifest = json.loads(manifest_path.read_text())
for name in ('cann-hcomm-compat.tar.gz', 'cann-hccd-compat.tar.gz', 'cann-hcomm-compat.ini'):
    src, dst = device/name, cann/'compat'/name
    shutil.copy2(src, dst)
    manifest.append({'source': str(src), 'destination': str(dst),
                     'deployed_sha256': hashlib.sha256(dst.read_bytes()).hexdigest()})
shutil.copy2(device/'src/legacy/ascend910/framework/ccl_kernel.json',
             cann/'opp/built-in/op_impl/aicpu/config/ccl_kernel.json')
stage = root/'user_kernel_package'
stage.mkdir(exist_ok=True)
archive = device/'aicpu_hcomm.tar.gz'
with tarfile.open(archive) as source:
    source.extractall(stage, filter='data')
kernels = stage/'aicpu_kernels_device'
shutil.copy2(root/'libstandard_read_kernel.so', kernels/'libstandard_read_kernel.so')
strip = cann/'toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-strip'
for library in kernels.glob('*.so'):
    subprocess.run([str(strip), '--strip-debug', str(library)], check=True)
(kernels/'bin_hash.cfg').write_text(''.join(
    f'{p.name}={hashlib.sha256(p.read_bytes()).hexdigest()}\n' for p in sorted(kernels.glob('*.so'))))
target = cann/'opp/built-in/op_impl/aicpu/kernel/aicpu_hcomm.tar.gz'
with tarfile.open(target, 'w:gz') as out:
    out.add(kernels, arcname='aicpu_kernels_device')
manifest.append({'source': str(archive), 'destination': str(target),
                 'source_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
                 'deployed_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
                 'transformation': 'add user kernel; strip debug; regenerate bin_hash.cfg'})
manifest_path.write_text(json.dumps(manifest, indent=2)+'\n')
PY
