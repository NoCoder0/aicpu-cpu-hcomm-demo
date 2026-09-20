#ifndef DEMO_CONTROL_H
#define DEMO_CONTROL_H
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEMO_BYTES 4096
#define DEMO_TEXT "hello rdma demo"
#define DEMO_PORT 19515

static void Die(const char *what)
{
    fprintf(stderr, "FAIL: %s (errno=%d: %s)\n", what, errno, strerror(errno));
    exit(1);
}

static void SocketTimeout(int fd)
{
    struct timeval timeout = {30, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) Die("socket timeout");
}

static void SendLine(int fd, const char *line)
{
    size_t offset = 0, length = strlen(line);
    while (offset < length) {
        ssize_t n = send(fd, line + offset, length - offset, MSG_NOSIGNAL);
        if (n <= 0) Die("send control");
        offset += (size_t)n;
    }
}

static void ReadLine(int fd, char *line, size_t size)
{
    for (size_t i = 0; i + 1 < size; ++i) {
        if (recv(fd, line + i, 1, 0) != 1) Die("read control");
        if (line[i] == '\n') { line[i] = 0; return; }
    }
    Die("oversized control message");
}

static void GidText(const void *gid, char *text)
{
    if (!inet_ntop(AF_INET6, gid, text, INET6_ADDRSTRLEN)) Die("inet_ntop");
}
#endif
