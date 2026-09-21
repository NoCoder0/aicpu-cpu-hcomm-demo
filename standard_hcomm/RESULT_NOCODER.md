# NoCoder 分支：标准 hcomm READ 实测记录

2026-09-21。**最小单段 Host DRAM → NPU HBM READ 已通过。**
上游基线 `64ef7f9e05964add831d976876ce54adb74cd286`，来源 `NoCoder0/hcomm` 的
`feat/aicpu-urma-design` 分支；本地 hcomm 修复提交 `33d156bf832744a4f767144149ed1b2e304ff2e3`。
应用分支 `feature/nocoder-hcomm-host3`，旧 `feature/standard-hcomm-host-to-hbm` 分支保留。

## 已验证结果

| 检查 | 结果 |
|---|---|
| CPU 20.168.0.19 → NPU2 20.168.0.3 ping | 3/3 收到，0%丢包 |
| NPU2 → CPU，hccn_tool 指定 pkt 64 | 3/3 收到，0%丢包；复查仍通过 |
| NPU 健康 / 端口 | Health OK，Link UP |
| hcomm Debug 构建 | A3 `--pkg --full --experimental`、Host `--pkg --experimental` 均退出0 |
| 标准 Endpoint / AICPU_TS Thread | 实机创建、同步及释放通过 |
| HcommMemReg / Export / Import | Host DRAM 与 NPU HBM 注册成功；NPU 导入真实 Host MR |
| HcommChannelCreate | 两端返回0，状态 READY；底层真实 QP 达到 RTS |
| 标准 BatchStart / Read / Drain / BatchEnd | 返回值均0 |
| ACL stream / device 同步 | 均0 |
| HBM 数据 | `68 65 6c 6c 6f 20 72 64 6d 61 20 64 65 6d 6f 00` |
| HBM 哨兵 | 剩余4080字节全部保持0xa5 |
| 清理和进程退出 | 线程、通道、MR、HBM正常释放；两端退出0 |
| 回归测试 | 描述符/标签3项、资源报文2项均通过，链接实际构建的库 |

成功实测的完整日志：
[A3](../evidence/nocoder_20260921/a3_run_a3_batch2.log)、
[Host3](../evidence/nocoder_20260921/host3_run_host3_batch2.log)。
对应退出码文件与库 SHA256 清单保存在同一证据目录。日志时间可能分别使用 UTC 和 UTC+8；
首次成功读取发生于北京时间约20:13；增加报文长度防护并完成回归后，最终版本于约20:30再次通过，
见 [最终 A3 日志](../evidence/nocoder_20260921/a3_final_run_a3_final2.log)、
[最终 Host3 日志](../evidence/nocoder_20260921/host3_final_run_host3_final2.log)。
[描述符/标签测试](../evidence/nocoder_20260921/host3_test_final_import.log)、
[报文回归测试](../evidence/nocoder_20260921/host3_test_final_wire.log)全部通过。
早期失败日志一并保留，不能与成功运行混用。

## 运行环境

| 项目 | A3 | Host3 |
|---|---|---|
| 管理地址 | 10.1.101.201 | 10.1.101.27 |
| RDMA 地址 | 20.168.0.3 | 20.168.0.19 |
| 设备 | device 2，物理卡1 Chip0 | mlx5_2 port1，enp165s0f0np0 |
| 容器 | rdma-hcomm-cann91 | rdma-hcomm-host-cann91 |
| 宿主机工作区 | /data1/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91/nocoder | /home/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91/nocoder |
| 容器工作区 | /workspace/nocoder | /workspace/nocoder |
| CANN | /usr/local/Ascend/cann-9.1.0 | /usr/local/Ascend/cann-9.1.0 |

Host3 曾重启；继续任务时启动了已存在的独立容器。原 hhy 容器没有修改。
所有编译、库部署、demo执行均在独立容器内；宿主机只做 SSH、容器管理和只读网络诊断。

