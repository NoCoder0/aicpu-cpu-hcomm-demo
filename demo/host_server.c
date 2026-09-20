#include "control.h"
#include <infiniband/verbs.h>
#include <time.h>

static void Modify(struct ibv_qp *qp, struct ibv_qp_attr *attr, int mask)
{
    int rc = ibv_modify_qp(qp, attr, mask);
    if (rc) { errno = rc; Die("ibv_modify_qp"); }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(100);
    int count = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
    struct ibv_context *context = NULL;
    for (int i = 0; devices && i < count; ++i) {
        if (!strcmp(ibv_get_device_name(devices[i]), "mlx5_3")) context = ibv_open_device(devices[i]);
    }
    if (!context) Die("open mlx5_3");
    struct ibv_device_attr caps;
    struct ibv_port_attr port;
    union ibv_gid gid;
    if (ibv_query_device(context, &caps) || ibv_query_port(context, 1, &port) ||
        ibv_query_gid(context, 1, 3, &gid)) Die("query RDMA device");
    printf("Host mlx5_3 port=1 gid_index=3 max_sge=%d max_sge_rd=%d active_mtu=%d\n",
           caps.max_sge, caps.max_sge_rd, port.active_mtu);
    struct ibv_pd *pd = ibv_alloc_pd(context);
    struct ibv_cq *cq = ibv_create_cq(context, 16, NULL, NULL, 0);
    if (!pd || !cq) Die("allocate PD/CQ");
    unsigned char *buffer = NULL;
    if (posix_memalign((void **)&buffer, 4096, DEMO_BYTES)) Die("allocate host DRAM");
    memset(buffer, 0, DEMO_BYTES);
    memcpy(buffer, DEMO_TEXT, sizeof(DEMO_TEXT));
    struct ibv_mr *mr = ibv_reg_mr(pd, buffer, DEMO_BYTES, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) Die("register host DRAM");
    struct ibv_qp_init_attr init = {0};
    init.send_cq = cq; init.recv_cq = cq; init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 8; init.cap.max_recv_wr = 8;
    init.cap.max_send_sge = 1; init.cap.max_recv_sge = 1;
    struct ibv_qp *qp = ibv_create_qp(pd, &init);
    if (!qp) Die("create host QP");
    struct ibv_qp_attr attr = {0};
    attr.qp_state = IBV_QPS_INIT; attr.port_num = 1; attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;
    Modify(qp, &attr, IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS);
    int listener = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (listener < 0) Die("socket");
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_port = htons(DEMO_PORT);
    inet_pton(AF_INET, "10.1.101.32", &address.sin_addr);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) Die("listen");
    printf("READY control=10.1.101.32:%d RDMA=20.168.0.17 payload='%s' bytes=%zu\n",
           DEMO_PORT, buffer, sizeof(DEMO_TEXT));
    int peer = accept(listener, NULL, NULL);
    if (peer < 0) Die("accept");
    SocketTimeout(peer);
    uint32_t psn = ((uint32_t)time(NULL) ^ (uint32_t)getpid()) & 0xffffff;
    char gidString[INET6_ADDRSTRLEN], line[512];
    GidText(&gid, gidString);
    snprintf(line, sizeof(line), "HOST1 %u %u %s %llx %u %zu %u\n", qp->qp_num, psn, gidString,
             (unsigned long long)(uintptr_t)buffer, mr->rkey, sizeof(DEMO_TEXT), (unsigned)port.active_mtu);
    SendLine(peer, line);
    ReadLine(peer, line, sizeof(line));
    unsigned remoteQpn, remotePsn, mtu;
    if (sscanf(line, "NPU1 %u %u %45s %u", &remoteQpn, &remotePsn, gidString, &mtu) != 4 ||
        mtu < IBV_MTU_256 || mtu > (unsigned)port.active_mtu) Die("NPU metadata/MTU");
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR; attr.path_mtu = (enum ibv_mtu)mtu;
    attr.dest_qp_num = remoteQpn; attr.rq_psn = remotePsn;
    attr.max_dest_rd_atomic = 1; attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1; attr.ah_attr.port_num = 1;
    attr.ah_attr.grh.sgid_index = 3; attr.ah_attr.grh.hop_limit = 1;
    if (inet_pton(AF_INET6, gidString, &attr.ah_attr.grh.dgid) != 1) Die("remote GID");
    Modify(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
           IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS; attr.sq_psn = psn; attr.timeout = 14;
    attr.retry_cnt = 7; attr.rnr_retry = 7; attr.max_rd_atomic = 1;
    Modify(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
           IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    printf("QP RTS local_qpn=%u remote_qpn=%u mtu=%u\n", qp->qp_num, remoteQpn, mtu);
    SendLine(peer, "READY\n");
    ReadLine(peer, line, sizeof(line));
    if (strcmp(line, "PASS")) Die("NPU did not confirm validation");
    printf("PASS: NPU confirmed RDMA READ and HBM payload/guard validation\n");
    close(peer); close(listener);
    if (ibv_destroy_qp(qp) || ibv_dereg_mr(mr) || ibv_destroy_cq(cq) || ibv_dealloc_pd(pd)) Die("cleanup");
    free(buffer); ibv_close_device(context); ibv_free_device_list(devices);
    return 0;
}
