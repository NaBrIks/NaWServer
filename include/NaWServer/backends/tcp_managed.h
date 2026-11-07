// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp_transport.h>

#ifndef NAWSERVER_TCP_MANAGED_HEAD_BYTES
#define NAWSERVER_TCP_MANAGED_HEAD_BYTES 1024
#endif
#ifndef NAWSERVER_TCP_MANAGED_RESPONSE_BYTES
#define NAWSERVER_TCP_MANAGED_RESPONSE_BYTES 4096
#endif
#ifndef NAWSERVER_TCP_MANAGED_STREAM_BYTES
#define NAWSERVER_TCP_MANAGED_STREAM_BYTES 16384
#endif
#ifndef NAWSERVER_TCP_MANAGED_BODY_BLOCK_BYTES
#define NAWSERVER_TCP_MANAGED_BODY_BLOCK_BYTES 1024
#endif

namespace NaWServer
{

	// Physical workspaces and timeouts only. Client identity and admission policy
	// belong to the early middleware chain and never to this transport object.
	struct TcpManagedOptions
	{
		std::uint16_t ingress_slots = 2;
		std::size_t header_bytes = NAWSERVER_TCP_MANAGED_HEAD_BYTES;
		std::size_t response_header_bytes = 512;
		std::size_t response_body_bytes = NAWSERVER_TCP_MANAGED_RESPONSE_BYTES;
		std::size_t request_body_block_bytes = NAWSERVER_TCP_MANAGED_BODY_BLOCK_BYTES;
		std::size_t stream_bytes_per_client = NAWSERVER_TCP_MANAGED_STREAM_BYTES;
		std::uint16_t websocket_tx_blocks_per_client = 1;
		std::uint32_t header_timeout_ms = 500;
		std::uint32_t reject_timeout_ms = 250;
		std::size_t io_slice_bytes = 1024;
	};

} // namespace NaWServer
