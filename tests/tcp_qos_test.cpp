// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/admission_http.h>
#include <NaWServer/route.h>
#include <array>
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
		if(!value) { std::printf("TCP_QOS_FAIL line=%d\n", line); std::fflush(stdout); std::abort(); }
	}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	struct Socket
	{
		sockets::Handle value = sockets::invalid;
		~Socket()
		{
			if(value != sockets::invalid)
				sockets::closeSocket(value);
		}

		Socket(const Socket &) = delete;
		explicit Socket(sockets::Handle handle)
		  : value(handle)
		{
		}

		Socket(Socket &&other) noexcept
		  : value(other.value)
		{
			other.value = sockets::invalid;
		}

	};
	Socket connectTo(std::uint16_t port)
	{
		Socket result{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
		CHECK(result.value != sockets::invalid && sockets::setTimeouts(result.value, 2000));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		CHECK(connect(result.value, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
		return result;
	}

	void send(Socket &socket, const std::string &request)
	{
		std::size_t offset = 0;
		while(offset < request.size())
		{
			const int sent = sockets::sendSome(socket.value, request.data() + offset, request.size() - offset);
			CHECK(sent > 0);
			offset += static_cast<std::size_t>(sent);
		}
	}

	std::string request(std::uint16_t port, const char *key, bool await_eof = false)
	{
		auto socket = connectTo(port);
		send(socket, std::string("GET /small HTTP/1.1\r\nHost: localhost\r\nX-Client-Key: ") + key + "\r\n\r\n");
		std::string result;
		char buffer[1024];
		for(;;)
		{
			const int read = sockets::receiveSome(socket.value, buffer, sizeof(buffer));
			CHECK(read >= 0 && result.size() < 16384);
			if(!read) return result;

			result.append(buffer, static_cast<std::size_t>(read));
			const auto head = result.find("\r\n\r\n");
			const auto length = result.find("Content-Length: ");
			if(!await_eof && head != result.npos && length != result.npos &&
			    result.size() >= head + 4 + std::strtoul(result.c_str() + length + 16, nullptr, 10)) return result;
		}
	}

	class Handler final : public HandlerBase
	{
	public:
		std::atomic<bool> bulk_started{false};
		std::atomic<unsigned> small_called{0};
		static const std::array<std::uint8_t, 4 * 1024 * 1024> bulk;
		RouteDescriptors routes() const override
		{
			static constexpr RouteDescriptor descriptor{"/bulk", HttpMethod::Get};
			return {&descriptor, 1};
		}

		void handleRequest(Request *request, Response &response) override
		{
			const auto *peer =
			    static_cast<const tcp::PeerAddress *>(request->transportExtension(&tcp::peer_address_key));
			CHECK(peer && peer->valid && peer->ipv4 == 0x7f000001 && peer->port);
			if(request->path() == "/bulk")
			{
				CHECK(response.sendImmutable(200, "application/octet-stream",
				    {bulk.data(), bulk.size()}) == SendResult::Ok);
				bulk_started = true;
				return;
			}

			++small_called;
			char body[] = "before";
			CHECK(response.header("X-Captured", "original") == SendResult::Ok);
			CHECK(response.text(200, body) == SendResult::Ok);
			body[0] = 'X'; // The queued response must not retain this callback's stack.
		}

	};
	const std::array<std::uint8_t, 4 * 1024 * 1024> Handler::bulk{};
	std::uint64_t now()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				   std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	void boundedIngress(sockets::TcpTransport &transport)
	{
		AdmissionOptions configuration;
		configuration.reserved_key = "primary";
		configuration.reserved = configuration.shared = {1, 0, 0, 0, 1000};
		Admission<1> policy(configuration);
		AdmissionMiddleware middleware(AdmissionRef(policy), now);
		auto chain = makeHeaderChain(middleware);
		TcpOptions options;
		options.port = 0;
		options.buffer_memory.max_bytes = 65536;
		options.http_memory.enabled = true;
		options.http_memory.initial_requests = 2;
		options.http_memory.reserved_requests = 1;
		options.managed.ingress_slots = 4;
		TcpHttpBackend backend(transport, options);
		auto endpoint = [](Response &response) {
				return response.text(200, "alive");
			};
		Route route("/small", HttpMethod::Get, EndpointRef(endpoint));
		HandlerBase *const routes[]{&route};
		HttpServer server(backend, routes, {}, {}, {}, chain);
		CHECK(server.begin() == HttpError::Ok);
		CHECK(request(backend.port(), "guest").find("HTTP/1.1 200") == 0);
		{
			auto slow_headers = connectTo(backend.port());
			send(slow_headers, "GET /small HTTP/1.1\r\nHost:");
			// TCP has no identity policy. A slow header uses a bounded generic slot;
			// completed headers reach middleware, including the protected API key.
			CHECK(request(backend.port(), "extra", true).find("HTTP/1.1 503") == 0);
			CHECK(request(backend.port(), "primary").find("HTTP/1.1 200") == 0);
		}
		CHECK(server.end() == HttpError::Ok);
		CHECK(backend.bufferMemoryStats().used_bytes == 0 && transport.ownedHandles() == 0);
	}

}

int main()
{
	CHECK(sockets::initialize() == 0);
	sockets::TcpTransport transport;
	CHECK(transport.start().status == tcp::Status::Ok);
	AdmissionOptions configuration;
	configuration.reserved_key = "primary";
	configuration.reserved = {1, 0, 0, 0, 1000};
	configuration.shared = {1, 0, 0, 3, 10000};
	Admission<1> policy(configuration);
	TcpOptions options;
	options.port = 0;
	options.http_memory.enabled = true;
	options.http_memory.initial_requests = 2;
	options.http_memory.reserved_requests = 1;
	options.buffer_memory.max_bytes = 65536;
	TcpHttpBackend backend(transport, options);
	Handler handler;
	AdmissionMiddleware middleware(AdmissionRef(policy), now);
	auto chain = makeHeaderChain(middleware);
	auto small = [&handler](Context &context)
		{
			++handler.small_called;
			char body[] = "before";
			CHECK(context.response.header("X-Captured", "original") == SendResult::Ok);
			const auto result = context.response.text(200, body);
			body[0] = 'X';
			return responseResult(result);
		};
	Route small_route("/small", HttpMethod::Get, EndpointRef(small));
	HandlerBase *const routes[]{&handler, &small_route};
	HttpServer server(backend, routes, {}, {}, {}, chain);
	CHECK(server.begin() == HttpError::Ok);
	const auto initial = backend.bufferMemoryStats();
	{
		auto bulk = connectTo(backend.port());
		send(bulk, "GET /bulk HTTP/1.1\r\nHost: localhost\r\nX-Client-Key: guest\r\n\r\n");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while(!handler.bulk_started)
		{
			CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield();
		}
		const auto started = std::chrono::steady_clock::now();
		for(unsigned index = 0; index < 20; ++index)
			CHECK(request(backend.port(), "extra").find("HTTP/1.1 503") == 0);
		const auto primary = request(backend.port(), "primary");
		CHECK(primary.find("HTTP/1.1 200") == 0 && primary.find("X-Captured: original") != primary.npos);
		CHECK(primary.substr(primary.find("\r\n\r\n") + 4) == "before");
		CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
		CHECK(handler.small_called == 1);
		const auto after = backend.bufferMemoryStats();
		CHECK(after.used_bytes == initial.used_bytes && after.peak_bytes == initial.peak_bytes);
	}
	const auto released_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	for(;;)
	{
		const auto result = request(backend.port(), "guest");
		if(result.find("HTTP/1.1 200") == 0) break;

		CHECK(result.find("HTTP/1.1 503") == 0 && std::chrono::steady_clock::now() < released_by);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(request(backend.port(), "guest").find("HTTP/1.1 200") == 0);
	CHECK(request(backend.port(), "guest").find("HTTP/1.1 429") == 0);
	CHECK(request(backend.port(), "primary").find("HTTP/1.1 200") == 0);
	CHECK(server.end() == HttpError::Ok);
	CHECK(backend.bufferMemoryStats().used_bytes == 0);
	CHECK(transport.ownedHandles() == 0);
	boundedIngress(transport);
	CHECK(transport.stop().status == tcp::Status::Ok);
	CHECK(sockets::cleanup() == 0);
	std::puts("TCP_QOS_PASS");
}
