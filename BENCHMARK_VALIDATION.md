# 基础版本实测：1600 × 656 字节

2026-09-22，在README所列Host3 / A3 D设备2及独立CANN 9.1容器验证。源码基于
`110809f97b83c896cdb6d947acd458ca444f1988`；hcomm固定
`33d156bf832744a4f767144149ed1b2e304ff2e3`，无子模块改动。

两端IP和链路复查通过，双向ping各3/3。复用基线已构建的hcomm，重新执行两端
`deploy.sh`编译本轮应用/kernel并部署，编译及部署退出0。应用使用-O2，内部日志级别ERROR。

## 最终结果

两组均为10轮预热+100轮测量，每轮1600个不同源块、656字节/块、1,049,600字节有效数据。
**220轮完整HBM payload、全部段间间隙及首尾保护区校验通过，两端退出0。**
profiling的110个kernel、110个完成通知和110组通信任务均与应用样本数量匹配。

单位均为微秒，P50/P95/P99采用nearest-rank。

| 指标 | 平均 | P50 | P95 | P99 |
|---|---:|---:|---:|---:|
| 无profiling：主机Launch至完成 | 1912.707 | 1899.738 | 2072.375 | 2104.427 |
| 有profiling：主机Launch至完成 | 1972.166 | 1927.789 | 2153.498 | 2324.935 |
| msprof：StandardReadKernel任务时间 | 1867.978 | 1803.864 | 2046.179 | 2062.538 |
| msprof：AICPU Task_time | 1826.388 | 1772.085 | 2009.500 | 2025.479 |
| msprof：通信任务首个doorbell至完成Record | 1710.253 | 1670.647 | 1876.542 | 2076.018 |
| msprof：kernel开始至通信完成Record | 1894.607 | 1833.024 | 2069.039 | 2267.215 |

本次有/无profiling主机均值相差约59.459微秒（3.11%）。这是两次独立运行观察到的差值，
包含调度波动，不能据此声称隔离测出了工具本身的精确开销。
kernel执行与通信任务重叠，不能相加。通信任务跨度包含任务生成/下发间隔，不能当作纯网络时间。

## 完成等待修正

初版仅同步kernel流再调用`aclrtSynchronizeDevice()`，在有/无profiling两组均于第41轮
offset=1914368出现期望payload仍为0xa5。所有API返回0仍不足以证明读取完成。
加入标准 `HcommAclrtNotifyRecordOnThread`，在Drain后记录通知，再在ACL流中
`aclrtWaitAndResetNotify`并同步，随后多组110轮通过。未采用sleep或放宽校验规避问题。
失败组只作为诊断证据，不纳入上表。

本机msprof在失败应用结束后可返回0；`profile.sh`通过独立`application.exit`严格检查实际应用退出码。

## 证据与复现

最终运行命令：Host两次分别运行`bash run.sh host`；A3依次运行
`bash profile.sh build/base-final-profile-profile 100 10`和`bash run.sh a3 100 10`。
配置、公开接口流程和统计工具使用方式见README。

容器根目录均为`/workspace/aicpu-cpu-hcomm-demo`：

- `build/base-final-profile-a3.log`、`build/base-final-profile-host.log`及对应`.exit`。
- `build/base-final-plain-a3.log`、`build/base-final-plain-host.log`及对应`.exit`。
- `build/base-final-profile-profile/PROF_000001_20260922150321433_00139817BFFNJJPE/`。
- `mindstudio_profiler_output/task_time_20260922150337.csv`和`aicpu_20260922150338.csv`。
- 本地原始证据在`build/base-final-profile/`和`build/base-final-plain/`。

小型统计与SHA256清单提交在`results/base-profile.json`和`results/base-plain.json`；大型CSV和trace不提交。

| 证据 | SHA256 |
|---|---|
| profiling应用日志 | `7d632d303d1cbe571fff7e34493090c0badf59a22b053c452cb736fd6bf4ea19` |
| 无profiling应用日志 | `031313f89a97880386f21300f8afc4be63fc7ae9974717403ba060be2e984e44` |
| task_time CSV | `f6bdf6723f4ee1f4f850ca82e7b7c87afc99e9f6e8e664be1c600eb2f26a73f4` |
| aicpu CSV | `b27053acd5d1cfec37fbda0709e16e84222f119ce587b65a33a2cefc59f25e11` |

## 范围

这是1 QP的标准Hcomm逐块READ基础版本，不是SGL多段聚合。1600个逻辑块不等于1600个线上报文。
没有测长期稳定性、并发多请求、其它卡/Host、不同NUMA绑定或最大网络带宽。Host与AICPU阶段不跨时钟相减。
基线5项描述符兼容测试的历史结果见VALIDATION；本次核心验收是重新编译后的双节点逐轮数据验证与真实profiling。
