# 最小 RDMA READ Demo 实测报告

2026-09-20 22:39（北京时间），**实机通过**：NPU 卡侧主动读取鲲鹏 Host DRAM，将
`hello rdma demo` 写入 NPU HBM。当前采用一条 WR、一个 SGE，未修改 hcomm 源码。

## 两端对应关系

| 端 | 管理 IP | RDMA IP | 设备 |
|---|---|---|---|
| A3 D / NPU | 10.1.101.201 | **20.168.0.1** | 物理卡0 / Chip0 / Device0，GID index 3 |
| 鲲鹏 Host4 | 10.1.101.32 | **20.168.0.17** | mlx5_3，port 1，RoCE v2，GID index 3 |

## NPU 调用链

1. `aclrtSetDevice(0)`、`aclrtMalloc` 分配 HBM，全部写入哨兵 0xa5。
2. `rtOpenNetService` 传入 `--hdcType=6`，按 hcomm `hrtOpenTsd` 的方式启动 HCCP。
3. `RaInit`、`RaRdevInitV2` 初始化卡侧 OFFLINE + Lite；关闭后台 Lite CQ 轮询。
4. `RaTypicalQpCreate` 建 RC/OP QP，TCP 交换 QPN、PSN、GID 和 Host MR 信息。
5. `RaTypicalQpModify` 配置 NPU QP；Host 用 `ibv_modify_qp` 配置 mlx5 QP。
6. `RaMrReg` 注册 HBM；`RaSendWrV2` 提交 `RA_WR_RDMA_READ`，`bufNum=1`。
7. `rtRDMADBSend` 提交 doorbell，`RaPollCq` 等待并检查成功 READ CQE。
8. stream 同步后 D2H 回读，逐字节校验数据及未参与传输的哨兵区。

Host 使用 libibverbs 注册 DRAM，开放 remote-read。TCP 只传控制信息，字符串通过 RDMA 读取。
源字符串长15字节，加结尾 NUL 共16字节；NPU 的 HBM MR 大小4096字节。

## 实测证据

原始日志：[run_20260920_223927.json](evidence/run_20260920_223927.json)。

```text
RaMrReg(qp, &mr) rc=0
RaSendWrV2(qp, &wr, &response) rc=0
rtRDMADBSend(response.db.dbIndex, response.db.dbInfo, stream) rc=0
CQ count=1 wr_id=1 status=0 opcode=2 vendor_err=0 byte_len=16
PASS: HBM='hello rdma demo' bytes=16 SGE=1 guard_bytes=4080 unchanged
```

Host 日志确认 `QP RTS local_qpn=553 remote_qpn=34` 并收到 NPU 校验成功响应。
两端进程退出码均为0；NPU MR/QP/Rdev/RA/网络服务/HBM/stream 的清理全部返回0。

## 版本差异和验证范围

- hcomm 参考提交：`fd1ef8d1e71446a8891aae71dbed2deda57d936f`；实际运行库为现场 CANN 9.0.0。
  未修改 hcomm，因而没有新增 hcomm 修改分支。源码头文件和现场二进制并非完全同版本。
- `TsdOpen(0,1)` 不足以启动所需 HCCP 服务，初次 `RaInit` 失败；改用源码中的
  `rtOpenNetService` 流程后成功。该失败并非两台机器断网。
- `RaGetInterfaceVersion(0,46)` 返回2。现场 `RaGetQpAttr` 反汇编仅写结构体前32字节，
  不能查询新版追加的 `pathMtu` 字段。Host QP MTU=1024，NPU QP MTU未由此接口取得。
  本实验固定16字节、一个响应包，已经实测通过；大块、多包传输须先增加 MTU 查询及匹配。
- Host 网卡查询 `max_sge=30,max_sge_rd=30`。NPU 默认 Typical QP 的单 SGE READ 已证实可用；
  **不能据此声明多 SGE READ 已通过或确定 NPU 的最大 SGE 数**。双 SGE 扩展设计见 HCOMM_API_PLAN.md。
- A3 缺少 verbs 开发头，复制 Host4 的头文件到工作区 vendor/include，仅用于构建。
  未安装或替换系统驱动/库，未修改网络配置或重置整卡。

## 文件位置

- 本地：`C:/code/RDMA_DEMO/rdma_sgl_host_to_hbm/demo`
- A3 D：`/data1/z00502111/rdma_sgl_host_to_hbm/demo`
- Host4：`/home/z00502111/rdma_sgl_host_to_hbm/demo`

源码与编译运行步骤见 [demo/README.md](demo/README.md)。先启动 Host `host_server`，再启动 NPU `npu_reader`。
