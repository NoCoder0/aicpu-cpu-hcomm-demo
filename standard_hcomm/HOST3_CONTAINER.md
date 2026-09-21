# Host3 独立容器

2026-09-21，在 Host3（10.1.101.27）从运行中的 `hhy` 创建快照并启动独立容器。
后续 Host 端编译、依赖调整与程序运行统一在此容器内执行。

| 项目 | 实际值 |
|---|---|
| 原容器 | `hhy` / `e26adf6ee663` |
| 新容器 | `rdma-hcomm-host-cann91` / `013bb91d2ca0`，运行中 |
| 快照镜像 | `rdma-hcomm-host3-hhy-snapshot:20260921` |
| 镜像 ID | `4453f7b4dd521c3a80523e36a69a3716a1400f919325f1878d65e96561c835d9` |
| CANN | `/usr/local/Ascend/cann-9.1.0`，compiler/opp version.info 均为 9.1.0 |
| 容器工作目录 | `/workspace` |
| Host3 持久化目录 | `/home/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91` |
| 构建工具 | g++ 11.4.0，CMake 4.3.2 |

在 Host3 执行：

```bash
docker exec -it rdma-hcomm-host-cann91 bash
source /workspace/host3_env.sh
```

非交互操作示例：

```bash
docker exec rdma-hcomm-host-cann91 bash -c \
  'source /workspace/host3_env.sh; g++ --version'
```

创建脚本：[create_host3_container.sh](create_host3_container.sh)。快照使用
`docker commit --pause=false`，未停止或重启原 `hhy`。两个容器的可写层独立。
CANN 安装位于镜像内，随快照复制；没有重新安装 CANN。
仅挂载独立工作目录和 `/dev/infiniband`，未沿用原容器的整个 `/home`、`/data`、`/opt` 挂载。
保留 host 网络、privileged、16 GiB shm；RDMA 硬件与网络仍与宿主机共享。

已在新容器中检查：

- `mlx5_0` 到 `mlx5_3` 的端口 1 均为 ACTIVE，uverbs 设备可见。
- `include/hcomm/hcomm_res.h` 和 `hcomm_primitives.h` 存在。
- `lib64/libhcomm.so` 及 `hcomm_plugin/libhcomm_cpu_roce_plugin.so` 存在。
- 加载 CANN 9.1 环境后，`ldd libhcomm.so` 未报告缺失依赖。

此检查只确认容器和 SDK 环境。`ldd` 中 HAL 解析到 CANN 的
`devlib/aarch64/libascend_hal.so`，不是宿主 NPU 驱动；不能凭依赖解析成功断言
标准 HOST Endpoint、Channel 或 RDMA READ 已可用。本次未替换库，未执行端到端 READ。

已有 demo 的默认地址仍为 Host4；切换实际测试前需要同步调整两端配置。
既有拓扑记录中，Host3 的 `20.168.0.19` / `mlx5_2` 对应 A3 D 的
NPU 设备 2 / `20.168.0.3`，不能直接复用 Host4 的 `20.168.0.17`。

原始检查记录：[host3_cann91_container.json](../evidence/host3_cann91_container.json)。
