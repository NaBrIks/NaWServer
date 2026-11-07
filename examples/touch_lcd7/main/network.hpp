// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include "sdkconfig.h"
#include "wire.hpp"
#include <NaWServer/request.h>
#include <NaWServer/response.h>
#include <NaWServer/header_response.h>
namespace lcd7_web
{
	// Application policy; transport capacity is configured separately in sdkconfig.
	struct NetworkPolicy
	{
		unsigned httpd_connections = 21;
		unsigned rejection_reserve = 3;
		unsigned websocket_clients = 4;
		unsigned sse_clients = 4;
		unsigned retry_after_ms = 2000;
	};
	inline constexpr NetworkPolicy network_policy{};
	static_assert(network_policy.rejection_reserve > 0);
	static_assert(
		network_policy.websocket_clients > 0 && network_policy.sse_clients > 0 && network_policy.retry_after_ms > 0);
	static_assert(network_policy.rejection_reserve < network_policy.httpd_connections);
	static_assert(network_policy.httpd_connections + 3 <= CONFIG_LWIP_MAX_SOCKETS);
	static_assert(network_policy.httpd_connections <= CONFIG_LWIP_MAX_ACTIVE_TCP);
	struct NetworkState
	{
		unsigned tcp_open = 0, ws_open = 0, sse_open = 0;
		bool valid = false;
		uint64_t sampled_ms = 0, rejections = 0;
		const char *last_rejection = "none";
	};
	NetworkState network_state();
	void network_collect(unsigned connections, unsigned websocket_clients, unsigned sse_clients);
	const char *network_admission(const char *kind);
	void network_rejected(const char *reason);
	void encode_network(Writer &writer);
	NaWServer::SendResult network_busy(NaWServer::HeaderRequest &request, NaWServer::Response &response, const char *reason);
	NaWServer::SendResult network_busy(NaWServer::HeaderRequest &request, NaWServer::HeaderResponse &response, const char *reason);
} // namespace lcd7_web
