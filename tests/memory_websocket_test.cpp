// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/memory.h>
#include <NaWServer/http_server.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/websocket.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace NaWServer;
namespace
{
	unsigned checks = 0;
	WebSocketSendResult connect_queue_result = WebSocketSendResult::Queued;
	WebSocketSendResult connect_overflow_result = WebSocketSendResult::Queued;
	void check(bool condition, int line)
	{
		++checks;
		if(!condition) { std::printf("MEMORY_WS_FAIL line=%d\n", line); std::fflush(stdout); std::abort(); }
	}

#define CHECK(value) check((value), __LINE__)

	void echoMessage(WebSocketClient *client, const uint8_t *data, std::size_t size, bool binary)
	{
		if(binary)
			(void)client->send_binary(data, size);
		else
		{
			std::string message;
			if(size) message.assign(reinterpret_cast<const char *>(data), size);

			(void)client->send(message.c_str());
		}
	}

}

int main()
{
	MemoryHttpBackend backend;
	WebSocketLimits limits;
	limits.max_message_size = 4096;
	limits.max_pending_messages = 4;
	limits.max_clients = 2;
	WebSocketEndpoint websocket("/ws", limits);
	HandlerStaticUriText http_route("/http", "alive");
	HandlerBase *const routes[]{&websocket, &http_route};
	HttpServer server(backend, routes);
	websocket.onConnect([](WebSocketClient *client)
	{
		for(unsigned index = 0; index < 4; ++index)
			connect_queue_result = client->send_binary(reinterpret_cast<const uint8_t *>("q"), 1);
		connect_overflow_result = client->send_binary(reinterpret_cast<const uint8_t *>("q"), 1);
	});
	websocket.onMessage(echoMessage);
	CHECK(server.begin() == HttpError::Ok);
	CHECK(backend.connectWebSocket("/missing").generation == 0);
	const auto first = backend.connectWebSocket("/ws");
	CHECK(first && websocket.count() == 1 && backend.websocketCount() == 1);
	CHECK(connect_queue_result == WebSocketSendResult::Queued);
	CHECK(connect_overflow_result == WebSocketSendResult::QueueFull);
	CHECK(backend.pendingMessageCount(first) == 4);
	CHECK(websocket.send_binary(nullptr, 1).rejected == 1);
	CHECK(websocket.send_binary(nullptr, 1).first_error == WebSocketSendResult::InvalidArgument);
	CHECK(backend.receive({}, nullptr, 0, false) == WebSocketReadStatus::Error);
	auto sent = backend.takeSent(first);
	CHECK(sent.size() == 4 && sent[0].binary && sent[0].bytes == std::vector<uint8_t>{'q'});
	CHECK(backend.pendingMessageCount(first) == 0);

	const uint8_t text[] = {'h', 'i'};
	CHECK(backend.receive(first, text, sizeof(text), false) == WebSocketReadStatus::Message);
	sent = backend.takeSent(first);
	CHECK(sent.size() == 1 && !sent[0].binary && sent[0].bytes == std::vector<uint8_t>({'h', 'i'}));
	std::vector<uint8_t> binary{0, 0xff, 'x', 0};
	CHECK(backend.receive(first, binary, true) == WebSocketReadStatus::Message);
	std::fill(binary.begin(), binary.end(), 0x55);
	sent = backend.takeSent(first);
	CHECK(sent.size() == 1 && sent[0].binary && sent[0].bytes == std::vector<uint8_t>({0, 0xff, 'x', 0}));
	CHECK(backend.receive(first, nullptr, 0, true) == WebSocketReadStatus::Message);
	sent = backend.takeSent(first);
	CHECK(sent.size() == 1 && sent[0].binary && sent[0].bytes.empty());
	CHECK(backend.receive(first, nullptr, 0, false) == WebSocketReadStatus::Message);
	sent = backend.takeSent(first);
	CHECK(sent.size() == 1 && !sent[0].binary && sent[0].bytes.empty());
	std::vector<uint8_t> maximum_payload(4096, 0xa5);
	CHECK(backend.receive(first, maximum_payload, true) == WebSocketReadStatus::Message);
	sent = backend.takeSent(first);
	CHECK(sent.size() == 1 && sent[0].bytes == maximum_payload);
	std::vector<uint8_t> oversized_payload(4097, 0);
	CHECK(backend.receive(first, oversized_payload, true) == WebSocketReadStatus::Closed);
	CHECK(websocket.count() == 0 && backend.websocketCount() == 0);
	CHECK(backend.receive(first, text, sizeof(text), false) == WebSocketReadStatus::Closed);

	const auto second = backend.connectWebSocket("/ws");
	const auto third = backend.connectWebSocket("/ws");
	CHECK(second && third && websocket.count() == 2);
	CHECK(!backend.connectWebSocket("/ws"));
	CHECK(backend.takeSent(second).size() == 4 && backend.takeSent(third).size() == 4);
	CHECK(websocket.send("broadcast").queued == 2);
	CHECK(backend.takeSent(second).size() == 1 && backend.takeSent(third).size() == 1);
	MemoryRequest http_request;
	http_request.target = "/http";
	const auto http_response = backend.perform(http_request);
	CHECK(http_response.status == 200 && websocket.count() == 2);
	CHECK(backend.close(second));
	CHECK(!backend.close(second) && websocket.count() == 1);
	CHECK(server.end() == HttpError::Ok && !websocket.count() && !backend.websocketCount());
	CHECK(backend.receive(third, text, sizeof(text), false) == WebSocketReadStatus::Closed);
	CHECK(server.begin() == HttpError::Ok);
	const auto after_restart = backend.connectWebSocket("/ws");
	CHECK(after_restart && after_restart.generation != third.generation);
	CHECK(backend.receive(third, text, sizeof(text), false) == WebSocketReadStatus::Closed);
	CHECK(websocket.count() == 1 && backend.websocketCount() == 1);
	CHECK(server.end() == HttpError::Ok);

	WebSocketLimits invalid_limits;
	invalid_limits.max_clients = 0;
	WebSocketEndpoint invalid_websocket("/invalid", invalid_limits);
	HandlerBase *const invalid_routes[]{&invalid_websocket};
	MemoryHttpBackend invalid_backend;
	HttpServer invalid_server(invalid_backend, invalid_routes);
	CHECK(invalid_server.begin() == HttpError::InvalidArgument);
	HttpLimits invalid_http_limits;
	invalid_http_limits.max_connections = 0;
	MemoryHttpBackend invalid_http_backend;
	struct EmptyHandler final : HttpRequestHandler
	{
		ExchangeDisposition handle(HttpExchange &) override
		{
			return ExchangeDisposition::Complete;
		}

	} handler;
	CHECK(invalid_http_backend.start(handler, invalid_http_limits) == HttpError::InvalidArgument);
	std::printf("MEMORY_WS_PASS checks=%u\n", checks);
}
