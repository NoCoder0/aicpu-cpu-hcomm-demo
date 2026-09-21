# AICPU ↔ CPU 标准 hcomm RDMA READ demo

在独立 CANN 9.1 容器内，用公开 `Hcomm*` 接口建链，由 AICPU 调用
`HcommReadOnThread` 将 Host DRAM 的16字节读取到 NPU HBM，并检查其余4080字节哨兵。
这是单段 READ（最小单 SGE），不代表已验证多 SGE、最大 SGL、性能或压力场景。

- Demo：[NoCoder0/aicpu-cpu-hcomm-demo / feat/aicpu-cpu-rdma-demo](https://github.com/NoCoder0/aicpu-cpu-hcomm-demo/tree/feat/aicpu-cpu-rdma-demo)。
- hcomm：[NoCoder0/hcomm / feat/aicpu-cpu-rdma](https://github.com/NoCoder0/hcomm/tree/feat/aicpu-cpu-rdma)，
  固定提交 `33d156bf832744a4f767144149ed1b2e304ff2e3`，基于原分支 `feat/aicpu-urma-design` 的
  `64ef7f9e05964add831d976876ce54adb74cd286`，包含 Host/A3 兼容修改。
- hcomm 修改已在独立仓库提交；本仓通过 `reference/hcomm` 子模块引用，不需要再应用 patch。
- 回归结果见 [VALIDATION.md](VALIDATION.md)。

## 最小文件集与审阅入口

| 文件 | 用途 |
|---|---|
| `npu_reader.cpp` | A3 控制程序与 AICPU 用户 kernel；直接调用标准 READ/Drain/Batch 接口 |
| `host_server.cpp` | Host DRAM 注册、标准建链、等待读取和校验完成 |
| `common.h` | 双端 IP、NPU ID、端口、opaque MR 交换及 Channel 创建 |
| `standard_read.json` | ACL 用户 kernel 加载配置 |
| `build.sh` / `deploy.sh` / `run.sh` | 容器内构建 hcomm、部署库并编译 demo、运行 |
| `tests/` | 链接实际 hcomm/插件的5项兼容回归测试 |
| `reference/hcomm` | 固定版本的 hcomm 子模块 |

旧 RA demo、旧 full_hcomm 实现、旧环境试验和日志不在当前文件树中；历史提交仍保留。
编译产物、运行日志和部署 SHA256 清单均生成在 `build/`，不提交到 Git。

## 建链与读取流程

```text
Host3：CPU / HOST-ROCE                    A3：AICPU_TS / DEVICE-ROCE
HcommEndpointCreate                      aclInit / aclrtSetDevice(2)
                                         HcommEndpointCreate
分配 DRAM，填入 hello rdma demo           分配4096字节 HBM，填充0xa5
HcommMemReg / HcommMemExport              HcommMemReg / HcommMemExport
           ← 管理 TCP:19516 交换 opaque 描述符和端口 →
                                         HcommMemImport（Host DRAM）
HcommChannelDescInit                      HcommChannelDescInit
HcommChannelCreate(CPU, SERVER)           HcommChannelCreate(AICPU_TS, CLIENT)
           ← hcomm 内部 TCP:19517 白名单、能力、资源交换与 QP 建链 →
HcommChannelGetStatus == READY            HcommChannelGetStatus == READY
           ← 管理 TCP 确认两端 READY →
保持源 MR 有效                           HcommThreadAlloc(AICPU_TS)
                                         ACL 启动 StandardReadKernel
                                         HcommBatchModeStart
                                         HcommReadOnThread（16字节）
     Host DRAM ===== RDMA READ ======>   NPU HBM
                                         HcommChannelDrainOnThread
                                         HcommBatchModeEnd（提交任务）
                                         同步 kernel stream 与 ACL device
                                         HBM 回拷，仅作结果验证
           ← 校验通过确认 →
销毁 Channel、注销 MR                     释放 Thread、Channel、MR、HBM
```

建链由 `HcommChannelCreate` 内部完成；应用不调用 RA 或 libibverbs、不手工交换 QPN/PSN，
不解析 MR 描述符中的 rkey。管理 TCP 不传 payload。Host 只作 READ responder，无须导入对端 HBM。

此分支的插件 Endpoint 不支持 `HcommEndpointGetListenPort`，因此用公开 `HcommChannelDesc.port`
显式指定19517；实际监听和 QP 状态推进仍由 hcomm 完成。此版本也没有公开 `HcommThreadResGetInfo`，
因此使用 `aclrtSynchronizeDevice()` 等待已提交的通信任务，不解引用私有线程句柄。
READ、Drain 返回0仅表明调用成功，必须检查 BatchEnd、同步、数据内容和完整清理结果。

## 环境与准备

| 角色 | 管理 IP | RDMA IP / 设备 | 本任务独立容器 |
|---|---|---|---|
| A3 D | 10.1.101.201 | 20.168.0.3 / device 2 | rdma-hcomm-cann91 |
| Host3 | 10.1.101.27 | 20.168.0.19 / mlx5_2，enp165s0f0np0 | rdma-hcomm-host-cann91 |

容器由各自 hhy 的 CANN 9.1 环境复制，已存在时直接复用。所有编译、部署和执行在这些独立
容器内进行。`deploy.sh` 会替换目标容器内的 CANN hcomm 库与 A3 kernel 包。
容器需能访问 RDMA 设备，A3 还需能访问 NPU、匹配的驱动及 HCCP；现场容器采用 host 网络。
宿主机只用于容器管理、SSH 和只读网络诊断。

容器依赖：AArch64 Linux、CANN `/usr/local/Ascend/cann-9.1.0`（含 hcc 设备编译器）、
g++、CMake、make、Python3、git、binutils、RDMA 开发库及 hcomm 上游构建依赖。
测试另需 `/usr/src/googletest/googletest` 源码，以相同旧 C++ string ABI 编译。
`CANN` 可覆盖安装路径，`JOBS` 可调整构建并行度（默认16）。两端使用相同的子模块提交。

Host3 已验证加载 CANN 自带的 `devlib/aarch64/libascend_hal.so`，未编写 HAL 替代实现。
此结果不意味着任意未安装 CANN/HAL 的裸 CPU 环境可直接运行。

### 1. 每次运行前检查双向 RDMA 网络

```bash
# A3 宿主机
/usr/local/Ascend/driver/tools/hccn_tool -i 2 -ip -g
/usr/local/Ascend/driver/tools/hccn_tool -i 2 -link -g
timeout 40 /usr/local/Ascend/driver/tools/hccn_tool -i 2 -ping -g address 20.168.0.19 pkt 64
# Host3 宿主机
ping -c 3 -W 2 -I 20.168.0.19 20.168.0.3
```

必须看到 Link UP，双向各3包收到。网络失败时先确认实际配对；若换卡/Host，修改
`common.h` 中管理 IP、双方 RDMA IP 和物理 NPU ID，两端重新编译，再检查双向 ping。

### 2. 两端容器中分别拉取同一版本

```bash
# 宿主机：如容器停止，先 docker start <容器名>，然后进入容器
# Host3：docker exec -it rdma-hcomm-host-cann91 bash
# A3：   docker exec -it rdma-hcomm-cann91 bash
cd /workspace
GIT_LFS_SKIP_SMUDGE=1 git clone --branch feat/aicpu-cpu-rdma-demo \
  --recurse-submodules --shallow-submodules \
  https://github.com/NoCoder0/aicpu-cpu-hcomm-demo.git
cd aicpu-cpu-hcomm-demo
git submodule status
# 应为 33d156bf...；不要使用 submodule update --remote 漂移到其他版本。
```

LFS 跳过的是上游文档图片等资料，demo 不依赖它们；构建依赖按上游脚本下载，需要相应网络或缓存。
已有检出目录更新时使用 `git pull --ff-only` 后执行 `GIT_LFS_SKIP_SMUDGE=1 git submodule update --init --recursive`。

### 3. 分别构建与部署

```bash
# Host3 容器，仓库根目录
set -o pipefail
mkdir -p build
bash build.sh host 2>&1 | tee build/hcomm.log
bash deploy.sh host 2>&1 | tee build/deploy.log

# A3 容器，仓库根目录
set -o pipefail
mkdir -p build
bash build.sh a3 2>&1 | tee build/hcomm.log
bash deploy.sh a3 2>&1 | tee build/deploy.log
```

每条命令必须退出0后继续。Host 构建 `--pkg --experimental`；A3 额外 `--full`。
均使用 Debug 构建以提供此分支 experimental 插件需要的动态符号。
部署时只去除调试段；A3 用户 kernel 与同次构建的设备库一起打包，重建 `bin_hash.cfg`。
部署清单为 `build/deployed_libraries.json`，hcomm 构建退出码为 `build/hcomm.exit`。

### 4. 先 Host、后 A3，在两个终端运行

```bash
# Host3 容器，仓库根目录。等待输出 Control listening 后启动 A3。
set -o pipefail
bash run.sh host 2>&1 | tee build/host.log
rc=${PIPESTATUS[0]}; echo "$rc" > build/host.exit; test "$rc" -eq 0

# A3 容器，仓库根目录
set -o pipefail
bash run.sh a3 2>&1 | tee build/a3.log
rc=${PIPESTATUS[0]}; echo "$rc" > build/a3.exit; test "$rc" -eq 0
```

程序总超时300秒，建链超时120秒。需同时满足：

- 双方 Channel 状态 READY（日志 `CHANNEL status=0`）。
- A3 的 BatchStart、Read、Drain、BatchEnd 以及 ACL 同步返回0。
- A3 输出 `PASS: standard HcommReadOnThread HBM='hello rdma demo', bytes=16, guards=4080 unchanged`。
- Host 输出 `PASS: NPU confirmed standard HCOMM READ and HBM validation`。
- 双方清理完成，两个退出码文件均为0；不能仅凭 READ 返回0判断传输成功。

### 5. Host3 容器兼容回归

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
python3 tests/run.py
python3 tests/run.py --plugin
```

第一条3项测试覆盖 MR 描述符和 socket tag，第二条2项覆盖资源报文格式及非法输入。
测试不发起 RDMA，不能替代上面的双节点 HBM 实测。

## hcomm 兼容修改范围

修复 Host MR 描述符导入、两端 socket tag、Host RA 动态符号/白名单初始化，以及 A3/Host
资源通道的109字节 Drain 报文匹配。公开接口和 READ 数据原语未修改；应用不调用 RA/verbs。
Host 资源模式限定为1 QP、0用户 notify。不支持的资源形状明确返回错误。
