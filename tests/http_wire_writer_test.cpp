// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "http/wire/response_writer.h"
#include "support/tcp_scripted_transport.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <type_traits>

namespace
{

	std::size_t allocations = 0;
	unsigned checks = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("HTTP_WIRE_WRITER_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

} // namespace

void *operator new(std::size_t size)
{
	++allocations;
	if(auto *bytes = std::malloc(size ? size : 1)) return bytes;

	std::abort();
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void *bytes) noexcept
{
	std::free(bytes);
}

void operator delete[](void *bytes) noexcept
{
	std::free(bytes);
}

void operator delete(void *bytes, std::size_t) noexcept
{
	std::free(bytes);
}

void operator delete[](void *bytes, std::size_t) noexcept
{
	std::free(bytes);
}

namespace
{

	using namespace NaWServer;
	using namespace NaWServer::http::wire;
	using namespace NaWServer::tcp;
	using namespace NaWServer::tcp::test;

#define CHECK(value) check((value), __LINE__)

	std::string drain(ResponseWriter &writer, std::size_t maximum_chunk)
	{
		std::string result;
		for(std::size_t attempt = 0; attempt < 20000 && !writer.complete(); ++attempt)
		{
			const auto span = writer.current();
			CHECK(!span.empty());
			const auto size = std::min(span.size(), maximum_chunk);
			result.append(reinterpret_cast<const char *>(span.data()), size);
			CHECK(writer.advance(size));
		}
		CHECK(writer.complete() && writer.current().empty());
		return result;
	}

	void wireSemantics()
	{
		const std::uint8_t binary[]{0, 255, 1, 0};
		const Header headers[]{{"X-Empty", ""}, {"X-Value", "one\ttwo"}, {"Connection", "close"}};
		std::string expected = "HTTP/1.1 201 Response\r\nConnection: close\r\nContent-Length: 4\r\n"
		    "Content-Type: application/octet-stream\r\nX-Empty: \r\nX-Value: one\ttwo\r\n\r\n";
		expected.append(reinterpret_cast<const char *>(binary), sizeof(binary));
		for(const auto maximum_chunk : {1u, 2u, 3u, 7u, 127u})
		{
			ResponseWriter writer;
			CHECK(writer.complete() && writer.current().empty());
			CHECK(writer.prepare(HttpMethod::Post, 201, "application/octet-stream", headers, 3,
			    {binary, sizeof(binary)}) == SendResult::Ok);
			CHECK(drain(writer, maximum_chunk) == expected);
			CHECK(!writer.advance(1));
		}
		for(const auto status : {204, 304})
		{
			ResponseWriter writer;
			CHECK(writer.prepare(HttpMethod::Get, status, "text/plain", nullptr, 0, {}) == SendResult::Ok);
			const auto wire = drain(writer, 3);
			CHECK(wire == "HTTP/1.1 " + std::to_string(status) +
			    " Response\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n");
			CHECK(wire.find("Content-Length:") == std::string::npos);
		}
		ResponseWriter head;
		CHECK(head.prepare(HttpMethod::Head, 200, "text/plain", nullptr, 0,
		    {binary, sizeof(binary)}) == SendResult::Ok);
		CHECK(drain(head, 1) == "HTTP/1.1 200 Response\r\nConnection: close\r\nContent-Length: 4\r\n"
		    "Content-Type: text/plain\r\n\r\n");
		CHECK(head.prepare(HttpMethod::Get, 599, "text/plain", nullptr, 0, {}) == SendResult::Ok);
		CHECK(drain(head, 2) == "HTTP/1.1 599 Response\r\nConnection: close\r\nContent-Length: 0\r\n"
		    "Content-Type: text/plain\r\n\r\n");
	}

