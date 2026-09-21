# 独立 CANN 9.1 容器运行记录

2026-09-21。A3 管理地址 `10.1.101.201`。

## 环境与隔离范围

原容器 `hhy` 的可写层通过 `docker commit --pause=false` 保存为
`rdma-hcomm-hhy-snapshot:20260921`，镜像 ID 为
`5b234a7c453605fc3e4358ad0b778f365fea8b52865429ff068d6876e67d3382`。
新容器为 `rdma-hcomm-cann91`，保留 CANN `/usr/local/Ascend/cann-9.1.0`。

宿主机工作目录：
`/data1/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91`；容器内为 `/workspace`。
没有沿用原容器的 `/home`、`/data`、`/opt` 等宽范围可写挂载。
驱动、固件和 hccn 配置只读挂载。容器使用 host 网络和 privileged 模式，
因此 NPU、RDMA 网卡和网络端口仍是共享硬件资源，并非独占设备。

```bash
docker exec -it rdma-hcomm-cann91 bash
cd /workspace
source /usr/local/Ascend/cann-9.1.0/set_env.sh
```

创建脚本为 [create_container.sh](create_container.sh)，已有同名容器或镜像时拒绝重建。
Docker commit 不包含 bind mount；新工作区中的源码和 demo 单独复制。

## 构建记录

上游基线为 `fd1ef8d1e71446a8891aae71dbed2deda57d936f`，兼容改动提交 `5843761d`。
源码在 `/workspace/source`；使用 [build_in_container.sh](build_in_container.sh)
构建主机库、设备库以及 experimental Host-only RoCE 插件。

CANN 9.1 已解决先前 9.0 下的 `__sk__`、AIV 参数和
`RES_ADDR_TYPE_NDA_URMA_DB` 编译错误。但这份较新上游仍有两处 SDK 兼容问题：

- 新 IPC link-selection 枚举仅在 9.2 头文件中存在。旧头文件直接使用已有的
  `OpenIpcMemoryLegacy()`，不补造枚举数值。
- Runtime 9.1 只有 `aclrtSetExceptionInfoCallback`。本实验分支恢复该公开 API 的
  注册/清空语义；不模拟新版本多回调接口。集合通信和多回调并存场景未验证。

上述兼容改动不修改公开 `Hcomm*` 头文件，也不增加自定义 READ 或手工 QP 建链接口。
Release 符号隐藏使上游 experimental 插件链接失败，因此采用上游 Debug 构建选项。
`build.log`、`build_compat.log`、`build_compat2.log` 保留失败记录；
`build_debug.log` 记录最终构建尝试，`build.exit` 保存其退出码。

最终全量构建退出码为 **0**，产出 `cann-hcomm_9.2.0_linux-aarch64.run`。
注意：这是 hcomm 9.2 源码加 CANN 9.1 兼容改动的实验组合，不能称为官方配套的 9.1 发布包。
demo 的 Host 程序、NPU 控制程序、AICPU 用户 kernel 均完成编译及实际链接。
原始 Debug 设备包 196 MB，首次加载探测90秒超时；运行副本去掉调试段并更新校验值后为
8.2 MB，设备包加载和后续 NPU 资源探测通过。原始 Debug 构建产物仍保留。

## 已核实的运行能力

新容器原有 CANN 9.1 库的最小探测：ACL 初始化、选卡、
`HcommEndpointCreate(DEVICE, ROCE, 20.168.0.1)`、销毁 Endpoint、ACL 清理均返回 0。
这是 Endpoint 能力验证，不能替代标准 Channel 建链和 RDMA READ 的数据验证。

NPU0 的现有 custom-op-secverify-mode 为 0；本次只查询，未更改硬件验签设置。
容器与探测证据见 [container evidence](../evidence/standard_hcomm_cann91_container.json)。

## 当前运行结果

| 验证项 | 结果 |
|---|---|
| 源码构建版 NPU DEVICE/ROCE Endpoint 创建和释放 | 全部返回0，进程退出0 |
| `HcommThreadAlloc(AICPU_TS)` | 返回0，实际执行配套设备初始化 kernel |
| `HcommThreadResGetInfo` 获取通信 stream | 返回0，stream 非空 |
| `HcommThreadFree` 与 ACL 清理 | 全部返回0，进程退出0 |
| Host4 装载标准 RoCE NIC 插件 | 日志确认 `hcomm_cpu_roce` 处理 ROCE 协议 |
| Host4 `HcommEndpointCreate` | 返回4，程序退出1 |
| 两端 Channel 建链、用户 READ kernel、HBM 数据验证 | **未执行、未通过** |

原始结果见 [validation evidence](../evidence/standard_hcomm_cann91_validation.json)。
Host4 的真实库依赖位于
`/home/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91/host_bundle`，
`libraries.json` 记录实际来源与部署哈希；没有安装到 Host4 系统目录，也没有使用 HAL stub。

复现部署时，`deploy_in_container.sh` 会生成 `/workspace/host_bundle`；将该目录打包后，
通过 A3 到 Host4 的管理网直接传输，再在上述 Host4 工作目录解压即可。
传输包只包含 Host 程序、真实依赖库、RoCE 插件和运行脚本，不包含登录凭据。

Host4 失败链路已由运行日志和源码共同确认：

```text
HcommEndpointCreate(HOST, ROCE)
  → experimental CpuRoceEndpoint::Init
  → InitHostPeerRaOnce / RaInit(PEER)
  → RaPeerInit / RsInit
  → RsInitRscbCfg / RsGetChipLogicId
  → DlDrvDeviceGetIndexByPhyId(chipId=0) = -7
  → -ENODEV(-19) → RaInit 228002 → HcommEndpointCreate 4
```

`src/base_comm/resources/hccp/rdma_service/rs.c` 中 `RsGetChipLogicId`
无条件查询 NPU 驱动，未按 PEER/纯 Host 模式分支；后续还会查询 NPU product type。
因此当前这份 experimental 插件及 HCCP 组合没有完成本机无卡鲲鹏场景的初始化支持。
不是缺少插件文件或 RDMA 网卡，也不是两端 IP、应用 TCP 端口或 QP 建链超时。
继续完成目标需要配套的纯 Host HCCP 实现，或补齐该后端的真实 CPU/NIC 资源路径；
不能把逻辑设备查询强行返回成功、伪造 NPU 型号或改回应用直接 verbs 作为本版通过结果。

复现资源探测：

```bash
# A3：在自己的容器中执行，测试结束会释放已创建资源。
docker exec rdma-hcomm-cann91 bash /workspace/run_npu.sh --probe-thread

# Host4：失败诊断写入标准输出，当前预期返回1。
cd /home/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91/host_bundle
bash run_host.sh --probe-endpoint
```

`host_server` 与 `npu_reader` 的完整模式仍保留公开 Hcomm 建链/读接口与逐阶段注释。
当前没有任何标准接口端到端 READ 的 PASS；旧分支的成功结果不用于替代此次验证。
