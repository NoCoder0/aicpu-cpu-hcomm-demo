# Host聚合版本与基础READ对照

2026-09-22，在README所列Host3、A3 D设备2及独立CANN 9.1容器完成实测。
按要求先完成1600块基础READ和profiling并提交 `c755392`，之后才实现聚合。
聚合代码在该基础提交之后完成；2026-09-23将hcomm修复提交并推送为
`d336953a180e83b556d60e5d5c1ca18d6922f5ba`，当时demo通过子模块固定该版本，聚合代码和测量记录另行提交到demo仓库。
后续已移除`reference`子模块，构建改为直接拉取该远端分支到`build/hcomm`，见README。
本记录及SHA256清单保留实测时的路径与版本，不代表后续分支版本已经过同一轮测试。

## 参考与实现

参考任务《审查 memfabric sparse_copy-UB聚合》，任务ID
`01a0b3fc-5f85-76c0-bd0d-e057d869bde8`，其中memfabric代码提交
`b526de1d28e9b890e971d8c5419a05dfb2eb41f3`：

- `src/acc_offload/csrc/operators/aicpu/hybm_aggregate_urma_demo.cc`：四阶段聚合及6 lane scatter。
- `src/acc_offload/csrc/python_wrapper/pymf_acc_offload.cpp`：16 worker常驻gather线程池。
- `examples/kv_offload/sparse_copy_urma/03_aicpu_host_aggregate_urma.py`：源池规模及离散目标布局。

沿用其地址表→Host gather→连续写回+ready→AICPU scatter的结构，传输改用标准Hcomm RoCE接口。
源池262144块，每轮选1600个不同地址，每块656字节；HBM目标步长1312字节。
地址采用可复现排列，随generation变化；这并非参考Python随机分布的逐地址复现。

1. AICPU leader发送12808字节请求表，经Drain后写独立缓存行doorbell，再Drain并记录ACL完成通知。
2. Host读取收到的generation及全部地址，验证它们属于本轮请求。16个常驻worker使用收到的地址gather，总payload为1,049,600字节。
3. Host用一次`HcommWriteNbi`写入HBM连续scratch，`HcommChannelFence`确认完成，再单独写8字节ready并Fence。ready把generation和status合成一个对齐64位值。
4. 6个AICPU lane等待本轮ready，各自失效要读取的scratch缓存，再复制到离散目标并clean目标缓存，最后执行`dsb sy`。所有lane完成后记录kernel内部E2E。
5. A3控制程序同步kernel流、消费请求通信线程的完成通知，再逐字节校验1600块payload、所有间隙及首尾保护区。每轮地址和payload变化。

TCP仅用于描述符交换、源数据准备确认、最终验收；准备、目标填充值、地址表上传和校验均在计时区间外。
每轮串行等待完成后复用资源，未实现多请求流水。请求及ready等待有30秒超时，进程有300秒最终超时；错误不按成功样本记录。
失败且完成状态未知时退出进程，不提前注销可能仍被DMA引用的MR。

## 为写回补齐的hcomm兼容路径

子模块修复提交 `d336953a180e83b556d60e5d5c1ca18d6922f5ba` 基于
`33d156bf832744a4f767144149ed1b2e304ff2e3`，只修改experimental层的四个文件：

- `experimental/base_comm/comm_mem/roce_mem.{h,cc}`：区分A3 DEVICE/ROCE的33字节MR载荷与Host DTO，正确恢复device VA、size、type、rkey；验证长度、类型和地址范围。增加按peer及范围查询已导入MR的方法。
- `experimental/base_comm/channel/host_cpu_roce_channel.{h,cc}`：混合通道且`exchangeAllMems`时，从endpoint导入表查询远端MR/rkey，供Host READ/WRITE使用；其它路径保留通道MR表查找。

初次聚合调试暴露两个问题：A3描述符被按Host DTO解析导致地址错位；混合资源交换只携带Drain资源，Host发送路径使用空的远端应用MR列表。
修复后公开API未增加，应用不解析rkey/QPN，不自行构建verbs请求。
基础READ原先不需要Host主动写HBM，故未覆盖这些路径。

## 正式验证

两端插件重新构建、应用/kernel部署均退出0，应用使用`-O2 -Wall -Wextra -Werror`。
内部日志级别ERROR。Host网卡mlx5_2在NUMA 2；两方案服务线程均绑定CPU224，源池由该线程分配并首次写入。
聚合另有16个worker，绑定192、194、…、222（NUMA 2）；这是方案增加的CPU资源成本。
A3两方案都选择设备2、相同源/目标布局、相同逐轮准备与校验。

四组正式运行各10轮预热+100轮测量：聚合有/无profiling、基础READ有/无profiling。
**共440轮完整HBM校验通过，每组两端均退出0。**
聚合profiling对应110个kernel、660条AICPU lane记录；每6条记录的时间戳均处于对应kernel区间内。
基础profiling对应110个kernel、110条AICPU记录、110组完整READ/Drain/Record任务。