	void validateBeforeOutput()
	{
		ResponseWriter writer;
		const std::uint8_t byte = 42;
		for(const auto status : {-1, 0, 101, 199, 600})
		{
			CHECK(writer.prepare(HttpMethod::Get, status, "text/plain", nullptr, 0, {}) == SendResult::InvalidArgument);
			CHECK(writer.complete() && writer.current().empty());
		}
		for(const char *type : {static_cast<const char *>(nullptr), "", "text/plain\r\nX-Bad: yes", "text/\x7f"})
		{
			CHECK(writer.prepare(HttpMethod::Get, 200, type, nullptr, 0, {}) == SendResult::InvalidArgument);
			CHECK(writer.complete() && writer.current().empty());
		}
		CHECK(writer.prepare(HttpMethod::Get, 200, "text/plain", nullptr, 0, {nullptr, 1}) ==
		    SendResult::InvalidArgument);
		CHECK(writer.prepare(HttpMethod::Get, 200, "text/plain", nullptr, 1, {}) == SendResult::InvalidArgument);
		for(const auto status : {204, 304})
		{
			CHECK(writer.prepare(HttpMethod::Get, status, "text/plain", nullptr, 0, {&byte, 1}) ==
			    SendResult::InvalidArgument);
		}
		const auto too_large = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) + 1;
		CHECK(writer.prepare(HttpMethod::Head, 200, "text/plain", nullptr, 0, {&byte, too_large}) ==
		    SendResult::InvalidArgument);
		const Header invalid[]{{nullptr, "x"}, {"X-Test", nullptr}, {"", "x"}, {"X Bad", "x"},
			{"X-Test", "a\rb"}, {"X-Test", "a\nb"}, {"X-Test", "a\x01"}, {"X-Test", "a\x7f"},
			{"Content-Length", "4"}, {"content-type", "text/plain"}, {"TRANSFER-ENCODING", "chunked"},
			{"Connection", "keep-alive"}, {"Connection", " close "}};
		for(const auto header : invalid)
		{
			const Header table[]{{"X-Valid", "this must never be exposed"}, header};
			CHECK(writer.prepare(HttpMethod::Get, 200, "text/plain", table, 2, {}) == SendResult::InvalidArgument);
			CHECK(writer.complete() && writer.current().empty());
		}
		const Header duplicate[]{{"X-Test", "one"}, {"x-test", "two"}};
		CHECK(writer.prepare(HttpMethod::Get, 200, "text/plain", duplicate, 2, {}) == SendResult::InvalidArgument);
		const Header connections[]{{"Connection", "close"}, {"connection", "close"}};
		CHECK(writer.prepare(HttpMethod::Get, 200, "text/plain", connections, 2, {}) == SendResult::InvalidArgument);
		const Header single{"X-Test", "value"};
		CHECK(writer.prepare(HttpMethod::Get, 200, "text/plain", &single, Response::max_headers + 1, {}) ==
		    SendResult::HeaderLimit);
		CHECK(writer.complete() && writer.current().empty());
	}

