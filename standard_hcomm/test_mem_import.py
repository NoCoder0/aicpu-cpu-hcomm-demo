"""在 Host3 容器内用实际构建的 hcomm 运行聚焦的描述符兼容测试。"""
from pathlib import Path
import shlex
import subprocess
import sys

root = Path('/workspace/nocoder')
source = root / 'source'
cann = Path('/usr/local/Ascend/cann-9.1.0')
plugin = '--plugin' in sys.argv
flags_file = source / ('build/experimental/base_comm/nic_plugin/CMakeFiles/hcomm_cpu_roce_plugin.dir/flags.make'
                       if plugin else 'build/src/legacy/ascend910/framework/CMakeFiles/hcomm.dir/flags.make')
include_line = next(line for line in flags_file.read_text().splitlines() if line.startswith('CXX_INCLUDES ='))
includes = shlex.split(include_line.split('=', 1)[1])
test = source / 'test/ut/framework/next/comms/endpoints/reged_mems/ut_aicpu_host_roce_import.cc'
if plugin:
    test = root / 'test_resource_wire.cpp'
gtest = Path('/usr/src/googletest/googletest')
command = ['g++', '-std=c++17', '-D_GLIBCXX_USE_CXX11_ABI=0', '-O0', '-g', *includes, str(test)]
if plugin:
    command += ['-fno-access-control', f'-I{source}', f'-L{cann}/hcomm_plugin',
                f'-Wl,-rpath,{cann}/hcomm_plugin', '-lhcomm_cpu_roce_plugin']
if (gtest / 'src/gtest-all.cc').is_file():
    # SDK 使用旧 libstdc++ string ABI；从系统提供的 gtest 源码编译成相同 ABI。
    command += [f'-I{gtest}', f'-I{gtest}/include', str(gtest / 'src/gtest-all.cc'), str(gtest / 'src/gtest_main.cc')]
else:
    command += ['-lgtest_main', '-lgtest']
command += [f'-L{cann}/lib64', f'-Wl,-rpath,{cann}/lib64', f'-Wl,-rpath-link,{cann}/lib64',
            '-lhcomm', '-lhccl_plf', '-lunified_dlog', '-lc_sec', '-ldl', '-pthread',
            '-o', str(root / 'test_mem_import')]
result = subprocess.run(command)
if result.returncode:
    raise SystemExit(result.returncode)
subprocess.run([str(root / 'test_mem_import')], check=True)
