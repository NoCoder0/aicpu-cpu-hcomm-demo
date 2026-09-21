# 整理后实机回归

2026-09-21，北京时间约21:11完成。**标准 hcomm Host DRAM → NPU HBM READ 通过。**

## 版本与环境

- 实测 demo 提交：`debe6b966914a406ad3036cc92cf7eee1156e0a7`，分支 `feat/aicpu-cpu-rdma-demo`。
- hcomm：`33d156bf832744a4f767144149ed1b2e304ff2e3`，分支 `feat/aicpu-cpu-rdma`。
- hcomm 基线：原 `feat/aicpu-urma-design` 的 `64ef7f9e05964add831d976876ce54adb74cd286`。
- 两端实测时 Git 工作区均干净；本记录之后的提交只补充文档，不改变实测代码。
- A3 D：管理 `10.1.101.201`，NPU2 RDMA `20.168.0.3`，容器 `rdma-hcomm-cann91`。
- Host3：管理 `10.1.101.27`，RDMA `20.168.0.19`（mlx5_2），容器 `rdma-hcomm-host-cann91`。
- 容器工作区均为 `/workspace/aicpu-cpu-hcomm-demo`；CANN `/usr/local/Ascend/cann-9.1.0`。
- Host3 编译工具：g++ 11.4.0、CMake 4.3.2、Python 3.12.13。

## 执行方式与结果

按 [README.md](README.md) 完成网络检查、克隆子模块、构建、部署、Host先启动/A3后启动及兼容测试。
在新的检出目录进行完整 hcomm 构建；只复制旧目录的 `third_party` 依赖缓存，没有复用旧 hcomm
`build/`、`build_out/` 或旧 demo 可执行文件。本次设 `JOBS=32`。

首次从 GitHub 递归克隆成功。后续更新发生 GitHub 连接超时，使用包含已推送提交的 Git bundle
快进更新两端，确认提交号和 SHA256 一致后重新编译应用。该传输方式不改变代码：
`demo-update.bundle` SHA256 为 `c2284d80e05e7441e8b848c8846914cf72cd2d45834661f0431795f84553d9b4`。

| 检查 | 本次结果 |
|---|---|
| NPU2 链路 | Link UP |
| NPU2 → Host3，hccn_tool pkt 64 | 3/3 收到，0%丢包 |
| Host3 → NPU2，指定 RDMA 源 IP ping | 3/3 收到，0%丢包 |
| Host `bash build.sh host` / `bash deploy.sh host` | 全量构建、部署均退出0 |
| A3 `bash build.sh a3` / `bash deploy.sh a3` | 全量主机/设备构建、部署均退出0 |
| 两端 Channel | `CHANNEL status=0`，READY |
| BatchStart / Read / Drain / BatchEnd | 均返回0 |
| ACL stream/device 同步 | 返回0 |
| HBM 数据 | 16字节 `hello rdma demo`（含末尾 NUL）正确 |
| HBM 哨兵 | 其余4080字节全部保持0xa5 |
| 清理与进程 | A3、Host均退出0 |
| `python3 tests/run.py` | 3项通过，退出0 |
| `python3 tests/run.py --plugin` | 2项通过，退出0 |
| 部署库/设备包完整性 | A3清单16项、Host清单12项，实际文件 SHA256 全部匹配 |

A3 关键输出：

```text
CHANNEL status=0
API args.batchStartResult => 0
API args.readResult => 0
API args.drainResult => 0
API args.batchEndResult => 0
HBM first 16 bytes: 68 65 6c 6c 6f 20 72 64 6d 61 20 64 65 6d 6f 00
PASS: standard HcommReadOnThread HBM='hello rdma demo', bytes=16, guards=4080 unchanged
```

Host 关键输出：

```text
CHANNEL status=0
PASS: NPU confirmed standard HCOMM READ and HBM validation
```

## 日志定位与校验

日志和部署清单留在各容器 `/workspace/aicpu-cpu-hcomm-demo/build/`，不将大型运行日志提交到最小代码仓。

| 角色 | 文件 | SHA256 |
|---|---|---|
| A3 | `a3.log` | `bd5da03f7997760705e744acf5c1164521f0faf6eda2d45ca75849bf3a0a3390` |
| Host3 | `host.log` | `4c026b7fd0860c38949e694983a8cba3cd6e5d285119ffaf7034b43709757b01` |

同目录的 `hcomm.log` / `hcomm.exit`、`deploy.log` / `deploy.exit` 记录构建和部署，
`a3.exit` / `host.exit` 记录程序退出码；Host3 的 `test-import.log` / `test-wire.log` 及同名 `.exit`
记录5项兼容测试。`deployed_libraries.json` 保存构建源库及部署库校验值，`verified.json` 保存实际部署校验结果。

从宿主机定位：

- A3：`/data1/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91/aicpu-cpu-hcomm-demo/build/`
- Host3：`/home/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91/aicpu-cpu-hcomm-demo/build/`

## 边界

验证的是1 QP、0用户 notify、一次16字节连续 READ。没有验证单 WR 多 SGE、最大 SGL 长度、
批量多地址读取、性能或长期稳定性。5项兼容测试不等同于 hcomm 完整 UT/ST 测试套件。
应用使用标准公开接口；hcomm 包含该分支的兼容修复，不能把结果归因于未经修改的原分支。
