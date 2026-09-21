# 标准 hcomm：Host3 DRAM → A3 NPU HBM

**已实机通过。** 2026-09-21，在两个独立 CANN 9.1 容器内，用公开 `Hcomm*` 接口建链，
由 AICPU 调用 `HcommReadOnThread` 读取16字节到 HBM；4080字节哨兵保持不变，两端退出0。
完整环境、改动及证据见 [RESULT_NOCODER.md](RESULT_NOCODER.md)。

当前分支 `feature/nocoder-hcomm-host3`；hcomm 子模块分支 `demo/nocoder-host3`，基于
[NoCoder0/feat/aicpu-urma-design](https://github.com/NoCoder0/hcomm/tree/feat/aicpu-urma-design)
的固定提交 `64ef7f9e05964add831d976876ce54adb74cd286` 加本地兼容修复。
应用使用标准接口；**hcomm 库包含修改，并非上游分支原样即可跑通**。

| 角色 | 管理 IP | RDMA IP / 设备 | 独立容器 |
|---|---|---|---|
| A3 D | 10.1.101.201 | 20.168.0.3 / device 2（卡1 Chip0） | rdma-hcomm-cann91 |
| Host3 | 10.1.101.27 | 20.168.0.19 / mlx5_2，enp165s0f0np0 | rdma-hcomm-host-cann91 |

## 审阅入口

- [npu_reader.cpp](npu_reader.cpp)：同一文件编译为 A3 控制程序和用户 AICPU kernel；开头可直接看到标准 READ、Drain、Batch 接口。
- [host_server.cpp](host_server.cpp)：Host DRAM 注册、公开接口建链、保持源 MR 到校验完成。
- [common.h](common.h)：地址、端口、opaque 描述符交换及标准 Channel 创建。
- [build_nocoder.sh](build_nocoder.sh)、[deploy_nocoder.sh](deploy_nocoder.sh)、[run_nocoder.sh](run_nocoder.sh)：容器内构建、部署、运行。

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

## 现场复现

每次先检查网络；本环境会被其他任务调整。宿主机的只读诊断：

```bash
# A3：卡侧主动 ping CPU，明确指定报文长度
/usr/local/Ascend/driver/tools/hccn_tool -i 2 -ip -g
/usr/local/Ascend/driver/tools/hccn_tool -i 2 -link -g
timeout 40 /usr/local/Ascend/driver/tools/hccn_tool -i 2 -ping -g address 20.168.0.19 pkt 64
# Host3：从对应 RDMA 网口反向 ping 卡
ping -c 3 -W 2 -I 20.168.0.19 20.168.0.3
```

两端已有构建产物，先在 Host3 启动，再在 A3 启动；程序都有300秒总超时：

```bash
# Host3（如重启导致容器停止，先 docker start rdma-hcomm-host-cann91）
docker exec rdma-hcomm-host-cann91 bash /workspace/nocoder/run_nocoder.sh host
# A3
docker exec rdma-hcomm-cann91 bash /workspace/nocoder/run_nocoder.sh a3
```

必须检查两端退出码0，以及 A3 的 `PASS: standard HcommReadOnThread`。构建和部署见结果文档。
当前固定设备/IP 在 `common.h`；若换卡，要一并核对管理地址、卡侧物理ID、RDMA IP和对应 Host 网口，
重新编译双方应用并再次做双向 ping。不要仅修改一个 IP 后沿用旧资源句柄。

## 验证边界

本次是单 QP、16字节、单段读取，验证最小单 SGE 情形。**未验证单 WR 多 SGE、最大 SGL 长度、
批量多地址传输、性能或压力场景。** 当前 Host 插件针对 A3 资源通道的兼容路径限制为1个 QP、0个用户 notify。

以前的 CANN 9.0、Host4、9.2源码兼容试验保留在历史分支和
[CONTAINER_RUN.md](CONTAINER_RUN.md)、[HOST3_CONTAINER.md](HOST3_CONTAINER.md)，其阻塞结论不代表当前版本。
