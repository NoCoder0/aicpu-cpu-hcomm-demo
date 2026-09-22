# AICPU ↔ CPU 标准 hcomm：1600 × 656 B 离散 READ

每轮从鲲鹏 Host DRAM 的 262144 段源池选取1600个不同数据块，每块656字节，总有效数据量 **1,049,600字节**，写入 A3 HBM 的离散目标槽。目标步长1312字节，前后各4096字节保护区。逻辑数据块数、Hcomm调用数、TS任务数和网络线上报文数是不同概念。

最初16字节跑通基线保留在提交 `110809f97b83c896cdb6d947acd458ca444f1988`，历史记录见 [VALIDATION.md](VALIDATION.md)。当前测量记录见 [BENCHMARK_VALIDATION.md](BENCHMARK_VALIDATION.md)。

## 审阅入口

- `npu_reader.cpp`：同一文件编译 A3 主机控制程序和 AICPU `StandardReadKernel`，直接调用标准接口。
- `host_server.cpp`：Host源池注册、建链、每轮准备数据并保持 MR 有效。
- `common.h`：Endpoint、Mem、Channel公开接口和管理TCP握手。
- `benchmark.h`：两端统一的数据布局、离散地址排列、逐轮变化的数据模式。
- `profile.sh` / `analyze_profile.py`：采集、退出码检查、算子与通信任务统计。
- `reference/hcomm`：固定 `33d156bf832744a4f767144149ed1b2e304ff2e3`；本轮基础实现没有修改子模块。

## 建链与完成条件

1. Host创建 HOST/ROCE Endpoint；A3初始化ACL、选择设备2，创建 DEVICE/ROCE Endpoint。
2. 分配DRAM/HBM，调用 `HcommMemReg` / `HcommMemExport`；管理TCP交换opaque描述符。A3用 `HcommMemImport` 导入Host源池。
3. `HcommChannelCreate` / `HcommChannelGetStatus` 推进两端至READY。应用不解析rkey、QPN等私有信息。
4. A3通过 `HcommThreadAlloc(COMM_ENGINE_AICPU_TS, ...)` 创建通信线程。
5. 每轮Host先填充本轮所选数据，再经管理TCP确认源数据可读；该准备过程在测量区间外。
6. AICPU执行 `BatchStart → 1600 × HcommReadOnThread → HcommChannelDrainOnThread → HcommAclrtNotifyRecordOnThread → BatchEnd`。公开批量模式不承诺只在End时提交，库内部可以提前下发。
7. A3同步kernel流，通过 `aclrtWaitAndResetNotify` 等待Drain后记录的通知，再同步完成流。**仅同步kernel流或调用 `aclrtSynchronizeDevice` 不能替代这条显式完成依赖。**
8. 回拷HBM，逐字节校验所有payload、段间间隙和前后保护区。每轮地址和内容随generation变化，避免旧数据误通过。通过后才准备下一轮。
9. 最后一轮校验成功后通知Host，按线程、通道、MR、内存的顺序清理。未知完成状态下不提前注销MR。

源地址用固定可复现的排列生成，与UB参考测试采用相同源池规模；这不声称复现了参考脚本的随机分布。源数据生成、清零、参数上传、校验、建链均在计时区间外。逐轮校验与数据准备会影响缓存状态，结果不是无校验的饱和带宽测试。

## 环境及复现

| 角色 | 管理IP | RDMA地址 / 设备 | 独立容器 |
|---|---|---|---|
| A3 D | 10.1.101.201 | 20.168.0.3 / 物理设备2 | rdma-hcomm-cann91 |
| Host3 | 10.1.101.27 | 20.168.0.19 / mlx5_2 / enp165s0f0np0 | rdma-hcomm-host-cann91 |

只使用上述独立容器，工作目录 `/workspace/aicpu-cpu-hcomm-demo`，CANN `/usr/local/Ascend/cann-9.1.0`。Host侧依赖已有CANN/HAL环境，不能推定任意裸CPU环境可用。控制端口19516、HCOMM监听端口19517。正式运行前在宿主机复查：

```bash
# A3
/usr/local/Ascend/driver/tools/hccn_tool -i 2 -ip -g
/usr/local/Ascend/driver/tools/hccn_tool -i 2 -link -g
/usr/local/Ascend/driver/tools/hccn_tool -i 2 -ping -g address 20.168.0.19 pkt 64
# Host3
ping -c 3 -W 2 -I 20.168.0.19 20.168.0.3
```

两端容器使用相同源码和子模块提交。初次构建：Host执行 `bash build.sh host`，A3执行 `bash build.sh a3`。hcomm未改动且已构建时，可直接重新部署应用：

```bash
# Host3容器
bash deploy.sh host
# A3容器
bash deploy.sh a3
```

部署会替换独立容器内hcomm库和AICPU kernel包。每步需退出0后继续。库构建使用Debug模式以保留Host插件所需符号；应用和用户kernel用 `-O2 -Wall -Wextra -Werror`。性能运行默认关闭stdout内部日志，日志级别ERROR；可通过环境变量覆盖。

```bash
# 先启动Host3；等待Control listening
bash run.sh host > build/host.log 2>&1
# A3：100轮测量，10轮预热，每轮都校验
bash run.sh a3 100 10 > build/a3.log 2>&1
# profiling需重新启动一次Host，输出目录必须不存在
bash profile.sh build/profile-new 100 10 > build/profile-a3.log 2>&1
# 汇总（profile目录指向msprof导出的mindstudio_profiler_output）
python3 analyze_profile.py build/profile-a3.log --profile build/profile-new/PROF_xxx/mindstudio_profiler_output --output build/profile-summary.json
python3 analyze_profile.py build/a3.log --output build/plain-summary.json
```

必须检查**两端退出0、所有轮校验通过、采集样本数匹配**。本机msprof可能在应用失败后仍返回0，因此 `profile.sh` 额外记录和检查 `application.exit`。

## 计时口径

| 指标 | 范围 |
|---|---|
| `submit_us` | AICPU内部从BatchStart前到BatchEnd返回后的API生成/提交时间，不能当作纯通信完成时间 |
| `launch_stream_us` | 主机调用Launch到kernel流同步返回，包含下发、调度和kernel执行 |
| `completion_wait_us` | 随后在ACL流等待并消费完成通知的主机经过时间 |
| `host_complete_us` | 上述两个主机区间之和；不含建链、准备与校验 |
| msprof `kernel_task` | `task_time.csv` 中 StandardReadKernel 的任务时间 |
| msprof `aicpu_task` / `aicpu_total` | `aicpu.csv` 同名kernel的Task_time / Total_time，保留工具原口径 |
| `communication_task_span` | 首个通信WRITE_VALUE_SQE开始到Drain后NOTIFY_RECORD_SQE结束；包括后续任务尚未生成的间隔和通知开销，不是线速传输时间 |
| `kernel_start_to_completion_record` | 同一设备时间线中，kernel任务开始到通信完成Record结束 |

算子执行与通信任务可以重叠，不能将两者相加。只在同一时钟域内相减；跨主机阶段均各自测量。统计使用10轮预热后100轮样本，分位数采用nearest-rank，不以阶段均值相加替代实测E2E。

兼容测试仍可在Host容器运行 `python3 tests/run.py` 和 `python3 tests/run.py --plugin`；它们不替代HBM实测。所有原始日志、profiling和部署清单留在 `build/`，不提交大型产物或凭据。
