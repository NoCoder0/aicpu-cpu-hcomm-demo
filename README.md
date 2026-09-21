# Host DRAM → NPU HBM：最小 RDMA SGL Demo 可行性

**当前分支：`feature/standard-hcomm-host-to-hbm`。已从 hhy 复制独立 CANN 9.1 容器，
完成 hcomm 全量构建、demo 编译和 NPU Endpoint 实测。Host4 的标准 RoCE 插件加载成功，
但底层 HCCP 仍要求查询 NPU 逻辑设备号，导致无卡 Host 初始化失败；标准 READ 尚未跑通。**
审阅入口：[标准接口代码与建链说明](standard_hcomm/README.md)。
环境、兼容改动和运行证据：[独立容器记录](standard_hcomm/CONTAINER_RUN.md)。

**上一分支结果：含自定义 READ 扩展的完整 hcomm 源码构建版于 2026-09-20 23:24 实机通过。**
见 [完整 hcomm 说明](full_hcomm/README.md) 和 [本版实测报告](full_hcomm/RESULT.md)。
原有 RA/HCCP 版本保存在 `baseline/ra-hccp-verified`（`af0719f`）；
扩展 READ 分支为 `feature/full-hcomm-host-to-hbm`。以下内容保留首版结果和早期可行性设计。

日期：2026-09-20。工作区：`C:/code/RDMA_DEMO/rdma_sgl_host_to_hbm`。
**最新状态（22:39）：单 SGE 的 Host DRAM → NPU HBM RDMA READ 已实机跑通。**
HBM 中读到 `hello rdma demo`（含 NUL 共16字节），成功 CQE，4080字节哨兵不变，两端退出码0。
见 [实测报告](RESULT.md)、[源码与运行说明](demo/README.md)、[原始日志](evidence/run_20260920_223927.json)。
按照后续先跑通单段读取的要求，实际首版使用 `RaTypicalQpCreate`，未修改 hcomm。
多 SGE / 大块多包传输尚未验证。

**以下保留最初的双 SGE 可行性设计，描述的是后续扩展，不代表已经完成的实现。**

补充：已参考固定版本 hcomm 源码，选定 `RaQpCreateWithAttrs → RaTypicalQpModify → RaSendWrV2 → rtRDMADBSend → RaPollCq` 路径。默认 Typical QP 的 Lite 发送 SGE 配置为1，需显式申请2并检查驱动接口版本；详见 [NPU接口方案](HCOMM_API_PLAN.md)。

## 结论

**协议层面方案成立，现场具备部分条件，可以进入最小验证；当前不能宣布端到端可用。**
首版只做一条 RC QP、一次 RDMA READ、2 个本地 SGE，将鲲鹏连续 DRAM 数据直接读入 NPU 的两个不连续 HBM 区间。

还必须验证三件事：NPU HBM 能被卡侧 RDMA 注册；NPU QP 能与 Host4 的 mlx5 QP 互通；NPU 当前驱动/QP 支持 READ 的单 WR 多 SGE。
Host 网卡的“30 段”不是 NPU 的能力，也不是本实验必须达到的目标。

## 选定设备与现场证据

| 项目 | A3 D / NPU 端 | 鲲鹏 Host4 端 |
|---|---|---|
| 管理 / VPC IP | 10.1.101.201 | 10.1.101.32 |
| RDMA IP | 20.168.0.1/24 | 20.168.0.17/24 |
| 设备 | 物理卡0、Chip0，hccn_tool设备0 | mlx5_3，端口1 |
| Linux RDMA 网口 | 卡侧端口，不是宿主机 enp162s0f0 | enp165s0f1np1 |
| 本次查询状态 | Link UP；活动QP数量0 | 网口UP；NPU邻居REACHABLE；仅系统GSI QP |
| SGE能力 | NPU卡侧尚未取得 | max_sge=30，max_sge_rd=30 |
| 软件 | toolkit路径指向cann-9.0.0；存在libra.so | 存在gcc及infiniband/verbs.h |
| 网卡固件 | 卡侧版本尚未采集 | 28.47.1026 |

201 的普通 `ibv_devinfo` 返回 `rocep162s0f0/1` 的16段上限；`rdma link` 将它们映射到宿主机 enp162s0f0/1，**不能用来证明NPU卡侧的SGE上限**。
本次 NPU 到 20.168.0.17 的 hccn_tool ping 在12秒外部超时后退出（124），未得到成功结果；Link和邻居记录不替代端到端READ验证。

## “SGL读取”的精确定义

