# 标准 HCOMM 接口版：代码可审阅，端到端验证受 SDK 版本阻塞

2026-09-21。本版应用只通过公开 `Hcomm*` 接口管理通信资源及提交 READ；
没有调用上一版的 `HcclReadByAscendQP`，没有直接调用 RA、Runtime RDMA doorbell 或 libibverbs。
**尚未实机跑通，不能用上一版的 PASS 作为本版成功证据。**

分支：`feature/standard-hcomm-host-to-hbm`。hcomm 子模块分支：
`demo/standard-hcomm-host-to-hbm`，固定于未修改的上游提交
`fd1ef8d1e71446a8891aae71dbed2deda57d936f`。
先前已验证的扩展 READ 版本在 `feature/full-hcomm-host-to-hbm` / `e5452f1`，需切回该分支复现。

## 先看这些代码

| 文件 | 审阅内容 |
|---|---|
| [npu_reader.cpp](npu_reader.cpp) | 主机侧 main 按7个阶段组织；文件开头的 AICPU 分支直接调用标准 `HcommReadOnThread` / `HcommChannelDrainOnThread` |
| [host_server.cpp](host_server.cpp) | 无卡鲲鹏端的 Endpoint、DRAM 注册和标准 Channel 建链 |
| [common.h](common.h) | Endpoint 描述、opaque MR 交换、Channel 创建与状态推进；无手工 QP 修改 |
| [probe_endpoint.cpp](probe_endpoint.cpp) | 只创建标准 DEVICE/ROCE Endpoint 的最小能力探测 |
| [build_demo.sh](build_demo.sh) | 主机程序和用户 AICPU kernel 的独立编译目标 |

这里的“标准”指使用该固定上游版本公开头文件中的 API，不代表所有 API 已承诺稳定 ABI。
无卡鲲鹏端依赖上游 `--experimental` RoCE NIC 插件，这个实现也需要单独部署和验证。

## 建链流程

固定选用 NPU0 `20.168.0.1`，Host4 `20.168.0.17`；Host 管理地址 `10.1.101.32`。
应用控制面 TCP 使用19516端口。hcomm 自己的建链端口通过标准接口获取。

```text
Host4（HOST/ROCE，CPU）                      A3 NPU0（DEVICE/ROCE，AICPU_TS）
HcommEndpointCreate                         aclInit / aclrtSetDevice
                                            HcommEndpointCreate
分配 DRAM，写入 hello rdma demo              分配 HBM，填充 0xa5
HcommMemReg                                 HcommMemReg
HcommMemExport                              HcommMemExport
HcommEndpointGetListenPort
          ← TCP 交换 opaque MR 描述符及监听端口 →
HcommMemImport                              HcommMemImport
HcommChannelDescInit                        HcommChannelDescInit
HcommChannelCreate(SERVER, CPU)              HcommChannelCreate(CLIENT, AICPU_TS)
HcommChannelGetStatus 循环                   HcommChannelGetStatus 循环
          ← hcomm 内部完成 socket/QP 等建链工作 →
READY                                      READY
          ← 控制面确认两端均 READY →
保持源 MR 存活                              HcommThreadAlloc(AICPU_TS)
                                            ACL 启动用户 AICPU kernel
                                            HcommReadOnThread
         Host DRAM ======== RDMA READ =====> NPU HBM
                                            HcommChannelDrainOnThread
                                            同步 kernel stream 与通信 stream
                                            HBM 回拷验证数据及哨兵
          ← 只有验证通过后才互发完成确认 →
销毁 Channel / 注销 MR                      释放 Thread / Channel / MR / HBM
```

应用 TCP 不交换 QPN/PSN/rkey，也不传被测字符串。`HcommMemExport` 的结果作为 opaque 字节串交换，
由 `HcommMemImport` 解析和登记。`HcommChannelGetStatus` 会推进建链，不是一个可以省略的只读查询。

## 为什么 READ 写在 AICPU 分支

`npu_reader.cpp` 使用 `STANDARD_HCOMM_AICPU` 区分同一文件的两个编译目标：

- 默认目标在 A3 宿主机上运行，负责资源创建、ACL kernel 启动和结果验证。
- AICPU 目标导出 `StandardReadKernel`，里面直接调用 `HcommReadOnThread`。
  这是使用标准通信接口的用户 kernel，不是给 RA 换一个自定义函数名。

`HcommThreadAlloc(AICPU_TS)` 返回设备侧线程句柄，不能拿到宿主机中当普通指针调用数据面 API。
本版用标准 `HcommThreadResGetInfo` 取得对应的通信 stream，在 kernel 提交结束后同步该 stream。
`standard_read.json` 给出用户 kernel 的函数描述；用户 DSO、依赖的同版本设备库、包布局和签名/装载
仍需在匹配环境中完成验证，仅把 `.so` 与 `.json` 放在同一目录不能视为已部署。

