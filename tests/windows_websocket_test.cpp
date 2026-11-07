// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#ifndef NAWSERVER_TEST_TCP
#include <NaWServer/backends/windows.h>
#endif
#ifdef NAWSERVER_TEST_TCP
#include <NaWServer/backends/tcp.h>
#endif
#include <NaWServer/handler_static.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/websocket.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sockets = NaWServer::test::host;

using namespace NaWServer;
namespace
{
	unsigned checked = 0;
	const char *active_scenario = "startup";
	void check(bool value, int line)
	{
		++checked;
		if(!value)
		{
			std::printf("WINDOWS_WEBSOCKET_FAIL line=%d scenario=%s\n", line, active_scenario);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)
	void sendAll(sockets::Handle socket, const uint8_t *data, std::size_t size)
	{
		while(size)
		{
			const int sent = sockets::sendSome(socket, reinterpret_cast<const char *>(data), static_cast<int>(size), 0);
			CHECK(sent > 0);
			data += sent;
			size -= static_cast<std::size_t>(sent);
		}
	}

	void sendAll(sockets::Handle socket, const std::string &data)
	{
		sendAll(socket, reinterpret_cast<const uint8_t *>(data.data()), data.size());
	}

	sockets::Handle connectTo(uint16_t port)
	{
		const sockets::Handle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(socket != sockets::invalid);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		CHECK(connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
		CHECK(sockets::setTimeouts(socket, 3000));
		return socket;
	}

	std::string readHttpResponse(sockets::Handle socket)
	{
		std::string response;
		char bytes[2048];
		for(;;)
		{
			const int count = sockets::receiveSome(socket, bytes, sizeof(bytes), 0);
			CHECK(count >= 0);
			if(count == 0) break;

			response.append(bytes, static_cast<std::size_t>(count));
		}
		return response;
	}

	struct Frame
	{
		uint8_t opcode = 0;
		std::vector<uint8_t> payload;
	};

	struct WebSocketPeer
	{
		sockets::Handle socket = sockets::invalid;
		std::vector<uint8_t> buffered;
		WebSocketPeer() = default;
		WebSocketPeer(const WebSocketPeer &) = delete;
		WebSocketPeer &operator=(const WebSocketPeer &) = delete;
		WebSocketPeer(WebSocketPeer &&other) noexcept
		  : socket(other.socket), buffered(std::move(other.buffered))
		{
			other.socket = sockets::invalid;
		}

		WebSocketPeer &operator=(WebSocketPeer &&other) noexcept
		{
			if(this != &other)
			{
				if(socket != sockets::invalid) sockets::closeSocket(socket);

				socket = other.socket;
				buffered = std::move(other.buffered);
				other.socket = sockets::invalid;
			}

			return *this;
		}

		~WebSocketPeer()
		{
			if(socket != sockets::invalid) sockets::closeSocket(socket);
		}

	};

	void receiveExact(WebSocketPeer &peer, uint8_t *destination, std::size_t size)
	{
		while(size)
		{
			if(!peer.buffered.empty())
			{
				const auto count = std::min(size, peer.buffered.size());
				std::copy_n(peer.buffered.data(), count, destination);
				peer.buffered.erase(peer.buffered.begin(), peer.buffered.begin() + static_cast<std::ptrdiff_t>(count));
				destination += count;
				size -= count;
				continue;
			}

			const int count = sockets::receiveSome(peer.socket, reinterpret_cast<char *>(destination),
			        static_cast<int>(size), 0);
			if(count <= 0)
				std::printf("WINDOWS_WEBSOCKET_RECEIVE_FAIL remaining=%zu error=%d\n", size, sockets::lastError());

			CHECK(count > 0);
			destination += count;
			size -= static_cast<std::size_t>(count);
		}
	}

	Frame readServerFrame(WebSocketPeer &peer)
	{
		uint8_t header[2];
		receiveExact(peer, header, sizeof(header));
		CHECK((header[0] & 0x80) != 0 && (header[1] & 0x80) == 0);
		Frame frame;
		frame.opcode = header[0] & 0x0f;
		uint64_t length = header[1] & 0x7f;
		if(length == 126)
		{
			uint8_t extended[2];
			receiveExact(peer, extended, sizeof(extended));
			length = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
		}
		else if(length == 127)
		{
			uint8_t extended[8];
			receiveExact(peer, extended, sizeof(extended));
			length = 0;
			for(uint8_t byte : extended)
				length = (length << 8) | byte;
		}

		CHECK(length <= 8192);
		frame.payload.resize(static_cast<std::size_t>(length));
		if(length) receiveExact(peer, frame.payload.data(), frame.payload.size());

		return frame;
	}

	std::vector<uint8_t> clientFrame(uint8_t opcode, const uint8_t *payload, std::size_t size, bool final = true)
	{
		std::vector<uint8_t> frame;
		frame.push_back(static_cast<uint8_t>((final ? 0x80 : 0) | opcode));
		if(size < 126) frame.push_back(static_cast<uint8_t>(0x80 | size));
		else if(size <= 0xffff)
		{
			frame.push_back(0x80 | 126);
			frame.push_back(static_cast<uint8_t>(size >> 8));
			frame.push_back(static_cast<uint8_t>(size));
		}
		else
		{
			frame.push_back(0x80 | 127);
			for(int shift = 56; shift >= 0; shift -= 8)
				frame.push_back(static_cast<uint8_t>(static_cast<uint64_t>(size) >> shift));
		}

		const uint8_t mask[]{0x12, 0x34, 0x56, 0x78};
		frame.insert(frame.end(), std::begin(mask), std::end(mask));
		for(std::size_t index = 0; index < size; ++index)
			frame.push_back(payload[index] ^ mask[index & 3]);
		return frame;
	}

	void sendClientFrame(sockets::Handle socket, uint8_t opcode, const uint8_t *payload, std::size_t size,
	    bool final = true)
	{
		const auto frame = clientFrame(opcode, payload, size, final);
		sendAll(socket, frame.data(), frame.size());
	}

	WebSocketPeer connectWebSocket(uint16_t port, const std::string &key = "dGhlIHNhbXBsZSBub25jZQ==",
	    const std::string &path = "/ws")
	{
		WebSocketPeer peer;
		peer.socket = connectTo(port);
		const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
		    "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n\r\n";
		sendAll(peer.socket, request);
		std::string response;
		char byte;
		while(response.find("\r\n\r\n") == response.npos)
		{
			const int count = sockets::receiveSome(peer.socket, &byte, 1, 0);
			CHECK(count == 1);
			response.push_back(byte);
		}
		if(response.find("HTTP/1.1 101 Switching Protocols\r\n") != 0)
			std::printf("WINDOWS_WEBSOCKET_HANDSHAKE_RESPONSE=%s\n", response.c_str());

		CHECK(response.find("HTTP/1.1 101 Switching Protocols\r\n") == 0);
		CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != response.npos);
		return peer;
	}

	SendResult httpPing(Request &, Response &response)
	{
		return response.text(200, "http-alive");
	}

	class EchoWebSocket final : public WebSocketEndpoint
	{
	public:
		std::atomic<unsigned> connections{0};
		std::atomic<unsigned> messages{0};
		std::mutex mutex;
		std::vector<uint8_t> last_message;
		bool last_binary = false;

		explicit EchoWebSocket(const std::string &path = "/ws", WebSocketLimits limits = {})
		  : WebSocketEndpoint(path, limits)
		{
			onConnect([this](WebSocketClient *) {
				++connections;
			});
			onMessage([this](WebSocketClient *client, const uint8_t *data, size_t size, bool binary)
			{
				{
					std::lock_guard<std::mutex> lock(mutex);
					last_message.assign(data, data + size);
					last_binary = binary;
				}
				++messages;
				if(binary) (void)client->send_binary(data, size);
				else
				{
					std::string text(reinterpret_cast<const char *>(data), size);
					text.push_back('\0');
					(void)client->send(text.c_str());
				}
			});
		}

	};

	bool waitFor(const std::atomic<unsigned> &value, unsigned expected)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while(value.load() < expected && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return value.load() >= expected;
	}

	void waitForRetirement(const WebSocketEndpoint &endpoint)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while(endpoint.count() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));

