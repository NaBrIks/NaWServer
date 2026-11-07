// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/tcp_transport.h>
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/sse.h>
#include <NaWServer/websocket.h>
#include <type_traits>

static_assert(__cplusplus == 201703L, "Common TCP API must compile as C++17");
static_assert(std::is_trivially_copyable_v<NaWServer::tcp::Handle>);
static_assert(std::is_trivially_copyable_v<NaWServer::tcp::IoResult>);
static_assert(std::is_trivially_copyable_v<NaWServer::tcp::Interest>);
static_assert(std::is_trivially_copyable_v<NaWServer::tcp::Ready>);
