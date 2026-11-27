// SPDX-License-Identifier: MIT
#pragma once
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

int portal_test_getsockname(int fd, struct sockaddr *address, socklen_t *size);
int portal_test_socket(int domain, int type, int protocol);
int portal_test_setsockopt(int fd, int level, int name, const void *arg, socklen_t size);
int portal_test_bind(int fd, const struct sockaddr *address, socklen_t len);
ssize_t portal_test_recvmsg(int fd, struct msghdr *msg, int flags);
ssize_t portal_test_sendto(int fd, const void *data, size_t size, int flags,
                           const struct sockaddr *address, socklen_t len);
int portal_test_close(int fd);

#define getsockname portal_test_getsockname
#define socket portal_test_socket
#define setsockopt portal_test_setsockopt
#define bind portal_test_bind
#define recvmsg portal_test_recvmsg
#define sendto portal_test_sendto
#define close portal_test_close
