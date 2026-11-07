// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
	using namespace NaWServer;
	namespace sockets = NaWServer::test::host;
	unsigned checks = 0;
	void check(bool value, int line)
	{
		++checks;
		if(!value) { std::printf("RESPONSE_STREAM_FAIL line=%d\n", line); std::fflush(stdout); std::_Exit(1); }
	}
#define CHECK(value) check((value), __LINE__)
	struct Source
	{
		std::size_t length = 0;
		bool short_read = false;
		std::atomic<unsigned> reads{0};
		static BodyReadResult read(void *context, std::size_t offset, std::uint8_t *bytes, std::size_t capacity)
		{
			auto &source = *static_cast<Source *>(context);
			++source.reads;
			if(source.short_read && offset >= 1024) return {BodyReadStatus::End, 0};

			const auto size = std::min({capacity, source.length - offset,
			    source.short_read ? 1024 - offset : std::size_t{1024}});
			for(std::size_t index = 0; index < size; ++index) bytes[index] = (offset + index) % 251;

			return {BodyReadStatus::Data, size};
		}
	};

	std::string request(std::uint16_t port, const char *method, const char *path)
	{
		const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(socket != sockets::invalid && sockets::setTimeouts(socket, 5000));
		sockaddr_in address{};
		address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
		CHECK(connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
		const std::string head = std::string(method) + " " + path + " HTTP/1.1\r\nHost: test\r\n\r\n";
		CHECK(sockets::sendSome(socket, head.data(), head.size()) == static_cast<int>(head.size()));
		std::string output;
		char block[4096];
		for(;;)
		{
			const auto read = sockets::receiveSome(socket, block, sizeof(block));
			CHECK(read >= 0);
			if(!read) break;

			output.append(block, static_cast<std::size_t>(read));
		}
		CHECK(sockets::closeSocket(socket) == 0);
		return output;
	}

	void run(std::size_t length, bool managed = false)
	{
		sockets::TcpTransport transport;
		CHECK(transport.start().status == tcp::Status::Ok);
		TcpOptions options; options.port = 0;
		options.transmission.enabled = true;
		options.transmission.maximum_bytes = 8 * 1024;
		options.http_memory.enabled = managed;
		TcpHttpBackend backend(transport, options);
		Source source; source.length = length;
		auto serve = [&](Response &response)
		{
			return response.sendSource(200, "application/octet-stream", {&source, length, Source::read});
		};
		auto generate = [&](Response &response)
		{
			if(response.beginStream(200, "application/octet-stream", length) != SendResult::Ok)
				return SendResult::TransportError;

			std::uint8_t block[1024];
			for(std::size_t offset = 0; offset < length;)
			{
				const auto size = std::min(sizeof(block), length - offset);
				for(std::size_t index = 0; index < size; ++index) block[index] = (offset + index) % 251;

				const auto result = response.write({block, size});
				if(result != SendResult::Ok) return result;

				offset += size;
			}
			return response.finish();
		};
		auto direct = [&](Response &response)
		{
			if(response.beginStream(200, "application/octet-stream", length) != SendResult::Ok)
				return SendResult::TransportError;

			for(std::size_t offset = 0; offset < length;)
			{
				auto block = response.acquire(length - offset);
				if(!block) return SendResult::TransportError;

				const auto size = block.capacity();
				for(std::size_t index = 0; index < size; ++index) block.data()[index] = (offset + index) % 251;

				if(!block.commit(size)) return SendResult::TransportError;

				offset += size;
			}
			return response.finish();
		};
		Route get("/file", HttpMethod::Get, EndpointRef(serve));
		Route head("/file", HttpMethod::Head, EndpointRef(serve));
		Route writer("/writer", HttpMethod::Get, EndpointRef(generate));
		Route lease("/direct", HttpMethod::Get, EndpointRef(direct));
		HandlerBase *const routes[]{&get, &head, &writer, &lease};
		HttpServer server(backend, routes);
		CHECK(server.begin() == HttpError::Ok);
		for(const char *path : {"/file", "/writer", "/direct"})
		{
			const auto output = request(backend.port(), "GET", path);
			const auto split = output.find("\r\n\r\n");
			CHECK(split != output.npos && output.size() - split - 4 == length);
			CHECK(output.find("Content-Length: " + std::to_string(length) + "\r\n") != output.npos);
			for(std::size_t index = 0; index < length; ++index)
				CHECK(static_cast<unsigned char>(output[split + 4 + index]) == index % 251);
		}
		const auto reads = source.reads.load();
		const auto output = request(backend.port(), "HEAD", "/file");
		CHECK(output.size() == output.find("\r\n\r\n") + 4 && source.reads.load() == reads);
		source.short_read = true;
		const auto truncated = request(backend.port(), "GET", "/file");
		CHECK(truncated.size() - truncated.find("\r\n\r\n") - 4 == 1024);
		CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);
		CHECK(transport.stop().status == tcp::Status::Ok);
	}
}

int main()
{
	CHECK(sockets::initialize() == 0);
	run(5 * 1024);
	run(160 * 1024);
#if NAWSERVER_TCP_MANAGED
	run(160 * 1024, true);
#endif
	CHECK(sockets::cleanup() == 0);
	std::printf("RESPONSE_STREAM_PASS checks=%u\n", checks);
}
