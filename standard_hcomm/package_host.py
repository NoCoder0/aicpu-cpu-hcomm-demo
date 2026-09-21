"""Package actual CANN dependencies for the CPU-only Kunpeng responder.

Run inside our container after installing the source-built hcomm and building
host_server. No SDK installation or global library-path change is needed on Host4.
System glibc/libstdc++/verbs stay supplied by Host4; only Ascend libraries are copied.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

work = Path(__file__).resolve().parent
cann = Path(os.environ["CANN"])
bundle = work / "host_bundle"
lib = bundle / "lib"
plugins = bundle / "hcomm_plugin"
lib.mkdir(parents=True, exist_ok=True)
plugins.mkdir(parents=True, exist_ok=True)
shutil.copy2(work / "host_server", bundle / "host_server")
plugin = cann / "hcomm_plugin/libhcomm_cpu_roce_plugin.so"
shutil.copy2(plugin, plugins / plugin.name)

# RA backends are loaded with dlopen, so seed them explicitly in addition to
# the ELF dependency closure. Do not include stub libraries from devlib.
seeds = [work / "host_server", plugin]
for pattern in ("libhcomm*.so*", "libhccl*.so*", "libra*.so*", "librs.so*"):
    seeds.extend((cann / "lib64").glob(pattern))
files = {}
for seed in seeds:
    if str(seed).startswith("/usr/local/Ascend/") and seed != plugin:
        files[seed.name] = seed
    result = subprocess.run(["ldd", str(seed)], text=True, capture_output=True, check=True)
    if "not found" in result.stdout:
        raise RuntimeError(result.stdout)
    for name, resolved in re.findall(r"\s*(\S+) => (\S+) \(", result.stdout):
        if resolved.startswith("/usr/local/Ascend/"):
            files[name] = Path(resolved)

manifest = {}
for name, source in sorted(files.items()):
    target = lib / name
    shutil.copy2(source.resolve(strict=True), target)
    # Keep dynamic exports required by the plugin, drop only debug sections from
    # the transport bundle. Full debug builds remain in the NPU workspace.
    subprocess.run(["strip", "--strip-debug", str(target)], check=True)
    manifest[name] = {"source": str(source.resolve()),
                      "sha256": hashlib.sha256(target.read_bytes()).hexdigest()}
(bundle / "libraries.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"Packaged {len(manifest)} real Ascend libraries; Host4 supplies system libraries.")
