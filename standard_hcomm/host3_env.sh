#!/bin/bash
# 在 Host3 的 rdma-hcomm-host-cann91 容器内 source 本文件。
# 原镜像环境含历史 CANN 9.0 路径；显式加载实际安装的 9.1 环境。
source /usr/local/Ascend/cann-9.1.0/set_env.sh
cd /workspace