	void borrowedSpansAndLifecycle()
	{
		static_assert(!std::is_copy_constructible_v<ResponseWriter> && !std::is_move_constructible_v<ResponseWriter>);
		ResponseWriter writer;
		const char type[] = "application/custom";
		const char name[] = "X-Borrowed";
		const char value[] = "static metadata";
		const std::uint8_t payload[]{0, 1, 255, 2};
		const Header headers[]{{name, value}};
		CHECK(writer.prepare(HttpMethod::Get, 200, type, headers, 1, {payload, sizeof(payload)}) == SendResult::Ok);
		const auto first = writer.current();
		CHECK(writer.prepare(HttpMethod::Get, 500, "must not replace active prefix", nullptr, 0, {}) ==
		    SendResult::AlreadySent);
		CHECK(writer.current().data() == first.data() && writer.current().size() == first.size());
		CHECK(!writer.advance(0) && !writer.advance(first.size() + 1));
		CHECK(writer.current().data() == first.data() && writer.current().size() == first.size());
		bool saw_type = false, saw_name = false, saw_value = false, saw_body = false;
		for(unsigned attempt = 0; attempt < 30 && !writer.complete(); ++attempt)
		{
			const auto current = writer.current();
			saw_type |= current.data() == reinterpret_cast<const std::uint8_t *>(type);
			saw_name |= current.data() == reinterpret_cast<const std::uint8_t *>(name);
			saw_value |= current.data() == reinterpret_cast<const std::uint8_t *>(value);
			if(current.data() == payload)
			{
				saw_body = true;
				CHECK(current.size() == sizeof(payload) && writer.advance(1));
				CHECK(writer.current().data() == payload + 1 && writer.current().size() == sizeof(payload) - 1);
				CHECK(writer.advance(sizeof(payload) - 1));
			}
			else
			{
				CHECK(writer.advance(current.size()));
			}
		}
		CHECK(writer.complete() && saw_type && saw_name && saw_value && saw_body);
		CHECK(writer.prepare(HttpMethod::Get, 200, type, nullptr, 0, {}) == SendResult::Ok);
		writer.reset();
		CHECK(writer.complete() && writer.current().empty() && !writer.advance(1));
		CHECK(writer.prepare(HttpMethod::Get, 200, type, nullptr, 0, {}) == SendResult::Ok);
		(void)drain(writer, 7);
	}

	void lengthAndStorageLimits()
	{
		ResponseWriter writer;
		const std::uint8_t byte = 1;
		const auto maximum_length = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
		CHECK(writer.prepare(HttpMethod::Head, 200, "text/plain", nullptr, 0, {&byte, maximum_length}) ==
		    SendResult::Ok);
		const auto wire = drain(writer, 7);
		CHECK(wire.find("Content-Length: " + std::to_string(maximum_length) + "\r\n") != std::string::npos);
		std::array<char, 4097> large_static_value;
		large_static_value.fill('v');
		large_static_value.back() = 0;
		const Header header{"X-Large-Default", large_static_value.data()};
		CHECK(writer.prepare(HttpMethod::Get, 200, "text/plain", &header, 1, {}) == SendResult::Ok);
		const auto large = drain(writer, 7);
		CHECK(large.find(std::string("X-Large-Default: ") + large_static_value.data() + "\r\n") != std::string::npos);
	}

	SendResult pumpWriter(ResponseWriter &writer, ScriptedTransport &transport, Handle connection)
	{
		// A finite test driver, not the future engine's deadline/stop implementation.
		for(unsigned attempt = 0; attempt < 20000 && !writer.complete(); ++attempt)
		{
			const auto span = writer.current();
			const auto result = transport.writeSome(connection, span.data(), span.size());
			if(result.status == IoStatus::Progress)
			{
				CHECK(result.size > 0 && result.size <= span.size());
				CHECK(writer.advance(result.size));
			}
			else if(result.status == IoStatus::WouldBlock)
			{
				CHECK(writer.current().data() == span.data() && writer.current().size() == span.size());
				const Interest interest{connection, false, true};
				Ready ready;
				if(transport.wait(&interest, &ready, 1, 20).status != WaitStatus::Ready)
					return SendResult::TransportError;
			}
			else if(result.status == IoStatus::Interrupted)
			{
				CHECK(writer.current().data() == span.data() && writer.current().size() == span.size());
			}
			else
			{
				return SendResult::TransportError;
			}
		}
		return writer.complete() ? SendResult::Ok : SendResult::TransportError;
	}

