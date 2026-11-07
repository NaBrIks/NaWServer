// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/sse.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/websocket.h>
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/esp_idf_tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "checks.hpp"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace NaWServer;
using namespace NaWServer::sse;
namespace
{
	std::size_t passed = 0;
#define REQUIRE(e)                                                                                                     \
		do                                                                                                                 \
		{                                                                                                                  \
			++passed;                                                                                                      \
			if(!(e))                                                                                                       \
			{                                                                                                              \
				std::printf("SSE_DEVICE_FAIL line=%d expr=%s\n", __LINE__, #e);                                            \
				std::abort();                                                                                              \
			}                                                                                                              \
		} while(false)
	tcp::IdfTransport transport;
	TcpOptions options()
	{
		TcpOptions value;
		value.bind_address = "127.0.0.1";
		value.port = 8081;
		return value;
	}
	TcpHttpBackend backend(transport, options());
	TcpAdapter event_backend(backend, Limits{4, 3, 2304, 8192, 200, 500, 1500});
	EventSource adapter(event_backend);
	std::array<Token, 64> tokens{};
	std::array<LastEventId, 64> last_ids{};
	std::atomic<unsigned> admitted{0};
	std::uint64_t producer_max_us = 0;
	std::uint64_t ms()
	{
		return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
	}

	void yield_tick()
	{
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	SendResult events(Request &request, Response &response)
	{
		unsigned index = 0;
		const auto parameter = request.parameter("i").value_or(std::string_view{});
		const std::string value(parameter);
		if(std::sscanf(value.c_str(), "%u", &index) != 1 || index >= tokens.size())
			return response.text(400, "Invalid index");
		const auto opened = adapter.open(request, response);
		if(opened.result == SendResult::Ok)
		{
			tokens[index] = opened.token;
			last_ids[index] = opened.last_id;
			admitted.fetch_add(1, std::memory_order_release);
			return SendResult::Ok;
		}
		return response.text(opened.admission == Admit::InvalidLastId ? 400 : 503, "refused");
	}
	SendResult health(Response &response)
	{
		return response.text(200, "ok");
	}
	WebSocketEndpoint websocket("/ws");
	Route event_route("/events", HttpMethod::Get, events);
	Route health_route("/health", HttpMethod::Get, health);
	HandlerBase *const routes[] = {&event_route, &health_route, &websocket};
	HttpServer server(backend, routes);

	int connect_client()
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		REQUIRE(fd >= 0);
		timeval timeout{3, 0};
		REQUIRE(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(8081);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		return fd;
	}

	void send_all(int fd, const std::string &data)
	{
		std::size_t offset = 0;
		while(offset < data.size())
		{
			int n = send(fd, data.data() + offset, data.size() - offset, 0);
			REQUIRE(n > 0);
			offset += static_cast<std::size_t>(n);
		}
	}

	void read_exact(int fd, char *data, std::size_t size)
	{
		std::size_t offset = 0;
		while(offset < size)
		{
			const int n = recv(fd, data + offset, size - offset, 0);
			REQUIRE(n > 0);
			offset += static_cast<std::size_t>(n);
		}
	}

	std::string read_until(int fd, const std::string &suffix)
	{
		std::string out;
		while(out.size() < 4096)
		{
			char byte = 0;
			int n = recv(fd, &byte, 1, 0);
			REQUIRE(n == 1);
			out += byte;
			if(out.size() >= suffix.size() && out.compare(out.size() - suffix.size(), suffix.size(), suffix) == 0)
				return out;
		}
		REQUIRE(false);
		return {};
	}

	int open_event(unsigned i, const std::string &extra = "", bool expect_success = true)
	{
		auto before = admitted.load(std::memory_order_acquire);
		int fd = connect_client();
		send_all(fd, "GET /events?i=" + std::to_string(i) + " HTTP/1.1\r\nHost: localhost\r\n" + extra + "\r\n");
		auto header = read_until(fd, "\r\n\r\n");
		REQUIRE(header.find(expect_success ? "200 OK" : "503 Service Unavailable") != std::string::npos);
		if(expect_success)
		{
			auto deadline = ms() + 1000;
			while(admitted.load(std::memory_order_acquire) == before && ms() < deadline)
				yield_tick();
			REQUIRE(admitted.load(std::memory_order_acquire) > before);
		}

		return fd;
	}

	Publish publish(unsigned i, const Frame &f)
	{
		Publish result;
		auto deadline = ms() + 1000;
		do
		{
			auto start = esp_timer_get_time();
			result = adapter.publish(tokens[i], f);
			producer_max_us = std::max(producer_max_us, static_cast<std::uint64_t>(esp_timer_get_time() - start));
			if(result == Publish::Busy)
				yield_tick();
		} while(result == Publish::Busy && ms() < deadline);
		return result;
	}

	void wait_active(std::size_t n)
	{
		auto deadline = ms() + 4000;
		while(adapter.active() != n && ms() < deadline)
			yield_tick();
		REQUIRE(adapter.active() == n);
	}

	void request_close(unsigned i, int fd)
	{
		auto deadline = ms() + 1000;
		while(!adapter.close(tokens[i]) && ms() < deadline)
			yield_tick();
		::close(fd);
	}

	void health_and_ws()
	{
		int fd = connect_client();
		send_all(fd, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
		REQUIRE(read_until(fd, "\r\n\r\n").find("200 OK") != std::string::npos);
		char ok[2];
		read_exact(fd, ok, 2);
		REQUIRE(std::memcmp(ok, "ok", 2) == 0);
		::close(fd);
		fd = connect_client();
		send_all(
			fd,
			"GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
			"dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
		REQUIRE(read_until(fd, "\r\n\r\n").find("101") != std::string::npos);
		const char wire[] = {static_cast<char>(0x81), static_cast<char>(0x81), 1, 2, 3, 4, static_cast<char>('x' ^ 1)};
		send_all(fd, std::string(wire, sizeof(wire)));
		char reply[3];
		read_exact(fd, reply, 3);
		REQUIRE(static_cast<unsigned char>(reply[0]) == 0x81 && reply[1] == 1 && reply[2] == 'x');
		::close(fd);
	}

} // namespace
extern "C" void app_main()
{
	static_assert(__cplusplus == 201703L, "C++17 required");
	// Serial monitor may attach after vendor reset. No Wi-Fi/NVS/credentials used.
	vTaskDelay(pdMS_TO_TICKS(7000));
	auto common = checks();
	std::printf(
		"SSE_COMMON passed=%zu cplusplus=%ld sizeof_sessions=%zu sizeof_adapter=%zu\n",
		common,
		__cplusplus,
		sizeof(Sessions),
		sizeof(TcpAdapter));
	REQUIRE(esp_netif_init() == ESP_OK);
	REQUIRE(esp_event_loop_create_default() == ESP_OK);
	REQUIRE(adapter.start());
	websocket.onMessage([](WebSocketClient *client, const uint8_t *bytes, size_t size, bool binary)
	{
		if(binary) (void)client->send_binary(bytes, size);
		else { const std::string text(reinterpret_cast<const char *>(bytes), size); (void)client->send(text.c_str()); }
	});
	REQUIRE(transport.start().status == tcp::Status::Ok);
	REQUIRE(server.begin() == HttpError::Ok);
	Frame event;
	REQUIRE(encode({u8"температура\n23\n", "reading", "boot:42", 1000}, event) == EncodeResult::Ok);
	std::array<int, 4> clients{};
	clients[0] = open_event(0, "Last-Event-ID: boot:41\r\n");
	REQUIRE(last_ids[0].present && std::string(last_ids[0].bytes.data(), last_ids[0].size) == "boot:41");
	clients[1] = open_event(1);
	REQUIRE(!last_ids[1].present);
	clients[2] = open_event(2, "Last-Event-ID:\r\n");
	REQUIRE(last_ids[2].present && last_ids[2].size == 0);
	clients[3] = open_event(3);
	wait_active(4);
	auto rejected = open_event(4, "", false);
	::close(rejected);
	REQUIRE(adapter.active() == 4);
	health_and_ws();
	for(unsigned i = 0; i < 4; ++i)
	{
		REQUIRE(publish(i, event) == Publish::Queued);
		auto wire = read_until(clients[i], "\n\n");
		while(wire.front() == ':')
			wire = read_until(clients[i], "\n\n");
		REQUIRE(wire == std::string(event.bytes.data(), event.size));
	}
	// Idle heartbeats are independent of absolute subscription age.
	vTaskDelay(pdMS_TO_TICKS(1800));
	REQUIRE(adapter.active() == 4);
	for(unsigned i = 0; i < 4; ++i)
	{
		request_close(i, clients[i]);
	}
	wait_active(0);
	vTaskDelay(pdMS_TO_TICKS(100));
	const auto heap_before = esp_get_free_heap_size();
	for(unsigned round = 0; round < 5; ++round)
	{
		for(unsigned i = 5; i < 25; ++i)
		{
			int fd = open_event(i);
			REQUIRE(publish(i, event) == Publish::Queued); // close with a possibly active send
			request_close(i, fd);
			wait_active(0);
		}
		vTaskDelay(pdMS_TO_TICKS(100));
		std::printf(
			"SSE_CHURN cycles=%u heap_before=%lu heap_after=%lu\n",
			20 * (round + 1),
			static_cast<unsigned long>(heap_before),
			static_cast<unsigned long>(esp_get_free_heap_size()));
	}
	// A reader stops consuming after the headers. Publish only after the previous
	// frame left the queue: this isolates network stall from producer overflow.
	int slow = open_event(25);
	int healthy1 = open_event(30), healthy2 = open_event(31);
	Frame large;
	REQUIRE(encode({std::string(700, 'z'), {}, {}, {}}, large) == EncodeResult::Ok);
	auto before = adapter.stats();
	auto start = ms();
	unsigned queued = 0;
	bool mixed_checked = false;
	while(adapter.active() > 2 && ms() - start < 12000)
	{
		if(adapter.stats().queued_bytes == 0)
		{
			auto result = publish(25, large);
			REQUIRE(result == Publish::Queued || result == Publish::Closed);
			if(result == Publish::Queued)
				++queued;
		}

		if(queued >= 5 && !mixed_checked)
		{
			health_and_ws();
			for(unsigned i : {30u, 31u})
			{
				REQUIRE(publish(i, event) == Publish::Queued);
				auto wire = read_until(i == 30 ? healthy1 : healthy2, "\n\n");
				while(wire.front() == ':')
					wire = read_until(i == 30 ? healthy1 : healthy2, "\n\n");
				REQUIRE(wire == std::string(event.bytes.data(), event.size));
			}
			mixed_checked = true;
		}

		yield_tick();
	}
	wait_active(2);
	::close(slow);
	REQUIRE(mixed_checked);
	auto after = adapter.stats();
	REQUIRE(
		after.closed_by_reason[static_cast<std::size_t>(CloseReason::Stall)] >
		before.closed_by_reason[static_cast<std::size_t>(CloseReason::Stall)]);
	REQUIRE(
		after.closed_by_reason[static_cast<std::size_t>(CloseReason::Overflow)] ==
		before.closed_by_reason[static_cast<std::size_t>(CloseReason::Overflow)]);
	std::printf(
		"SSE_STALL frames=%u elapsed_ms=%llu producer_max_us=%llu\n",
		queued,
		static_cast<unsigned long long>(ms() - start),
		static_cast<unsigned long long>(producer_max_us));
	request_close(30, healthy1);
	request_close(31, healthy2);
	wait_active(0);
	// Shutdown with four outstanding requests and pending data; repeat stop is safe.
	for(unsigned i = 26; i < 30; ++i)
	{
		clients[i - 26] = open_event(i);
		REQUIRE(publish(i, large) == Publish::Queued);
	}
	REQUIRE(adapter.stop(3000));
	REQUIRE(adapter.stop(0));
	REQUIRE(adapter.active() == 0);
	REQUIRE(adapter.stats().queued_bytes == 0);
	REQUIRE(adapter.close_errors() == 0);
	for(auto fd : clients)
		::close(fd);
	REQUIRE(server.end() == HttpError::Ok);
	REQUIRE(adapter.completions() == admitted.load(std::memory_order_acquire));
	vTaskDelay(pdMS_TO_TICKS(100));
	std::printf(
		"SSE_DONE passed=%zu admitted=%u completed=%zu heap_before_churn=%lu heap_after_stop=%lu "
		"producer_max_us=%llu\n",
		passed,
		admitted.load(),
		adapter.completions(),
		static_cast<unsigned long>(heap_before),
		static_cast<unsigned long>(esp_get_free_heap_size()),
		static_cast<unsigned long long>(producer_max_us));
	for(;;)
		vTaskDelay(pdMS_TO_TICKS(1000));
}
