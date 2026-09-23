# AICPU ↔ CPU 标准 hcomm：1600 × 656 B 离散 READ 与 Host 聚合

每轮从鲲鹏 Host DRAM 的 262144 段源池选取1600个不同数据块，每块656字节，总有效数据量 **1,049,600字节**，写入 A3 HBM 的离散目标槽。目标步长1312字节，前后各4096字节保护区。逻辑数据块数、Hcomm调用数、TS任务数和网络线上报文数是不同概念。

最初16字节跑通基线保留在提交 `110809f97b83c896cdb6d947acd458ca444f1988`，历史记录见 [VALIDATION.md](VALIDATION.md)。1600块基础READ及首次profiling已提交为 `c755392`，记录见 [BENCHMARK_VALIDATION.md](BENCHMARK_VALIDATION.md)。随后实现的聚合方案、修复和对照结果见 [AGGREGATE_VALIDATION.md](AGGREGATE_VALIDATION.md)。

## 审阅入口

- `npu_reader.cpp`：同一文件编译 A3 主机控制程序和 AICPU `StandardReadKernel`，直接调用标准接口。
- `host_server.cpp`：Host源池注册、建链、每轮准备数据并保持 MR 有效。
- `common.h`：Endpoint、Mem、Channel公开接口和管理TCP握手。
- `benchmark.h`：两端统一的数据布局、离散地址排列、逐轮变化的数据模式。
- `profile.sh` / `analyze_profile.py`：采集、退出码检查、算子与通信任务统计。
- `aggregate_kernel.cpp` / `aggregate_npu.cpp`：AICPU发送地址表、等待Host写回、6 lane scatter及逐轮校验。
- `aggregate_host.cpp` / `gather_pool.h`：16个常驻Host worker按收到的地址gather，标准Hcomm连续写回。
- `aggregate_protocol.h`：两端暂存区、请求表、doorbell/ready及共享参数布局。
- `reference/hcomm`：固定到 `d336953a180e83b556d60e5d5c1ca18d6922f5ba`（`NoCoder0/hcomm` 的 `feat/aicpu-cpu-rdma` 分支），已包含聚合写回所需的修复。demo不再维护hcomm补丁，使用 `git submodule update --init --recursive` 获取固定版本。

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

两端容器必须使用相同源码及hcomm修复。初次构建：Host执行 `bash build.sh host`，A3执行 `bash build.sh a3`。已有构建时，聚合新增的Host插件修复需要先在两端执行：

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
cmake --build reference/hcomm/build --target hcomm_cpu_roce_plugin -j16
```

构建成功后部署库和应用；仅应用变化时可直接部署：

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

## 聚合版运行

聚合采用参考任务《审查 memfabric sparse_copy-UB聚合》的四阶段流程：AICPU经Hcomm发送地址表和doorbell；Host验证实际收到的地址表，16 worker gather；Host连续写回1,049,600字节并等待Fence，再写8字节ready；6个AICPU lane失效scratch缓存后scatter并clean目标。管理TCP仅做建链、源数据准备确认及最终验收，不传payload。

以下CPU编号仅对应当前Host3（网卡NUMA 2）。基础READ对照也用服务CPU224；gather worker分别绑定192、194、…、222。

```bash
# Host3容器，先启动并等待Control listening
export HCOMM_BENCHMARK_MODE=aggregate
export HCOMM_HOST_CPU=224
export HCOMM_GATHER_CPUS=192,194,196,198,200,202,204,206,208,210,212,214,216,218,220,222
taskset -c 224 bash run.sh host > build/aggregate-host.log 2>&1
# A3容器，无profiling
export HCOMM_BENCHMARK_MODE=aggregate
bash run.sh a3 100 10 > build/aggregate-a3.log 2>&1
# 或：重新启动Host后采集profiling
bash profile.sh build/aggregate-profile-new 100 10 > build/aggregate-profile-a3.log 2>&1
python3 analyze_profile.py build/aggregate-profile-a3.log --kernel AggregateKernel \
  --host-log build/aggregate-host.log \
  --profile build/aggregate-profile-new/PROF_xxx/mindstudio_profiler_output \
  --output build/aggregate-summary.json
```

无profiling汇总时省略`--profile`。切回基础READ时，两端均设置`HCOMM_BENCHMARK_MODE=read`；Host对照命令为`taskset -c 224 bash run.sh host`。

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
