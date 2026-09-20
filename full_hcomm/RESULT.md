# 完整 hcomm：Host DRAM → NPU HBM 实测报告

2026-09-20 23:24（北京时间），实机通过。
本次完整构建 hcomm v9.0.0，加上平台异构通信层的最小 READ 扩展，
使用新生成的库完成了 NPU 主动 RDMA READ。

| 项目 | A3 D / NPU0 | 鲲鹏 Host4 |
|---|---|---|
| 管理 IP | 10.1.101.201 | 10.1.101.32 |
| RDMA IP | **20.168.0.1** | **20.168.0.17** |
| 设备 | 物理卡0 / Chip0 / Device0，GID index 3 | mlx5_3，port 1，GID index 3，RoCE v2 |
| 本次 QPN | 42 | 555 |
| 内存 | hcomm 分配并注册的4096字节 HBM | libibverbs 注册的4096字节 DRAM |

## 通过证据

[本次原始日志](../evidence/full_hcomm_run_20260920_232456.json)：

```text
HcclRdmaInitForRead() rc=0
hcclRegisterMem(&mr) rc=0
hcclCreateAscendQP(&local) rc=0
hcclModifyAscendQPEx(&local, &remote, &qos) rc=0
HcclReadByAscendQP(&local, &destination, &source, 15000, stream, &completion) rc=0
HCOMM READ CQ wr_id=1 status=0 opcode=2 byte_len=16 vendor_err=0
PASS: source-built hcomm HBM='hello rdma demo' bytes=16 SGE=1 guard_bytes=4080 unchanged
```

两端退出码均为0。NPU 侧 QP 销毁、MR 注销、HBM 释放、网络资源释放和 ACL 清理均返回0。
Host 收到 PASS 后释放资源。TCP 仅交换连接参数和状态，字符串由 RDMA 数据通道读取。
HBM 回拷 CPU 仅用于检查结果。

同一次运行还验证 READ 接口拒绝：空 QP 指针、零长度、257字节长度、地址溢出、零超时。
这些参数检查均在提交 WR 之前返回错误。

## 完整 hcomm 的构建与使用范围

- 基于 `https://github.com/hicann/hcomm.git` 的 `v9.0.0`，与现场 CANN 9.0.0 配套。
- hcomm 修改提交：`734e14ab24b9269e497993e1ba8a1bdeca4f617d`。
- 使用上游 `build.sh --pkg --full` 构建设备、HCCD 和主机部分，修复构建依赖后增量完成。
- 生成完整安装包 `source/build_out/cann-hcomm_9.0.0_linux-aarch64.run`（约52 MB），
  设备产物包括 `aicpu_hcomm.tar.gz` 和 `cann-hcomm-compat.tar.gz`。
  [安装包 SHA-256 与首次失败原因](../evidence/full_hcomm_package_and_first_error.json) 已留存。
- 用 `cmake --install` 将主机库部署到本工作区的 `deploy/hcomm/lib64`。
  本次未执行系统安装包，也未替换现场驱动、CANN Runtime 或卡侧系统服务。
- 符号解析、动态依赖和运行时映射确认 `libhcomm.so`、`libhccl_plf.so`、`libra.so`、
  `libra_hdc.so` 均加载自该部署目录。完整依赖和 SHA-256 见
  [构建与库证据](../evidence/full_hcomm_build_and_libraries.json)。
- 9个修改文件与远端编译源码逐文件比对通过（忽略 CRLF/LF），见
  [源码核对记录](../evidence/full_hcomm_source_verification.json)。
- demo 只调用 hcomm/ACL。建链由 hcomm 原有 `hcclCreateAscendQP` / `hcclModifyAscendQPEx` 实现；
  READ 由新增 `HcclReadByAscendQP` 在 hcomm 库内部完成。
  READ 扩展是实验性接口，不声称上游已有通用 `HcommRead` 支持本机型。

## 修改范围与已知限制

在 `src/platform/typical` 新增 READ 源码和头文件；增加显式 CQ 轮询初始化选项，
原有初始化默认行为保持不变。对外头文件改为显式依赖，方便 demo 使用；补齐实现文件依赖。
未修改 legacy 代码。

仅验证一个 QP、一个在途 READ、一个 SGE、16字节字符串。
接口限制1至256字节单包传输，尚未解决大块多包 MTU 匹配，也未验证多 SGE、并发或性能。
Host 能力查询为 `max_sge=30`、`max_sge_rd=30`，不能据此推断 NPU 上限。

首次新库运行因 `HCCL_CONNECT_TIMEOUT=15` 小于该版本120秒下限而失败；
改为120后通过。READ 的 CQ 等待仍为15秒，demo 进程有90秒总时限。
失败记录保留于 [首次运行日志](../evidence/full_hcomm_run_20260920_232416.json)。

运行步骤和建链源码入口见 [README](README.md)。
