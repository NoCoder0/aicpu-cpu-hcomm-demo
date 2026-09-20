# 完整 hcomm 版本（实机通过）

2026-09-20 23:24（北京时间）实机验证通过：Host4 DRAM → NPU0 HBM，
`hello rdma demo` 含 NUL 共16字节，单 WR / 单 SGE，两端退出码0。
见 [实测报告](RESULT.md) 和 [原始日志](../evidence/full_hcomm_run_20260920_232456.json)。

## Git 分支

- 已验证 RA/HCCP 基线：`baseline/ra-hccp-verified`，提交 `af0719f`。
- 新实验：外层仓 `feature/full-hcomm-host-to-hbm`。
- hcomm 子模块：`demo/cann9-full-hcomm`，基于上游标签 `v9.0.0`（`09a304742994c603879a57291579afd52c3126d9`）。
- hcomm 修改提交：`734e14ab24b9269e497993e1ba8a1bdeca4f617d`。
- 初始参考的 master 提交仍保存在基线子模块指针中。

本目录构建完整 hcomm 的主机和设备包，NPU demo 使用构建产物中的异构通信接口。
鲲鹏 Host4 继续使用 libibverbs 提供可读取 DRAM，管理网 TCP 负责交换 QP/MR 元数据。
不将纯 CPU Host4 伪装成有 NPU 的 HCCL rank。

## 接口选择

v9.0.0 的通用 HcommEndpoint 路径未实现 A3 DEVICE+RoCE 组合，因此使用同仓库
`src/platform/typical` 已有异构通信接口：

`hcclCreateAscendQP → hcclModifyAscendQPEx → hcclRegisterMem`。

在该层添加实验性 `HcclRdmaInitForRead` 和 `HcclReadByAscendQP`，后者在库内调用
RA、Runtime 并校验 CQE。demo 不直接调用 `Ra*` 或 `rtRDMADBSend`。
初始化通过重载传递 `disabledLiteThread=true`，避免后台线程消费显式等待的 CQE；
原有初始化和 StartNic 签名仍保留，默认继续使用自动轮询。

## 建链与数据路径

```text
A3 10.1.101.201 / NPU0                         Host4 10.1.101.32
HcclRdmaInitForRead                            ibv_open_device("mlx5_3")
  NetworkManager / HCCP 初始化                  ibv_reg_mr(DRAM, REMOTE_READ)
hcclAllocWindowMem → hcclRegisterMem(HBM)       ibv_create_qp(RC)
hcclCreateAscendQP                             QP INIT
                 TCP :19515 交换 QPN / PSN / GID / Host MR
hcclModifyAscendQPEx                           ibv_modify_qp → RTR → RTS
                 等待 Host READY
HcclReadByAscendQP
  HrtRaSendWrV2(READ, 1 SGE) + hrtRDMADBSend
  20.168.0.1 <======= 16 bytes RoCE READ ======= 20.168.0.17
  hrtRaPollCq → 成功 READ CQE
aclrtMemcpy(HBM → CPU，仅用于结果检查)
                 TCP PASS；销毁 QP，再注销 MR 和释放内存
```

源码入口：

- [demo](npu_reader.cpp)：调用 hcomm、交换连接参数、检查数据和哨兵。
- [异构接口](../reference/hcomm/src/platform/typical/interface_hccl.cc)：创建、修改、销毁 QP，注册 HBM MR。
- [QP 管理](../reference/hcomm/src/platform/typical/typical_qp_manager.cc)：保存 QPN 到 RA QP handle 的映射，调用 RA 修改接口。
- [READ 扩展](../reference/hcomm/src/platform/typical/typical_read.cc)：参数检查、READ 提交、doorbell、CQ 检查。
- [资源初始化](../reference/hcomm/src/platform/typical/rdma_resource_manager.cc)：启动卡侧网络和手动 CQ 轮询模式。

## 构建与复现

工作区分别为 A3 `/data1/z00502111/rdma_sgl_host_to_hbm/full_hcomm`、
Host4 `/home/z00502111/rdma_sgl_host_to_hbm/full_hcomm`。
以下命令在对应服务器的工作区内执行；本地 Windows 保存源代码和 Git 分支。

1. 将本分支 `reference/hcomm` 的完整源码放到 A3 工作区的 `source/`，
   将本目录的脚本和 `npu_reader.cpp` 放到工作区，保留上级 `demo/control.h`。
   子模块必须包含本分支 READ 扩展，不能只取未经修改的 v9.0.0。
2. A3 执行 `bash build_remote.sh`。它运行上游 `build.sh --pkg --full`，构建设备和主机包；
   Python wheel 安装到工作区 `python_deps/`，不会修改系统 Python 包。
   首次构建需要联网获取 hcomm 的上游依赖，AIV 编译较耗时。
3. A3 执行 `bash build_incremental.sh`，完成主机打包，并用 CMake 安装到本目录 `deploy/`。
   部署目录中的主机库由当前源码构建；CANN Runtime、驱动和正在运行的卡侧系统服务仍来自现场环境。
   完整构建生成的设备包保留在构建目录，不覆盖现场设备服务。
4. A3 执行 `bash build_demo.sh`。链接 `deploy/hcomm/lib64` 中的 `libhcomm.so` 和 `libhccl_plf.so`。
5. Host4 复制 `../demo/{host_server.c,control.h,build_host.sh}` 到其工作区，执行
   `bash build_host.sh && ./host_server`。
6. A3 执行 `bash run_npu.sh`。它设置本次构建库的搜索路径和手动 READ 所需环境变量。

程序输出符号所在库及 `/proc/self/maps` 中实际加载的通信库路径。
通过标准是 READ CQE 成功、16 字节内容精确匹配、4080 字节哨兵不变、两端退出码均为0。
只看到发送函数返回0或 QP 建立成功不算通过。

## 限制

- 仅限独立 demo 进程、同线程同设备，初始化前不得已有其他 HCCL 资源。
- 每个 QP 仅允许一个在途 READ；调用者串行调用并保持两端 MR 有效。
- 本次16字节；接口限制为1至256字节单包 READ，未实现大块传输 MTU 匹配。
- 仍为单 SGE，不代表验证了多 SGE。
- CQ 错误或超时后必须先停止/销毁 QP，再释放目标 MR；demo 失败退出进程。

## 状态

本版已完成全量构建、工作区部署、新接口参数检查及端到端实机验证。
根目录 RESULT.md 的成功日志属于保留的 RA/HCCP 基线；本版证据在本目录 RESULT.md。