## 已完成的验证

| 检查 | 结果 | 证据 |
|---|---|---|
| Host 程序、NPU 控制程序、AICPU kernel、探测程序的公开头文件语法检查 | `-Wall -Wextra -Werror` 通过 | [syntax_check](../evidence/standard_hcomm_syntax_check.json) |
| AICPU 用户 kernel 交叉编译及链接 | `-Wl,-z,defs` 通过；引用标准 READ/Drain，未执行 | [kernel_link](../evidence/standard_hcomm_kernel_link.json) |
| 现场 v9 库的标准 DEVICE/ROCE Endpoint 探测 | 返回1，进程退出1；ACL 初始化/清理均成功 | [endpoint_probe](../evidence/standard_hcomm_v9_endpoint_probe.json) |
| 较新上游 hcomm `--pkg --full --experimental` 构建 | 退出2，未生成完整可部署环境 | [build_failure](../evidence/standard_hcomm_build_failure.json) |
| 两端标准 Channel 建链、标准 READ、HBM 数据验证 | **未执行、未通过** | 等待配套 SDK/运行环境 |

共享库符号表中 READ/Drain 显示 `UND` 是 DSO 的动态依赖引用；这里没有定义同名替代函数。
链接成功不证明设备动态装载、通信或数据正确。

## 当前阻塞与继续方式

旧 v9.0.0 源码的 Endpoint 工厂只对 `HOST + ROCE` 创建 CpuRoceEndpoint，没有 `DEVICE + ROCE` 分支。
现场标准 API 探测与这一限制一致。较新上游提供卡侧 RoCE 和 Host-only 插件，但用现场 CANN 9.0.0
完整构建时出现：

```text
error: unknown type name '__sk__'
error: Too many arguments ... input-parameter-size ... (2952)
error: 'RES_ADDR_TYPE_NDA_URMA_DB' undeclared
```

因此没有通过修改枚举数值、混装新旧库或绕回 RA 来制造成功结果。
部分设备目标（含 libccl_kernel.so）可构建，足以完成用户 kernel 链接检查，但不能替代完整运行环境。

下一步需要提供与上述 hcomm 提交配套的 CANN Toolkit 和运行包路径。
主机库、Host-only 插件和 AICPU 设备库必须配套；Host4 当前没有 CANN 安装，也需要部署对应主机依赖。
不能仅依据一个版本号假定兼容，需重新完整构建、检查实际加载路径，并继续两端验证。

远端目录：`/data1/z00502111/rdma_sgl_host_to_hbm/standard_hcomm`。
该目录保存完整源码、代码、构建脚本和 `build.log`。环境匹配后：

```bash
# A3：先构建 hcomm；CANN 指向配套 SDK，不覆盖系统目录。
CANN=/path/to/matching/cann bash build_hcomm.sh
# 明确指定匹配的设备库目录，再编译 demo；路径缺失会直接失败。
CANN=/path/to/matching/cann HCOMM_DEVICE_LIB=/path/to/device/lib bash build_demo.sh
```

待配套设备 kernel 装载流程完成并核验后，先在 Host4 启动 `host_server`，
再在 A3 启动 `npu_reader /absolute/path/to/standard_read.json`。
成功标准仍为16字节精确匹配、4080字节哨兵不变、两端退出码0。

## 标准接口依据

- 固定上游的 [Endpoint 工厂](../reference/hcomm/src/base_comm/resources/endpoints/endpoint.cc)
  和 [Channel 工厂](../reference/hcomm/src/base_comm/resources/endpoint_pairs/channels/channel.cc)。
- [HcommChannelGetStatus](../reference/hcomm/docs/zh/api_ref/comm_opdev/control_plane_api/basic_resource_mgmt/HcommChannelGetStatus.md)：建链推进和状态值。
- [HcommChannelDrainOnThread](../reference/hcomm/docs/zh/api_ref/comm_opdev/data_plane_api/cpu-cpu_ts-aicpu_ts/communication_operations/HcommChannelDrainOnThread.md)：A3 AICPU_TS RoCE 完成等待。
- [Host-only 插件](../reference/hcomm/experimental/base_comm/nic_plugin/README.md)：插件构建和加载规则。
- [上游构建说明](../reference/hcomm/docs/zh/build/build.md)：master 需要配套 CANN 开发环境。
- [官方较新版本 API 文档](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/commlib/commopdev/docs/zh/api_ref/comm_opdev/data_plane_api/cpu-cpu_ts-aicpu_ts/communication_operations/HcommBatchTransferOnThread.md)
  列出了 A3 RoCE 支持；不将较新文档的支持范围套用到现场 v9 库。
