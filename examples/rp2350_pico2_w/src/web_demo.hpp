#pragma once
#include <cstdint>
#include "../../common/demo_key_config.hpp"
#include <NaWServer/backends/tcp_transmission.h>
namespace rp_demo
{
	inline constexpr std::uint16_t http_port = 80;
	inline constexpr std::uint16_t websocket_client_limit = NAWSERVER_DEMO_SHARED_CLIENTS;
	// Four pages can each keep WS and SSE open; leave two transient HTTP slots.
	inline constexpr std::uint16_t http_connection_limit = 14;
	bool startWebServer();
	void pollWebDemo();
	NaWServer::tcp::TransmissionStats transmissionSnapshot();
}
