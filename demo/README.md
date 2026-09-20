# Host DRAM → NPU HBM 最小 RDMA READ

## 固定实验拓扑

| 角色 | 管理地址 | RDMA 地址 | 设备 | 工作目录 |
|---|---|---|---|---|
| A3 D / NPU 0 | 10.1.101.201 | 20.168.0.1 | device 0，卡侧 RoCE | /data1/z00502111/rdma_sgl_host_to_hbm |
| 鲲鹏 Host4 | 10.1.101.32 | 20.168.0.17 | mlx5_3 / port 1 / GID index 3，RoCE v2 | /home/z00502111/rdma_sgl_host_to_hbm |

这是固定环境的实验程序。TCP 19515 只交换 QP、GID、MR 地址/rkey、长度、就绪和完成信息。
Host 将 `hello rdma demo`（含结尾 NUL，共 16 字节）写入 DRAM MR。
NPU 用 ACL 分配 4096 字节 HBM，先全部填充 0xa5，再提交一条单 SGE RDMA READ。
收到成功的 READ CQE 后，D2H 回读校验前 16 字节及后续 4080 字节哨兵。
Host 不通过 TCP 发送该字符串内容，NPU 不通过 H2D 复制该字符串。

## NPU 使用的 hcomm 接口

依次调用 `rtOpenNetService → RaInit → RaRdevInitV2 → RaTypicalQpCreate → RaTypicalQpModify → RaMrReg → RaSendWrV2 → rtRDMADBSend → RaPollCq`。
ACL 用于设备上下文、HBM 分配、stream 和最终回读；数据传输来自 NPU 卡侧 RoCE。
`disabledLiteThread=true` 避免后台线程消费手动检查的 CQE。
`aclrtSynchronizeStream` 不代替 CQ 完成检查。

hcomm 源码固定到 `fd1ef8d1e71446a8891aae71dbed2deda57d936f`，参考路径见上一级 HCOMM_API_PLAN.md。
使用原始 hcomm HCCP/Lite 头文件和现场 CANN 9.0.0 `libra.so`，没有修改 hcomm。
网络服务启动复用 hcomm `adapter_tdt.cc:hrtOpenTsd` 的流程，参数为 `--hdcType=6`。
单独 `TsdOpen(0, 1)` 没有启动所需 HCCP 服务，故采用 `rtOpenNetService`。
A3 缺少 verbs 开发头文件，因此将 Host4 的 `/usr/include/infiniband` 和 `/usr/include/rdma`
复制到工作区 `vendor/include`，只作为构建头文件，没有替换系统库或驱动。

## 编译和执行

Host4：

```bash
cd /home/z00502111/rdma_sgl_host_to_hbm/demo
bash build_host.sh
./host_server
```

A3 D（另一个终端）：

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
cd /data1/z00502111/rdma_sgl_host_to_hbm/demo
bash build_npu.sh
./npu_reader
```

两端总运行时间均有上限，TCP 收发超时 30 秒，CQ 轮询上限 15 秒。
成功路径主动释放 MR/QP/CQ/设备上下文；失败路径退出进程，由内核/驱动回收进程资源，
避免失败时主动释放仍可能在途使用的 HBM。仅适用于隔离的最小实验。

## 结果判定和 SGL 范围

必须同时看到 NPU 成功 READ CQE、HBM 内容/哨兵校验 PASS，以及两端进程退出码 0。
2026-09-20 22:39 实测通过，实际输出见 `../evidence/run_20260920_223927.json`。
这个版本使用 `RaTypicalQpCreate` 的默认单 SGE 能力，不能由它推断 NPU 支持多 SGE READ。
Host 查询到的 `max_sge=30,max_sge_rd=30` 同样不能代替 NPU 侧实测。

现场 CANN 9.0.0 `RaGetQpAttr` 经反汇编确认只写入结构体前 32 字节，不填充新版头文件中的
`pathMtu` 等扩展字段；日志中的原始零值不代表真实 MTU 为零。本程序仅使用有效的基本字段。
Host QP 使用本机 active_mtu=3（1024 字节），NPU QP MTU 由卡侧服务配置，未通过此接口查询到。
本次固定 16 字节 READ 的单包实验已通过；**不可直接扩展为大块、多包传输**，
必须先增加两端 QP MTU 查询和匹配机制。hccn_tool 返回的网络接口 MTU=8192 不等于 QP path MTU。
