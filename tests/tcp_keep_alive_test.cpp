// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
namespace sockets = NaWServer::test::host;
using namespace NaWServer;
namespace
{
	void check(bool value, int line)
	{
		if(!value)
		{
			std::printf("KEEP_ALIVE_FAIL %d native=%d\n", line, sockets::lastError()); std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	void send(sockets::Handle socket, std::string_view bytes)
	{
		while(!bytes.empty())
		{
			const auto count = sockets::sendSome(socket, bytes.data(), bytes.size());
			CHECK(count > 0);
			bytes.remove_prefix(static_cast<std::size_t>(count));
		}
	}

	std::string receive(sockets::Handle socket)
	{
		std::string data;
		char byte;
		while(data.find("\r\n\r\n") == data.npos)
		{
			CHECK(sockets::receiveSome(socket, &byte, 1) == 1 && data.size() < 4096);
			data.push_back(byte);
		}
		const auto length = data.find("Content-Length: ");
		CHECK(length != data.npos);
		const auto count = std::strtoul(data.c_str() + length + 16, nullptr, 10);
		CHECK(count < 4096);
		for(std::size_t index = 0; index < count; ++index)
		{
			CHECK(sockets::receiveSome(socket, &byte, 1) == 1);
			data.push_back(byte);
		}
		return data;
	}

	sockets::Handle connectTo(std::uint16_t port)
	{
		const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(socket != sockets::invalid && sockets::setTimeouts(socket, 2000));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		CHECK(connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
		return socket;
	}

}
int main()
{
	CHECK(sockets::initialize() == 0);
	sockets::TcpTransport transport;
	CHECK(transport.start().status == tcp::Status::Ok);
	TcpOptions options;
	options.port = 0;
	options.http_memory.enabled = true;
	options.http_memory.initial_requests = 0;
	options.keep_alive.enabled = true;
	options.keep_alive.maximum_requests = 32;
	options.keep_alive.idle_timeout_ms = 500;
	std::atomic<std::size_t> available{1000000};
	options.buffer_memory.allocator = {&available,
		                               [](void *, std::size_t bytes) -> void * {
										   return ::operator new(bytes, std::nothrow);
									   },
		                               [](void *, void *pointer, std::size_t) {
										   ::operator delete(pointer);
									   },
		                               [](void *context, BufferAvailability &sample)
									   {
										   const auto bytes = static_cast<std::atomic<std::size_t> *>(context)->load();
										   sample = {bytes, bytes};
										   return true;
									   }};
	options.buffer_memory.min_free_bytes = 1024;
	TcpHttpBackend backend(transport, options);
	detail::BufferPool parent;
	CHECK(parent.initialize({{}, 4096}, BufferGroup::Tcp));
	MemoryManager output;
	CHECK(output.initialize(parent, {0, 1, 32}));
	std::atomic<unsigned> calls{0};
	auto endpoint = [&output, &calls](Response &response)
		{
			++calls;
			auto block = output.acquire(5);
			CHECK(block);
			std::memcpy(block.data(), "owned", 5);
			return response.sendOwned(200, "text/plain", std::move(block), 5);
		};
	Route route("/", HttpMethod::Get, EndpointRef(endpoint));
	auto pressure = [&available](Response &response)
		{
			available = 1024;
			static const std::string bytes(4096, 'x');
			return response.send(200, "text/plain",
					   {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
		};
	Route pressure_route("/pressure", HttpMethod::Get, EndpointRef(pressure));
	HandlerBase *const handlers[]{&route, &pressure_route};
	HttpServer server(backend, handlers);
	CHECK(server.begin() == HttpError::Ok);
	const auto initial = backend.bufferMemoryStats().used_bytes;
	const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	CHECK(socket != sockets::invalid && sockets::setTimeouts(socket, 2000));
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(backend.port());
	CHECK(connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
	for(unsigned index = 0; index < 20; ++index)
	{
		send(socket, "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
		const auto response = receive(socket);
		CHECK(response.find("HTTP/1.1 200") == 0 && response.find("Connection: keep-alive") != response.npos);
		CHECK(response.substr(response.size() - 5) == "owned");
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while(backend.bufferMemoryStats().used_bytes != initial)
	{
		CHECK(std::chrono::steady_clock::now() < deadline);
		std::this_thread::yield();
	}
	const auto memory = backend.managedMemoryStats();
	CHECK(memory.headers.active_blocks == 0 && memory.bodies.allocated_bytes == 0 && memory.responses.peak_bytes == 0);
	CHECK(output.stats().active_blocks == 0 && output.stats().allocated_bytes == 0);
	send(socket, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
	CHECK(receive(socket).find("Connection: close") != std::string::npos);
	char byte;
	CHECK(sockets::receiveSome(socket, &byte, 1) == 0);
	sockets::closeSocket(socket);
	const auto denied = connectTo(backend.port());
	send(denied, "GET /pressure HTTP/1.1\r\nHost: test\r\n\r\n");
	CHECK(receive(denied).find("HTTP/1.1 503") == 0);
	sockets::closeSocket(denied);
	CHECK(backend.managedMemoryStats().responses.blocked == MemoryBlockReason::LowMemory);
	const auto early = connectTo(backend.port());
	send(early, "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
	CHECK(receive(early).find("HTTP/1.1 503") == 0);
	sockets::closeSocket(early);
	CHECK(calls == 21);
	available = 1000000;
	const auto idle = connectTo(backend.port());
	send(idle, "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
	CHECK(receive(idle).find("HTTP/1.1 200") == 0);
	CHECK(sockets::receiveSome(idle, &byte, 1) == 0); // Idle timeout releases only this socket.
	sockets::closeSocket(idle);
	CHECK(calls == 22 && server.end() == HttpError::Ok);
	CHECK(backend.bufferMemoryStats().used_bytes == 0 && transport.ownedHandles() == 0);
	CHECK(transport.stop().status == tcp::Status::Ok && sockets::cleanup() == 0);
	std::puts("KEEP_ALIVE_PASS same_socket=21 no_idle_payload=1 owned_no_copy=1");
}