**HAL 环境约束：** `LD_DEBUG=libs` 实测 Host3 加载
`/usr/local/Ascend/cann-9.1.0/devlib/aarch64/libascend_hal.so`。
这是克隆环境已有的 CANN 开发库；未编写 HAL 替代实现，也未伪造接口返回值。
该分支底层 HCCP 仍存在 HAL 设备查询依赖，因此本次成功不能推广为“任意不含 CANN/HAL 的裸 CPU 环境都支持”。
Host 使用真实 mlx5 网卡、真实 MR/rkey 和 RC QP；最终 HBM 数据校验验证了实际传输。

## 为何上游分支需要兼容修复

1. Host `ExchangeRdmaBufferDto` 与 A3 的 RDMA MR 描述符格式不同。在 hcomm 内根据 HOST/ROCE 类型做转换，
   校验长度、地址范围，保留实际 rkey；应用继续使用 opaque 描述符。
2. Host 插件和 A3 的 socket tag 拼接方式不同。统一显式 channelName、server/client IP与端口，设备对设备规则保留。
3. Host RoCE Endpoint 缺少 PEER 白名单和 RA 动态符号表初始化。补齐后，白名单应答和混合通道 MR/QP 调用正常。
4. Host 混合通道原用245字节集合通信格式；A3资源通道用109字节 Drain 格式。插件在资源模式下匹配该格式，
   校验 QP数、额外 MR计数和截断数据，并初始化资源数组，防止清理未初始化条目。
5. 应用显式调用 `HcommBatchModeStart/End`，将 READ 和 Drain 任务一起提交。
   单独看 READ/Drain 返回0并不足够：未显式批量提交的试验中，HBM 校验失败。

公开头文件、公开函数签名、底层 READ 原语没有修改；没有调用之前的自定义 `HcclReadByAscendQP`。
建链内部代码有兼容修复，因此准确说法是“标准接口 + 此分支源码修复”，不是“未经修改的上游”。
Host 资源模式目前仅验证1 QP、0用户 notify；不支持的形状明确报错。

## 构建、部署与回归

当前两端 `/workspace/nocoder/source` 均保存源码。完整构建在各自容器执行：

```bash
# A3
bash /workspace/nocoder/build_nocoder.sh a3
bash /workspace/nocoder/deploy_nocoder.sh a3
# Host3
bash /workspace/nocoder/build_nocoder.sh host
bash /workspace/nocoder/deploy_nocoder.sh host
```

部署脚本只更新本任务容器内 CANN。A3 用户 kernel 与源码构建的设备库一起打包；
仅去除调试段，重新生成 bin_hash.cfg，记录部署文件 SHA256。Debug 原始产物保留。
该构建依赖容器已有编译器、CANN、依赖缓存及必要下载；源码归档解压到 `source` 后需将
`standard_hcomm` 下 demo源码、JSON和脚本复制到 `/workspace/nocoder`。

Host3 容器回归测试：

```bash
source /usr/local/Ascend/cann-9.1.0/set_env.sh
python3 /workspace/nocoder/test_mem_import.py
python3 /workspace/nocoder/test_mem_import.py --plugin
```

测试使用系统 gtest 源码以旧 C++ string ABI 编译，分别验证真实 hcomm 的内存导入/标签逻辑和
真实 Host 插件的报文解析；不伪造设备或传输成功。网络与硬件执行由上述双节点实测覆盖。

本地 hcomm 修复未推送远端。为避免只记录不可从远端获取的 submodule SHA，提供
[完整补丁](patches/0001-host3-a3-compat.patch)。在基线提交上执行 `git am` 即可恢复代码变更：

```bash
git clone --branch feat/aicpu-urma-design https://github.com/NoCoder0/hcomm.git source
cd source
git checkout 64ef7f9e05964add831d976876ce54adb74cd286
git am /path/to/standard_hcomm/patches/0001-host3-a3-compat.patch
```

## SGL 范围

本次只证明**一次16字节连续 READ（最小单 SGE）**。没有验证单 WR 多 SGE、最大 SGL 上限、
多个不连续 Host 源地址、性能、并发或长时间稳定性。不能将 `HcommBatchTransferOnThread` 的多条传输
直接等同于单 WR 多 SGE。现有代码可作为后续扩展的已验证起点。
