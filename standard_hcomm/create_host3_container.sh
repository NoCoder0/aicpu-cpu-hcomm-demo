#!/bin/bash
set -euo pipefail
# 在 Host3 (10.1.101.27) 宿主机运行；后续编译和程序运行使用 docker exec。
name=rdma-hcomm-host-cann91
image=rdma-hcomm-host3-hhy-snapshot:20260921
work=/home/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91
if docker container inspect "$name" >/dev/null 2>&1 || \
   docker image inspect "$image" >/dev/null 2>&1; then
    echo 'Container or snapshot exists; inspect/reuse it instead of overwriting.' >&2
    exit 1
fi
mkdir -p "$work"
exec > >(tee "$work/container_setup.log") 2>&1
trap 'echo $? > "$work/container_setup.exit"' EXIT
# 复制原容器的可写层，不暂停原进程。bind mount 的内容不属于镜像快照。
docker commit --pause=false hhy "$image"
# CANN 在镜像内；工作区单独挂载。沿用原容器的 host 网络、privileged 和
# 16 GiB shm 配置以访问 RDMA 设备，但不共享原容器的 /home、/data、/opt。
docker run -d --name "$name" --network host --privileged --shm-size 16g \
    --security-opt label=disable \
    --mount type=bind,source=/dev/infiniband,target=/dev/infiniband \
    --mount type=bind,source="$work",target=/workspace \
    --workdir /workspace --entrypoint /bin/bash "$image" -c 'exec sleep infinity'
docker exec "$name" bash -c '
    source /usr/local/Ascend/cann-9.1.0/set_env.sh
    cat /usr/local/Ascend/cann-9.1.0/compiler/version.info
    g++ --version | head -n 1
    cmake --version | head -n 1
    ls -l /dev/infiniband
'
