#!/bin/bash
set -eo pipefail
# Run ONLY through: docker exec rdma-hcomm-cann91 bash /workspace/deploy_in_container.sh
# The Toolkit directory is private to our container's writable layer.
test -f /.dockerenv
test "$PWD" = /workspace
export CANN=/usr/local/Ascend/cann-9.1.0
source "$CANN/set_env.sh"

# Keep exact provenance. This is a 9.2 hcomm source snapshot with explicit
# compatibility edits for Runtime 9.1, not an official 9.1 hcomm release.
python3 - <<'PY'
from pathlib import Path
import hashlib, json, shutil
work = Path('/workspace')
cann = Path('/usr/local/Ascend/cann-9.1.0')
build = work / 'source/build'
device = build / 'device_build'
copies = [(p, cann / 'lib64' / p.name) for p in (build / 'src').rglob('*.so')]
copies += [(p, cann / 'hcomm_plugin' / p.name)
           for p in (build / 'experimental/base_comm/nic_plugin').glob('*.so')]
for name in ('cann-hcomm-compat.tar.gz', 'cann-hccd-compat.tar.gz', 'cann-hcomm-compat.ini'):
    copies.append((device / name, cann / 'compat' / name))
copies += [
    (device / 'aicpu_hcomm.tar.gz', cann / 'opp/built-in/op_impl/aicpu/kernel/aicpu_hcomm.tar.gz'),
    (device / 'src/legacy/ascend910/framework/ccl_kernel.json',
     cann / 'opp/built-in/op_impl/aicpu/config/ccl_kernel.json')]
assert any(p.name == 'libhcomm.so' for p, _ in copies)
assert any(p.name == 'libhcomm_cpu_roce_plugin.so' for p, _ in copies)
manifest = []
for src, dst in copies:
    assert src.is_file(), src
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    manifest.append({'source': str(src), 'destination': str(dst),
                     'sha256': hashlib.sha256(dst.read_bytes()).hexdigest()})
(work / 'deployed_libraries.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY

export HCOMM_HOST_LIB="$CANN/lib64"
export HCOMM_DEVICE_LIB="$PWD/source/build/device_build/src/legacy/ascend910/framework"
bash build_demo.sh
python3 package_host.py
cp run_host.sh host_bundle/run_host.sh

# Extend the source-built device bundle with the user kernel that calls the
# public Hcomm READ/Drain APIs. Recompute all manifest hashes after packaging.
# The device's pre-existing verification mode was queried; we do not change it.
python3 - <<'PY'
from pathlib import Path
import hashlib, json, shutil, subprocess, tarfile
root = Path('/workspace')
stage = root / 'user_kernel_package'
stage.mkdir(exist_ok=True)
archive = root / 'source/build/device_build/aicpu_hcomm.tar.gz'
with tarfile.open(archive) as source:
    source.extractall(stage, filter='data')
kernels = stage / 'aicpu_kernels_device'
shutil.copy2(root / 'libstandard_read_kernel.so', kernels / 'libstandard_read_kernel.so')
# Debug mode is needed for host plugin exports, but gigabytes of device DWARF
# sections slow TSD package loading. Strip only debug sections from this copy;
# all public dynamic symbols and the original build products remain intact.
strip = '/usr/local/Ascend/cann-9.1.0/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-strip'
for library in kernels.glob('*.so'):
    subprocess.run([strip, '--strip-debug', str(library)], check=True)
hashes = [f'{p.name}={hashlib.sha256(p.read_bytes()).hexdigest()}\n'
          for p in sorted(kernels.glob('*.so'))]
(kernels / 'bin_hash.cfg').write_text(''.join(hashes))
target = Path('/usr/local/Ascend/cann-9.1.0/opp/built-in/op_impl/aicpu/kernel/aicpu_hcomm.tar.gz')
with tarfile.open(target, 'w:gz') as out:
    out.add(kernels, arcname='aicpu_kernels_device')
manifest_path = root / 'deployed_libraries.json'
manifest = json.loads(manifest_path.read_text())
for entry in manifest:
    if entry['destination'] == str(target):
        entry['source_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
        entry['sha256'] = hashlib.sha256(target.read_bytes()).hexdigest()
        entry['transformation'] = 'Add user READ kernel, strip debug sections, regenerate bin_hash.cfg'
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
PY
echo 'Deployment and demo build complete; this does not imply RDMA READ passed.'
