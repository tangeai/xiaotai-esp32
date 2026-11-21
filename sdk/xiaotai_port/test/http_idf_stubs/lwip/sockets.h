// SPDX-License-Identifier: MIT
#pragma once
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#define LWIP_SOCKET_OFFSET 9
#define LWIP_SELECT_MAXNFDS 15
int lwip_socket(int, int, int);
int lwip_fcntl(int, int, int);
int lwip_connect(int, const struct sockaddr *, socklen_t);
int lwip_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int lwip_getsockopt(int, int, int, void *, socklen_t *);
int lwip_setsockopt(int, int, int, const void *, socklen_t);
int lwip_close(int);
ssize_t lwip_send(int, const void *, size_t, int);
ssize_t lwip_recv(int, void *, size_t, int);