		CHECK(endpoint.count() == 0);
	}

}

int main()
{
	CHECK(sockets::initialize() == 0);
#ifdef NAWSERVER_TEST_TCP
	sockets::TcpTransport transport;
	CHECK(transport.start().status == tcp::Status::Ok);
	TcpOptions options;
	options.port = 0;
	// This corpus includes an explicitly larger 8192-byte endpoint. The shared
	// pool must match that profile; the default 4x4096 profile is tested elsewhere.
	options.websocket_tx_block_size = 8192;
	TcpHttpBackend backend(transport, options);
#else
	WindowsHttpBackend backend(0);
#endif
	EchoWebSocket websocket;
	WebSocketLimits large_limits;
	large_limits.max_message_size = 8192;
	EchoWebSocket large_websocket("/ws-large", large_limits);
	HandlerStaticUriText index("/", "index");
	Route ping("/ping", HttpMethod::Get, httpPing);
	HandlerBase *const routes[]{&index, &ping, &websocket, &large_websocket};
	HttpServer server(backend, routes);
	CHECK(server.begin() == HttpError::Ok);
	const uint16_t port = backend.port();
	CHECK(port != 0);
	auto large_peer = connectWebSocket(port, "dGhlIHNhbXBsZSBub25jZQ==", "/ws-large");
	active_scenario = "large binary 5000";
	CHECK(waitFor(large_websocket.connections, 1));
	std::vector<uint8_t> large_message(5000, 0xa5);
	sendClientFrame(large_peer.socket, 2, large_message.data(), large_message.size());
	CHECK(waitFor(large_websocket.messages, 1));
	auto large_echo = readServerFrame(large_peer);
	CHECK(large_echo.opcode == 2 && large_echo.payload == large_message);
	large_message.assign(8192, 0x5a);
	active_scenario = "large binary 8192";
	sendClientFrame(large_peer.socket, 2, large_message.data(), large_message.size());
	CHECK(waitFor(large_websocket.messages, 2));
	large_echo = readServerFrame(large_peer);
	CHECK(large_echo.opcode == 2 && large_echo.payload == large_message);
	// Reject the declared length before receiving its body. Sending an entire
	// rejected body can reset TCP when the server closes with unread peer data.
	const uint8_t over_limit_header[]{0x82, 0xfe, 0x20, 0x01, 0x12, 0x34, 0x56, 0x78};
	active_scenario = "large declared length 8193";
	sendAll(large_peer.socket, over_limit_header, sizeof(over_limit_header));
	auto large_close = readServerFrame(large_peer);
	CHECK(large_close.opcode == 8 && large_close.payload == std::vector<uint8_t>({0x03, 0xf1}));
	sockets::closeSocket(large_peer.socket);
	large_peer.socket = sockets::invalid;
	// These protocol cases are sequential fixtures. Dedicated I/O relinquishes
	// a closed socket before the callback owner retires its bounded buffers.
	waitForRetirement(large_websocket);

	auto peer = connectWebSocket(port);
	active_scenario = "text broadcast";
	CHECK(waitFor(websocket.connections, 1));
	std::string copied_broadcast = "queued-copy";
	const auto broadcast = websocket.send(copied_broadcast.c_str());
	CHECK(broadcast.queued == 1 && broadcast.rejected == 0);
	copied_broadcast.assign("overwritten");
	auto echoed = readServerFrame(peer);
	CHECK(echoed.opcode == 1 && std::string(echoed.payload.begin(), echoed.payload.end()) == "queued-copy");
	const uint8_t producer_binary[]{0, 'Q', 0xff};
	active_scenario = "binary broadcast";
	const auto binary_broadcast = websocket.send_binary(producer_binary, sizeof(producer_binary));
	CHECK(binary_broadcast.queued == 1 && binary_broadcast.rejected == 0);
	echoed = readServerFrame(peer);
	CHECK(echoed.opcode == 2 && echoed.payload == std::vector<uint8_t>({0, 'Q', 0xff}));
	const std::string http_request = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
	const sockets::Handle http_socket = connectTo(port);
	sendAll(http_socket, http_request);
	const auto http_response = readHttpResponse(http_socket);
	sockets::closeSocket(http_socket);
	CHECK(http_response.find("HTTP/1.1 200") == 0 && http_response.find("http-alive") != http_response.npos);

	const std::string first = "frag";
	const std::string second = "mented";
	const uint8_t fragment_ping[]{'i', 'n'};
	auto combined_frames = clientFrame(1, reinterpret_cast<const uint8_t *>(first.data()), first.size(), false);
	const auto ping_frame = clientFrame(9, fragment_ping, sizeof(fragment_ping));
	const auto final_fragment = clientFrame(0, reinterpret_cast<const uint8_t *>(second.data()), second.size());
	combined_frames.insert(combined_frames.end(), ping_frame.begin(), ping_frame.end());
	combined_frames.insert(combined_frames.end(), final_fragment.begin(), final_fragment.end());
	sendAll(peer.socket, combined_frames.data(), combined_frames.size());
	active_scenario = "fragmented text plus ping";
	const auto fragment_pong = readServerFrame(peer);
	CHECK(fragment_pong.opcode == 10 && fragment_pong.payload == std::vector<uint8_t>({'i', 'n'}));
	CHECK(waitFor(websocket.messages, 1));
	echoed = readServerFrame(peer);
	CHECK(echoed.opcode == 1 && std::string(echoed.payload.begin(), echoed.payload.end()) == "fragmented");
	{
		std::lock_guard<std::mutex> lock(websocket.mutex);
		const std::vector<uint8_t> expected{'f', 'r', 'a', 'g', 'm', 'e', 'n', 't', 'e', 'd'};
		CHECK(!websocket.last_binary && websocket.last_message == expected);
	}

	const uint8_t binary[]{'A', 0, 'B'};
	active_scenario = "binary callback echo";
	sendClientFrame(peer.socket, 2, binary, sizeof(binary));
	CHECK(waitFor(websocket.messages, 2));
	echoed = readServerFrame(peer);
	CHECK(echoed.opcode == 2 && echoed.payload == std::vector<uint8_t>({'A', 0, 'B'}));
	{
		std::lock_guard<std::mutex> lock(websocket.mutex);
		CHECK(websocket.last_binary && websocket.last_message == std::vector<uint8_t>({'A', 0, 'B'}));
	}

	const uint8_t ping_payload[]{'o', 'k'};
	active_scenario = "ping pong";
	sendClientFrame(peer.socket, 9, ping_payload, sizeof(ping_payload));
	const auto pong = readServerFrame(peer);
	CHECK(pong.opcode == 10 && pong.payload == std::vector<uint8_t>({'o', 'k'}));

	const uint8_t close_payload[]{0x03, 0xe8};
	active_scenario = "normal close";
	sendClientFrame(peer.socket, 8, close_payload, sizeof(close_payload));
	const auto close_reply = readServerFrame(peer);
	CHECK(close_reply.opcode == 8 && close_reply.payload == std::vector<uint8_t>({0x03, 0xe8}));
	sockets::closeSocket(peer.socket);
	peer.socket = sockets::invalid;

	auto unmasked = connectWebSocket(port);
	const uint8_t invalid_frame[]{0x81, 0x01, 'x'};
	active_scenario = "unmasked frame";
	sendAll(unmasked.socket, invalid_frame, sizeof(invalid_frame));
	auto close_frame = readServerFrame(unmasked);
	CHECK(close_frame.opcode == 8 && close_frame.payload == std::vector<uint8_t>({0x03, 0xea}));
	sockets::closeSocket(unmasked.socket);
	unmasked.socket = sockets::invalid;

	auto oversized = connectWebSocket(port);
	const uint8_t oversized_header[]{0x82, 0xfe, 0x10, 0x01, 0x12, 0x34, 0x56, 0x78};
	active_scenario = "declared length 4097";
	sendAll(oversized.socket, oversized_header, sizeof(oversized_header));
	close_frame = readServerFrame(oversized);
	CHECK(close_frame.opcode == 8 && close_frame.payload == std::vector<uint8_t>({0x03, 0xf1}));
	sockets::closeSocket(oversized.socket);
	oversized.socket = sockets::invalid;

	auto invalid_close = connectWebSocket(port);
	const uint8_t invalid_close_payload[]{0x03};
	active_scenario = "invalid close length 1";
	sendClientFrame(invalid_close.socket, 8, invalid_close_payload, sizeof(invalid_close_payload));
	close_frame = readServerFrame(invalid_close);
	CHECK(close_frame.opcode == 8 && close_frame.payload == std::vector<uint8_t>({0x03, 0xea}));
	sockets::closeSocket(invalid_close.socket);
	invalid_close.socket = sockets::invalid;

	auto invalid_handshake = connectTo(port);
	const std::string bad_request = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
	    "Connection: Upgrade\r\nSec-WebSocket-Version: 12\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
	sendAll(invalid_handshake, bad_request);
	active_scenario = "invalid handshake version";
	const auto bad_response = readHttpResponse(invalid_handshake);
	sockets::closeSocket(invalid_handshake);
	CHECK(bad_response.find("HTTP/1.1 400") == 0);

	peer = connectWebSocket(port);
	CHECK(waitFor(websocket.connections, 5));
	CHECK(server.end() == HttpError::Ok);
	CHECK(websocket.count() == 0);
	sockets::closeSocket(peer.socket);
	peer.socket = sockets::invalid;
	CHECK(server.begin() == HttpError::Ok);
	peer = connectWebSocket(backend.port());
	CHECK(waitFor(websocket.connections, 6));
	sendClientFrame(peer.socket, 8, close_payload, sizeof(close_payload));
	active_scenario = "normal close after restart";
	(void)readServerFrame(peer);
	sockets::closeSocket(peer.socket);
	peer.socket = sockets::invalid;
	CHECK(server.end() == HttpError::Ok);
	CHECK(websocket.count() == 0);
#ifdef NAWSERVER_TEST_TCP
	CHECK(transport.stop().status == tcp::Status::Ok);
#endif
	CHECK(sockets::cleanup() == 0);
	std::printf("WINDOWS_WEBSOCKET checks=%u PASS\n", checked);
}
