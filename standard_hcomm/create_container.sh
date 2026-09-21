#!/bin/bash
set -euo pipefail
work=/data1/z00502111/rdma_sgl_host_to_hbm/standard_hcomm_cann91
# 此脚本在 201 宿主机运行。已有同名容器/镜像时停止，避免覆盖此前的实验快照。
if docker container inspect rdma-hcomm-cann91 >/dev/null 2>&1 || \
   docker image inspect rdma-hcomm-hhy-snapshot:20260921 >/dev/null 2>&1; then
  echo 'Container or snapshot already exists; inspect/reuse it instead of recreating.' >&2
  exit 1
fi
mkdir -p "$work"
exec > "$work/container_setup.log" 2>&1
trap 'echo $? > "$work/container_setup.exit"' EXIT
# Snapshot the running container without pausing its processes. Bind mounts are not copied.
docker commit --pause=false hhy rdma-hcomm-hhy-snapshot:20260921
docker run -d --name rdma-hcomm-cann91 --network host --privileged --shm-size 16g \
  --security-opt label=disable \
  --mount type=bind,source=/usr/local/Ascend/driver,target=/usr/local/Ascend/driver,readonly \
  --mount type=bind,source=/usr/local/Ascend/firmware,target=/usr/local/Ascend/firmware,readonly \
  --mount type=bind,source=/usr/local/dcmi,target=/usr/local/dcmi,readonly \
  --mount type=bind,source=/etc/hccn.conf,target=/etc/hccn.conf,readonly \
  --mount type=bind,source=/etc/ascend_install.info,target=/etc/ascend_install.info,readonly \
  --mount type=bind,source=/dev/infiniband,target=/dev/infiniband \
  --mount type=bind,source="$work",target=/workspace \
  --workdir /workspace --entrypoint /bin/bash rdma-hcomm-hhy-snapshot:20260921 -c 'exec sleep infinity'
docker exec rdma-hcomm-cann91 bash -c 'cat /usr/local/Ascend/cann-9.1.0/opp/version.info; ls -l /dev/davinci0'