本设计的源端Host内存连续，目标HBM地址离散。标准READ WR只有一个远端地址/rkey，SGE列表描述发起端的本地目标内存。
若需求改为“从Host多个不连续源地址一次gather”，普通单条READ WR无法直接表达，需要另做协议设计。[标准WR定义](https://man7.org/linux/man-pages/man3/ibv_post_send.3.html)

```text
Host4 DRAM（注册8 KiB，允许REMOTE_READ）
  [ A:4 KiB ][ B:4 KiB ]
           |
           | NPU发起一条RDMA READ，2个SGE，总长8 KiB
           v
NPU HBM（分配并注册16 KiB，允许本地写）
  [ A:4 KiB ][ 哨兵:4 KiB ][ B:4 KiB ][ 哨兵:4 KiB ]
   offset=0                offset=8192
```

HBM可先申请一个大块再选两个间隔区间，只需一个MR/lkey；这仍是真实的非连续本地SGL，不要求申请两个独立物理内存块。
首版CPU发起控制和提交即可，不要求AI Core算子发起RDMA。

## 最小实现路径

计划分两个可执行文件：`host_server` 与 `npu_reader`。它们尚未创建。
首版固定1个QP、1个在途READ，队列深度取较小值（例如16），不做流水线、带宽测试或30段扩展。

1. **Host server**：使用libibverbs，绑定mlx5_3/1；申请页对齐DRAM，按位置生成可校验数据，注册允许REMOTE_READ的MR；创建RC QP，确认目标端READ权限及responder资源。
2. **NPU reader**：初始化ACL并选设备0；申请16 KiB HBM，全部填充哨兵；使用与现场驱动匹配的卡侧RA/HCCP接口注册HBM并获得有效地址/lkey，创建支持READ的卡侧QP。
3. **建链和元数据**：在VPC面通过TCP交换版本、长度、RDMA地址/GID、QPN、PSN、MTU、READ资源及Host MR地址/rkey；具体卡侧建链API必须先验证，不假定RA内部握手与自定义verbs握手兼容。RoCE版本/GID index根据现场查询选择，不硬编码。
4. **提交**：NPU提交一条READ，远端地址为Host MR起点，本地SGE为HBM+0和HBM+8192，各4096 B。逐个填入正确的地址/长度/lkey；不使用Host DRAM作为数据中转。
5. **完成与验证**：使用所选QP模式对应的完成机制，检查成功状态和wr_id；若模式需要额外doorbell/stream提交，执行完整流程，不能把发送接口返回0当作传输完成。完成且数据对ACL可见后，将HBM拷回CPU做逐字节验证，检查两个哨兵区未变化。
6. **结束**：通过TCP通知Host验证结果；操作完成后注销MR、释放QP/CQ/内存并退出。异常路径设置超时并停止后续提交，先终止在途访问再释放其资源。

HBM→CPU只用于最终验证，不属于被测Host→HBM传输路径。
禁止把HBM指针直接交给普通Host侧 `ibv_reg_mr` 并假定它可注册；必须验证对应NPU内存注册机制。

## 现有代码能复用什么

本地MemFabric参考版本：`4b4d6f0b`，仅阅读，未修改共享源码。

- [device_rdma_transport_manager.cpp](../memfabric_hybrid/src/hybm/csrc/transport/device/device_rdma_transport_manager.cpp)：`RemoteIO()` 有READ路径，但当前 `wr.buf_num=1`。其地址修正函数也只处理第一个SGE；不能只把1改成2。
- [dl_hccp_def.h](../memfabric_hybrid/src/hybm/csrc/under_api/dl_hccp_def.h)：`send_wr_v2` 含 `buf_list/buf_num/dst_addr/rkey/op`。结构有列表字段不证明当前驱动接受多段READ。
- [dl_hccp_api.h](../memfabric_hybrid/src/hybm/csrc/under_api/dl_hccp_api.h)：可参考MR/QP/发送接口封装；现场ABI仍需核对。
- 现场 `libra.so` 导出 `RaTypicalQpCreate/Modify`、`RaTypicalSendWr`、`RaGetQpAttr`、`RaMrReg`、`RaPollCq` 等符号。后续已从参考hcomm源码取得接口定义和调用链，见 `HCOMM_API_PLAN.md`；这些头文件与现场库的ABI匹配仍是实现前置项，不能仅凭符号名判断兼容。
- 原有HCOM/CPU SGL性能demo不作为NPU SGL已实现的证据；也不将HIXL的批量拷贝自动视为单WR多SGE。

## 验证顺序与通过标准

| 阶段 | 最小验证 | 判定 |
|---|---|---|
| 0：接口与能力 | 取得匹配驱动的RA头文件/说明；查询或验证QP能力及HBM注册 | 不猜ABI；能力不明时标记UNKNOWN |
| 1：单段基线 | K=1，从Host读4096 B到HBM并校验 | 证明QP互通、HBM注册与READ完成路径 |
| 2：SGL目标 | K=2，一条WR读8192 B到上述两个HBM区间 | 两段逐字节一致，所有哨兵不变 |
| 3：复核 | 换数据种子重复10次，记录QP和实际提交 | 数据WR为1条且SGE为2，无隐藏分拆或DRAM中转 |

日志至少包括设备/IP、实际QP标识、READ能力来源、opcode、WR数量、SGE数量及长度、完成状态、字节校验与哨兵结果。
QP创建成功后读取实际能力；申请参数不充当实际返回值。[设备能力定义](https://man7.org/linux/man-pages/man3/ibv_query_device.3.html)

若K=1成功而K=2不受支持，结论应为“Host→HBM READ可用，但当前路径的单WR多SGE不可用/未证实”，不得把两条单段READ包装为SGL成功。
后续实现首先解决NPU接口、QP互通与HBM注册，再编写这两个最小程序；本阶段不启动业务、不重启设备、不更改网络配置。

## 记录

`evidence/` 保存本次只读查询的原始输出和命令。密码不写入工作区。
