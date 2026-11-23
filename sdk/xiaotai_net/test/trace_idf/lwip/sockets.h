// SPDX-License-Identifier: MIT
#pragma once
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define LWIP_SOCKET_OFFSET 28
#define MEMP_NUM_NETCONN 4
#define LWIP_SELECT_MAXNFDS (LWIP_SOCKET_OFFSET + MEMP_NUM_NETCONN)

int trace_test_bind(int fd, const struct sockaddr *address, socklen_t length);
int trace_test_select(int nfds, fd_set *readable, fd_set *writable, fd_set *except, struct timeval *timeout);
int __wrap_lwip_socket(int domain, int type, int protocol);
int __wrap_lwip_close(int fd);
ssize_t __wrap_lwip_recvfrom(int fd, void *data, size_t size, int flags,
                            struct sockaddr *from, socklen_t *from_len);

#define socket __wrap_lwip_socket
#define closesocket __wrap_lwip_close
#define recvfrom __wrap_lwip_recvfrom
#define bind trace_test_bind
#define lwip_select trace_test_select
