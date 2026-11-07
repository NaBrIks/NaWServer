// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include "support/host_sockets.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <NaWServer/request.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
	using namespace NaWServer;
	namespace sockets = NaWServer::test::host;
	void check(bool value, int line)
	{
		if(!value) { std::printf("STREAMING_LIFECYCLE_FAIL %d native=%d\n", line, sockets::lastError()); std::fflush(stdout); std::_Exit(1); }
	}
#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	template <class F> void until(F predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while(!predicate())
		{
			CHECK(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	sockets::Handle connectTo(std::uint16_t port)
	{
		const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(socket != sockets::invalid && sockets::setTimeouts(socket, 3000));
		sockaddr_in address{};
		address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
		CHECK(connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
		return socket;
	}
	void send(sockets::Handle socket, std::string_view bytes)
	{
		while(!bytes.empty())
		{
			const auto size = sockets::sendSome(socket, bytes.data(), bytes.size());
			CHECK(size > 0); bytes.remove_prefix(static_cast<std::size_t>(size));
		}
	}
	std::string receive(sockets::Handle socket)
	{
		std::string output; char byte;
		while(output.find("\r\n\r\n") == output.npos)
		{
			CHECK(sockets::receiveSome(socket, &byte, 1) == 1 && output.size() < 4096); output.push_back(byte);
		}
		const auto field = output.find("Content-Length: "); CHECK(field != output.npos);
		const auto length = static_cast<std::size_t>(std::strtoul(output.c_str() + field + 16, nullptr, 10));
		CHECK(length <= 1024 * 1024);
		const auto head = output.size();
		char buffer[4096];
		while(output.size() - head < length)
		{
			const auto size = sockets::receiveSome(socket, buffer, std::min(sizeof(buffer), length - (output.size() - head)));
			CHECK(size > 0); output.append(buffer, static_cast<std::size_t>(size));
		}
		return output;
	}
	struct Sink
	{
		std::atomic<bool> paused{true};
		std::atomic<unsigned> began{0}, finished{0}, canceled{0}, released{0};
		std::atomic<std::size_t> bytes{0}, largest{0};
		static BodyReceiver begin(void *context, HttpExchange &)
		{
			auto &self = *static_cast<Sink *>(context); ++self.began; self.bytes = 0;
			return {context, [](void *raw, ByteView part)
				{
					auto &sink = *static_cast<Sink *>(raw);
					if(sink.paused) return BodyConsumeResult::Pause;
					const auto offset = sink.bytes.load();
					for(std::size_t i = 0; i < part.size(); ++i) CHECK(part.data()[i] == (offset + i) % 251);
					sink.bytes += part.size(); sink.largest = std::max(sink.largest.load(), part.size());
					return BodyConsumeResult::Accepted;
				}, [](void *raw) { ++static_cast<Sink *>(raw)->finished; return true; },
				[](void *raw) { ++static_cast<Sink *>(raw)->canceled; },
				[](void *raw) { ++static_cast<Sink *>(raw)->released; }};
		}
	};
	struct Source
	{
		std::atomic<unsigned> reads{0}, released{0};
		static BodyReadResult read(void *raw, std::size_t offset, std::uint8_t *bytes, std::size_t capacity)
		{
			++static_cast<Source *>(raw)->reads;
			for(std::size_t i = 0; i < capacity; ++i) bytes[i] = static_cast<std::uint8_t>((offset + i) % 251);
			return {BodyReadStatus::Data, capacity};
		}
		static void release(void *raw) { ++static_cast<Source *>(raw)->released; }
	};
}

int main()
{
	CHECK(sockets::initialize() == 0);
	sockets::TcpTransport transport; CHECK(transport.start().status == tcp::Status::Ok);
	TcpOptions options; options.port = 0; options.http_memory.enabled = true;
	options.keep_alive.enabled = true; options.wait_ceiling_ms = 2;
	options.no_progress_timeout_ms = 3000;
	options.transmission.enabled = true; options.transmission.maximum_bytes = 8192;
	HttpLimits limits; limits.max_body_size = 256; limits.exchange_timeout_ms = 10000;
	TcpHttpBackend backend(transport, options);
	Sink sink; Source source;
	std::atomic<unsigned> endpoints{0};
	auto uploaded = [&](Request &request, Response &response)
	{
		CHECK(request.body().empty()); ++endpoints;
		return response.text(200, "uploaded");
	};
	auto quick = [](Response &response) { return response.text(200, "quick"); };
	auto download = [&](Response &response)
	{
		return response.sendOwnedSource(200, "application/octet-stream",
		    OwnedResponseSource{{&source, 512 * 1024, Source::read}, Source::release});
	};
	auto drop = [](HeaderContext &, HeaderNext &) { return Result::Drop; };
	auto drop_chain = makeHeaderChain(drop);
	Route upload("/upload", HttpMethod::Post, EndpointRef(uploaded), {}, {}, {}, {},
	    {BodyMode::Stream, 1024 * 1024, 128, &sink, Sink::begin});
	Route health("/quick", HttpMethod::Get, EndpointRef(quick));
	Route file("/file", HttpMethod::Get, EndpointRef(download));
	Route rejected("/drop", HttpMethod::Post, EndpointRef(uploaded), {}, {}, {}, drop_chain);
	HandlerBase *routes[]{&upload, &health, &file, &rejected}; HttpServer server(backend, routes, limits);
	CHECK(server.begin() == HttpError::Ok);
	const auto port = backend.port();
	auto active = connectTo(port);
	constexpr std::size_t total = 512 * 1024;
	send(active, "POST /upload HTTP/1.1\r\nHost: test\r\nContent-Length: " + std::to_string(total) + "\r\n\r\n");
	until([&] { return sink.began.load() == 1; });
	std::string block(128, '\0');
	for(std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<char>(i % 251);
	send(active, block);
	auto sibling = connectTo(port); send(sibling, "GET /quick HTTP/1.1\r\nHost: test\r\n\r\n");
	CHECK(receive(sibling).find("quick") != std::string::npos);
	CHECK(sink.finished == 0); sink.paused = false;
	for(std::size_t offset = block.size(); offset < total; offset += block.size())
	{
		for(std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<char>((offset + i) % 251);
		send(active, block);
	}
	CHECK(receive(active).find("uploaded") != std::string::npos);
	CHECK(sink.bytes == total && sink.largest <= 128 && sink.finished == 1 && sink.canceled == 0 && sink.released == 1);
	CHECK(backend.managedMemoryStats().bodies.peak_bytes < total);
	send(active, "GET /quick HTTP/1.1\r\nHost: test\r\n\r\n");
	CHECK(receive(active).find("quick") != std::string::npos);
	CHECK(sockets::closeSocket(active) == 0);
	active = connectTo(port);
	send(active, "POST /drop HTTP/1.1\r\nHost: test\r\nContent-Length: 100\r\n\r\n");
	char byte; CHECK(sockets::receiveSome(active, &byte, 1) <= 0);
	CHECK(endpoints == 1 && sink.began == 1); sockets::closeSocket(active);
	active = connectTo(port);
	send(active, "POST /upload HTTP/1.1\r\nHost: test\r\nContent-Length: 10000\r\n\r\n");
	until([&] { return sink.began.load() == 2; }); sockets::closeSocket(active);
	until([&] { return sink.canceled.load() == 1; }); CHECK(sink.released == 2 && endpoints == 1);
	active = connectTo(port);
	send(active, "GET /file HTTP/1.1\r\nHost: test\r\n\r\n");
	until([&] { return source.reads.load() > 0; });
	send(sibling, "GET /quick HTTP/1.1\r\nHost: test\r\n\r\n");
	CHECK(receive(sibling).find("quick") != std::string::npos);
	const auto output = receive(active); const auto split = output.find("\r\n\r\n") + 4;
	CHECK(output.size() - split == total);
	for(std::size_t i = 0; i < total; ++i) CHECK(static_cast<unsigned char>(output[split + i]) == i % 251);
	until([&] { return source.released.load() == 1; });
	sockets::closeSocket(active); sockets::closeSocket(sibling);
	CHECK(server.end() == HttpError::Ok); CHECK(transport.stop().status == tcp::Status::Ok);
	CHECK(sockets::cleanup() == 0);
	std::puts("STREAMING_LIFECYCLE_PASS upload=524288 block=128 download=524288 pause/quick/keepalive/drop/cancel");
}
