# NPU 端接口方案：依据 hcomm 源码

更新：2026-09-20。单 SGE 版本已完成 HBM 注册和真实 RDMA READ，详见 [实测报告](RESULT.md)。
下文保留双 SGE 设计；实际最小版本使用 `RaTypicalQpCreate`，启动阶段采用 `rtOpenNetService`。
现场 `RaGetQpAttr` 不填充新版头文件的 MTU 字段，不能用于确认 MTU 匹配，详见报告中的范围限制。
参考源码：[hicann/hcomm](https://github.com/hicann/hcomm/tree/fd1ef8d1e71446a8891aae71dbed2deda57d936f)，固定提交 `fd1ef8d1e71446a8891aae71dbed2deda57d936f`，已拉取到本工作区 `reference/hcomm`。
该源码版本不自动等于现场 CANN 9.0.0 的实现；部署前仍要验证接口版本及结构体 ABI。

## 选定方案

**NPU 端从 A3 宿主机程序调用 ACL + RA/HCCP（libra.so）+ Runtime doorbell；Host4 使用 libibverbs。**
复用 hcomm 的底层操作顺序，首版不依赖集合通信域或自定义 AI Core/AICPU 算子。
数据由 NPU 卡侧 RoCE 引擎直接写入 HBM，CPU 只做资源管理、提交和校验。

| 步骤 | 拟用接口 | 用途 |
|---|---|---|
| 设备与HBM | aclInit、aclrtSetDevice(0)、aclrtCreateStream、aclrtMalloc | 选设备0，申请16 KiB HBM |
| 初始化卡侧RDMA | RaInit、RaRdevInitV2 | 按实际物理设备号和20.168.0.1初始化DEVICE/OFFLINE路径及Lite支持；必要TSD初始化沿hcomm流程 |
| 创建QP | **RaQpCreateWithAttrs** | RC、OP模式；显式max_send_sge=2，max_recv_sge=1；请求较小SQ/RQ/CQ深度 |
| 获取和交换QP信息 | RaGetQpAttr + VPC面TCP | 获取QPN、PSN、GID、gidIdx等，与Host4交换 |
| 建链 | **RaTypicalQpModify** | 用本地/远端TypicalQp配置卡侧QP；Host4用ibv_modify_qp |
| 注册HBM | **RaMrReg** | 输入HBM地址、大小和本地写权限，取得lkey；一块MR覆盖两个目标区间 |
| 提交SGL READ | **RaSendWrV2** | SendWrV2.op=RA_WR_RDMA_READ，bufNum=2，两个SgList |
| 发doorbell | **rtRDMADBSend** | 使用SendWrRsp.db中的dbIndex/dbInfo，经stream触发执行 |
| 完成与校验 | **RaPollCq**、ACL stream同步、aclrtMemcpy | 使用匹配ABI的CQE结构检查status/wr_id，确认完成和可见性后D2H验证 |
| 释放 | RaMrDereg、RaQpDestroy、对应Rdev/RA与ACL清理 | 确认在途访问终止后释放资源 |

具体参数值须从匹配版本头文件读取，不将上表直接当作可编译初始化代码。
如采用Rdev级注册，可选择RaRegisterMr/RaDeregisterMr；首版优先减少分支，采用QP级RaMrReg。
不会将Ra句柄强转为Host侧ibv_qp*，也不会将HBM注册到201的宿主机网卡。

## 关键源码证据

以下路径相对 `reference/hcomm/`，行号按固定提交：

1. **QP显式能力与无socket耦合建链**：
   `src/legacy/ascend910/platform/common/adapter/adapter_hccp.cc:3505`
   的CreateQpWithDepthConfig先构造QpExtAttrs，调用RaQpCreateWithAttrs，再用RaGetQpAttr取建链参数。
   3533–3538行明确检查RaGetInterfaceVersion：TYPICAL_QP_MODIFY opcode=46，版本至少2，才支持该组合。
   因此先检查版本；不满足时报告当前方案不支持，不能盲目混用接口。

2. **默认QP并不等于支持多SGE**：
   `src/base_comm/resources/hccp/rdma_agent/hdc/ra_hdc_rdma.c:407`
   中RaHdcTypicalQpCreate设置max_send_sge=QP_DEFAULT_MIN_CAP_SEND_SGE；
   `inc/private/network/ra_rs_comm.h:246` 定义该常量为1。
   对比RaHdcQpCreateWithAttrs在 `ra_hdc_rdma.c:193` 传递调用者申请的max_send_sge。
   因此首版不直接沿用默认TypicalQpCreate配置。

3. **一条WR可以携带列表，源码不会主动拆成两条WR**：
   `src/base_comm/resources/hccp/rdma_agent/hdc/ra_hdc_rdma.c:675`
   的RaHdcSendWrV2把bufList/bufNum、op、rkey、wrId传给RaHdcLiteTypicalSendWr。
   `rdma_agent/hdc/ra_hdc_lite.c:1308` 构造一个rdma_lite_send_wr，
   1331行循环复制每个SGE的地址、长度和lkey，1337行设置num_sge，随后调用一次RaRdmaLitePostSend。
   这证明软件表达路径存在，不证明现场硬件已接受2-SGE READ。

4. **发送返回后还要doorbell**：
   `src/legacy/ascend910/platform/typical/send_recv_executor.cc:581`
   的RdmaSendAsync先调用HrtRaSendWrV2，再读取返回的dbIndex/dbInfo并调用hrtRDMADBSend。
   `platform/common/adapter/adapter_rts.cc:2526` 中该封装调用rtRDMADBSend。
   本demo保持这个顺序，不能只调用RaSendWrV2然后宣称完成。

5. **完成查询有模式和Lite支持限制**：
   `src/base_comm/resources/hccp/rdma_agent/hdc/ra_hdc_rdma.c:1348`
   的RaHdcPollCq仅在对应OP模式及Lite可用时走RaHdcLitePollCq，否则返回不支持。
   使用匹配版本的rdma_lite_wc_v2布局；初始化时处理CQ自动poll线程与显式poll的关系，
   不能让后台线程先消费demo要验证的CQE。stream同步只证明相关stream任务的状态，仍必须确认RDMA完成。

## 首版提交内容（示意，非可运行代码）

```cpp
SgList sgl[2] = {
    {hbm_base,        4096, hbm_lkey},
    {hbm_base + 8192, 4096, hbm_lkey}
};
SendWrV2 wr{};
wr.wrId = 1;
wr.bufList = sgl;
wr.bufNum = 2;
wr.op = RA_WR_RDMA_READ;
wr.dstAddr = host_mr_addr; // READ时是远端源地址
wr.rkey = host_mr_rkey;
wr.sendFlag = RA_SEND_SIGNALED;

// 检查每个返回值，失败即退出该次操作
RaSendWrV2(qp, &wr, &rsp);
rtRDMADBSend(rsp.db.dbIndex, rsp.db.dbInfo, stream);
// 检查doorbell提交/stream状态，并有界RaPollCq等待成功CQE
// 完成后aclrtMemcpy回读16 KiB，验证两段数据及两个哨兵区
```

Host4提供连续8 KiB数据；本地HBM的两个4 KiB目标区间离散。K=1基线通过后再验证K=2。

## 能力边界与高层接口取舍

- `RaGetQpAttr` 的 `QpAttr`（hccp_common.h:636）包含QPN/GID/MTU等，**没有max_send_sge**。它不能单独完成SGE能力查询。
- `ra_hdc_lite.h:23` 的RA_SGLIST_MAX=16只是该源码封装的数组/循环界限，不是NPU硬件上限。发送循环最多复制16项；demo固定2项并先自行检查，不能任意传30项。
- `HcommReadOnThread(thread, channel, dst, src, len)` 是更高层的单区间读接口（include/hcomm_primitives.h:291）；其参数没有SGE列表。
  两次调用或批量描述符不能自动证明“一条WR、两个SGE”。本demo要验证这个底层事实，所以选RaSendWrV2。
- 本次源码阅读将“接口完全未知”缩小为“已有明确软件路径，需对齐现场ABI/接口版本并实测HBM与READ”。
  在K=2实际成功CQE、逐字节校验和哨兵校验通过前，仍不声明NPU支持该模式。