Host容器兼容测试：`python3 tests/run.py`通过3项，`python3 tests/run.py --plugin`通过4项，共7项。
新增测试覆盖A3线格式解码（logical VA与device VA不同）、错误类型/截断、导入rkey查询、错误peer、越界/溢出/零长度及unimport后的查询。
这些测试不模拟QP/驱动成功响应，硬件数据正确性由上述双节点运行验证。

## 时延结果

单位为微秒，分位数使用nearest-rank；每行100个正式样本，不剔除测量区间内的异常值。

| 同一口径 | 基础READ平均 | 聚合平均 | 基础P95 | 聚合P95 | 基础P99 | 聚合P99 |
|---|---:|---:|---:|---:|---:|---:|
| 无profiling：主机Launch至完成 | 1867.755 | 354.930 | 2033.494 | 386.596 | 2062.115 | 401.057 |
| 有profiling：主机Launch至完成 | 1939.942 | 361.107 | 2115.727 | 416.237 | 2145.638 | 430.578 |
| msprof：kernel任务时间 | 1873.699 | 267.099 | 2057.898 | 278.814 | 2068.638 | 295.934 |

无profiling的主机Launch至完成均值减少约81.0%，约5.26倍加速。
这里对比的是当前同配置两次运行的完成时延；基础kernel任务返回本身不保证通信完成，因此速度比较使用主机显式等待完成的指标。
最初基础commit的未绑核结果仍单独保存在BENCHMARK_VALIDATION，不替换历史数字。

聚合阶段细分如下；AICPU时钟和Host时钟独立，只在各自时钟内计算时差：

| 指标 | 无profiling平均 | 有profiling平均 | 范围 |
|---|---:|---:|---|
| AICPU `request_us` | 18.396 | 20.647 | BatchStart前至BatchEnd返回，包含两次Write、两次Drain和通知提交；不等同于doorbell抵达Host的时刻 |
| AICPU `wait_host_us` | 72.673 | 73.268 | 提交返回至观察本轮ready，包含剩余请求传输及Host服务等待 |
| AICPU `scatter_us` | 142.851 | 145.594 | leader观察ready后至6个lane全部完成，包含唤醒、失效、复制和clean |
| AICPU `kernel_e2e_us` | 233.921 | 239.509 | leader开始请求至全部scatter完成，逐轮实测 |
| Host `gather_us` | 15.384 | 16.364 | 发布线程池任务至16个worker完成 |
| Host `write_us` | 47.451 | 47.444 | 连续payload WriteNbi及Fence |
| Host `ready_us` | 3.785 | 3.797 | 准备并发送ready及Fence |
| Host `service_us` | 68.217 | 69.236 | 观察doorbell后至ready Fence返回，含地址表校验 |

Host阶段属于AICPU等待区间内的活动，不能再加到kernel E2E上。
msprof每轮6 lane的Task_time取最大值后再统计，有profiling均值238.382微秒；不把6个并行时间相加。
无profiling主机完成时延最大值602.595微秒，已纳入统计。

前10轮可见较慢的请求提交，部分约580–650微秒；按预先指定的10轮预热排除。
仅4轮的冒烟结果因此不代表稳定后的时延，原始日志保留全部预热记录。
有/无profiling聚合主机均值差6.177微秒（约1.74%），基础差72.187微秒（约3.86%）；独立运行间含调度波动，不能解释为隔离测得的工具开销。

## 证据与复现

运行方式见README。正式组名称为`aggregate-final-profile`、`aggregate-final-plain`、
`base-compare-profile`、`base-compare-plain`。
每组容器日志为`build/<名称>-a3.log`、`build/<名称>-host.log`，对应`.exit`均为0；
本地下载在`build/<名称>/a3.log`、`host.log`，CSV在`profile/`。

两次A3采集目录：

- `build/aggregate-final-profile-profile/PROF_000001_20260922153316387_00143846ODDAEEKJ/`。
- `build/base-compare-profile-profile/PROF_000001_20260922153527835_00144605BBMQIRKA/`。

小型结果与各原始日志/CSV的SHA256见`results/aggregate-{profile,plain}.json`、
`results/base-compare-{profile,plain}.json`。
`results/aggregate-environment.json`记录两端实际二进制、测试日志和所有运行源码的SHA256，源码按LF归一化后逐文件核对一致。
采集时，两端容器工作树基于原提交`110809f`加本次文件改动；本地基础commit为`c755392`。
环境清单保留采集时的HEAD和未提交状态，作为历史证据。提交前已再次核对全部运行源码与清单一致；
这些hcomm源码现已收录于`d336953a1`。提交整理另删除了`aggregate_npu.cpp`末尾空行，未改变运行逻辑；
清单的`commit_preparation`记录该格式调整后的SHA256。不能仅用采集时的远端`git HEAD`代表实际测试源码，应结合源码清单。

这是固定1600块、1 QP、单请求的首版聚合。有效数据块数与RDMA WR数、网络报文数不相等；没有测线上分包。
未覆盖长时间压力、多请求并发、其它NUMA拓扑、设备或网络，也未声称达到理论带宽上限。