	void scriptedPartialOutput()
	{
		const std::array<std::uint8_t, 4> payload{0, 255, 1, 0};
		std::string expected = "HTTP/1.1 200 Response\r\nConnection: close\r\nContent-Length: 4\r\n"
		    "Content-Type: application/octet-stream\r\n\r\n";
		expected.append(reinterpret_cast<const char *>(payload.data()), payload.size());
		for(const auto final_status : {IoStatus::Progress, IoStatus::Fatal, IoStatus::PeerClosed})
		{
			ScriptedTransport transport;
			Step listener;
			listener.operation = Operation::OpenListener;
			listener.handle = {1, true};
			listener.port = 12345;
			CHECK(transport.append(listener));
			const auto opened = transport.openListener("127.0.0.1", 0, 3);
			Step accept;
			accept.operation = Operation::Accept;
			accept.handle = {0, true};
			CHECK(transport.append(accept));
			auto connection = transport.accept(opened.handle).handle;
			Step write;
			write.operation = Operation::Write;
			write.expected_handle = connection;
			write.bytes = reinterpret_cast<const std::uint8_t *>(expected.data());
			write.size = 7;
			write.maximum_chunk = 3;
			CHECK(transport.append(write));
			Step blocked;
			blocked.operation = Operation::Write;
			blocked.io_status = IoStatus::WouldBlock;
			CHECK(transport.append(blocked));
			Step wait;
			wait.operation = Operation::Wait;
			wait.wait_status = WaitStatus::Ready;
			wait.readiness[0].writable = true;
			wait.elapsed_ms = 1;
			CHECK(transport.append(wait));
			blocked.io_status = IoStatus::Interrupted;
			CHECK(transport.append(blocked));
			write.bytes += 7;
			write.size = expected.size() - 7;
			write.maximum_chunk = 2;
			write.io_status = final_status;
			CHECK(transport.append(write));
			ResponseWriter writer;
			CHECK(writer.prepare(HttpMethod::Get, 200, "application/octet-stream", nullptr, 0,
			    {payload.data(), payload.size()}) == SendResult::Ok);
			const auto result = pumpWriter(writer, transport, connection);
			if(final_status == IoStatus::Progress)
			{
				CHECK(result == SendResult::Ok && writer.complete());
				CHECK(transport.capturedSize() == expected.size());
				CHECK(std::memcmp(transport.capturedBytes(), expected.data(), expected.size()) == 0);
			}
			else
			{
				CHECK(result == SendResult::TransportError && !writer.complete());
				CHECK(transport.capturedSize() == 7 && std::memcmp(transport.capturedBytes(), expected.data(), 7) == 0);
				CHECK(writer.prepare(HttpMethod::Get, 500, "text/plain", nullptr, 0, {}) == SendResult::AlreadySent);
				writer.reset();
			}

			CHECK(transport.failures() == 0 && transport.remaining() == 0 && transport.milliseconds() == 1);
			Step close;
			close.operation = Operation::Close;
			CHECK(transport.append(close) && transport.append(close));
			CHECK(transport.close(connection).released);
			auto listener_handle = opened.handle;
			CHECK(transport.close(listener_handle).released && transport.owned() == 0);
		}
	}

	void allocationLedger()
	{
		ResponseWriter writer;
		const Header headers[]{{"X-Static", "value"}, {"Connection", "close"}};
		std::array<std::uint8_t, 4096> payload{};
		const auto before = allocations;
		std::size_t accepted = 0;
		for(unsigned response = 0; response < 100; ++response)
		{
			CHECK(writer.prepare(HttpMethod::Post, 200, "application/octet-stream", headers, 2,
			    {payload.data(), payload.size()}) == SendResult::Ok);
			for(unsigned attempt = 0; attempt < 1000 && !writer.complete(); ++attempt)
			{
				const auto span = writer.current();
				const auto size = std::min(span.size(), std::size_t{7});
				accepted += size;
				CHECK(writer.advance(size));
			}
			CHECK(writer.complete());
		}
		CHECK(accepted > 100 * payload.size() && allocations == before);
	}

} // namespace

int main()
{
	wireSemantics();
	validateBeforeOutput();
	borrowedSpansAndLifecycle();
	lengthAndStorageLimits();
	scriptedPartialOutput();
	allocationLedger();
	std::printf("HTTP_WIRE_WRITER checks=%u writer_bytes=%zu writer_allocations=0 PASS\n",
	    checks, sizeof(NaWServer::http::wire::ResponseWriter));
}
