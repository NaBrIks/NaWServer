// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

// Host-only SDK boundary fixture. No native network operations are performed.
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>

#define LWIP_SO_SNDTIMEO 1
#ifndef LWIP_SO_SNDRCVTIMEO_NONSTANDARD
#define LWIP_SO_SNDRCVTIMEO_NONSTANDARD 0
#endif
#ifndef LWIP_SOCKET_OFFSET
#define LWIP_SOCKET_OFFSET 32
#endif
#define CONFIG_LWIP_MAX_SOCKETS 4
#define SO_REUSE 1

extern "C"
{

int lwip_socket(int domain, int type, int protocol);
int lwip_setsockopt(int descriptor, int level, int option, const void *value, socklen_t size);
int lwip_getsockopt(int descriptor, int level, int option, void *value, socklen_t *size);
int lwip_ioctl(int descriptor, long command, void *value);
int lwip_bind(int descriptor, const sockaddr *address, socklen_t size);
int lwip_listen(int descriptor, int backlog);
int lwip_getsockname(int descriptor, sockaddr *address, socklen_t *size);
int lwip_getpeername(int descriptor, sockaddr *address, socklen_t *size);
int lwip_accept(int descriptor, sockaddr *address, socklen_t *size);
ssize_t lwip_recv(int descriptor, void *destination, size_t capacity, int flags);
ssize_t lwip_send(int descriptor, const void *bytes, size_t size, int flags);
int lwip_poll(pollfd *descriptors, nfds_t count, int timeout);
int lwip_shutdown(int descriptor, int how);
int lwip_close(int descriptor);

}
