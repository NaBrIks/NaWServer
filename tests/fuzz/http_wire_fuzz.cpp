// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "http/wire/request_parser.h"
#include "http/wire/response_writer.h"
#include "websocket/wire/handshake.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{

	using namespace NaWServer;
	using namespace NaWServer::http::wire;

	void require(bool value)
	{
		if(!value) std::abort();
	}

	struct Workspace
	{
		std::uint64_t before = 0x185a3cfd74e0296bULL;
		std::array<char, 8192> bytes{};
		std::array<HeaderEntry, 32> entries{};
		std::uint64_t after = 0x62ab8c71e3054fd9ULL;
		void check() const
		{
			require(before == 0x185a3cfd74e0296bULL && after == 0x62ab8c71e3054fd9ULL);
		}

	};

	void parseInput(const std::uint8_t *data, std::size_t size)
	{
		Workspace whole, partial;
		HttpLimits limits;
		RequestParser first(whole.bytes.data(), whole.bytes.size(), whole.entries.data(), whole.entries.size(), limits);
		RequestParser second(partial.bytes.data(), partial.bytes.size(), partial.entries.data(), partial.entries.size(),
		    limits);
		size = std::min(size, whole.bytes.size());
		if(size) std::memcpy(whole.bytes.data(), data, size);

		const auto expected = first.advance(size);
		auto actual = second.advance(0);
		std::size_t copied = 0;
		const std::size_t chunk = size ? 1 + data[size - 1] % 127 : 1;
		while(copied < size && actual.state == ParseStatus::NeedMore)
		{
			const auto amount = std::min(chunk, size - copied);
			std::memcpy(partial.bytes.data() + copied, data + copied, amount);
			copied += amount;
			actual = second.advance(copied);
		}
		require(expected.state == actual.state && expected.status == actual.status &&
		    expected.head_bytes == actual.head_bytes);
		require(expected.state != ParseStatus::InvalidState);
		whole.check();
		partial.check();
		if(expected.state != ParseStatus::Complete) return;

		require(first.head().target == second.head().target && first.head().method == second.head().method);
		require(first.head().content_length == second.head().content_length && first.http11() == second.http11());
		require(first.headerCount() == second.headerCount() && first.rawHead() == second.rawHead());
		require(first.rawHead().size() == expected.head_bytes && expected.head_bytes <= size);
		const int first_upgrade = websocket::wire::validateHandshake(first);
		require(first_upgrade == websocket::wire::validateHandshake(second));
		if(const auto key = first.headerValue("Sec-WebSocket-Key"))
		{
			std::array<char, 31> accept;
			accept.fill(static_cast<char>(0xa5));
			const bool valid = websocket::wire::makeWebSocketAccept(*key, accept.data() + 1, 29);
			require(static_cast<unsigned char>(accept.front()) == 0xa5 &&
			    static_cast<unsigned char>(accept.back()) == 0xa5);
			if(valid) require(accept[29] == '\0');
		}

		std::array<char, 65> name{};
		if(size) std::memcpy(name.data(), data, std::min(size, name.size() - 1));

		std::array<char, 258> destination;
		destination.fill(static_cast<char>(0xa5));
		const std::size_t capacity = size ? data[0] : 0;
		const auto read = first.readHeader(name.data(), destination.data() + 1, capacity);
		require(static_cast<unsigned char>(destination.front()) == 0xa5 &&
		    static_cast<unsigned char>(destination.back()) == 0xa5);
		require(static_cast<unsigned char>(destination[capacity + 1]) == 0xa5);
		if(read.status == HeaderReadStatus::Ok) require(read.size < capacity && destination[read.size + 1] == '\0');
	}

	struct Output
	{
		std::uint64_t hash = 1469598103934665603ULL;
		std::size_t size = 0;
	};

	Output emit(ResponseWriter &writer, std::size_t maximum)
	{
		Output result;
		for(std::size_t steps = 0; !writer.complete(); ++steps)
		{
			require(steps < 8192);
			const auto span = writer.current();
			require(span.data() && !span.empty());
			require(!writer.advance(0) && !writer.advance(span.size() + 1));
			require(writer.current().data() == span.data() && writer.current().size() == span.size());
			const auto amount = std::min(maximum, span.size());
			for(std::size_t index = 0; index < amount; ++index)
				result.hash = (result.hash ^ span.data()[index]) * 1099511628211ULL;
			result.size += amount;
			require(result.size < 8192 && writer.advance(amount));
		}
		require(writer.current().empty());
		return result;
	}

	void writerInput(const std::uint8_t *data, std::size_t size)
	{
		std::array<char, 65> text{};
		if(size) std::memcpy(text.data(), data, std::min(size, text.size() - 1));

		const unsigned flags = size ? data[0] : 0;
		const int statuses[]{200, 201, 204, 304, 101, 599, 0, -1};
		const int status = statuses[size > 1 ? data[1] % 8 : 0];
		const char *names[]{"X-Fuzz", "Content-Length", "Transfer-Encoding", "Connection", text.data()};
		const char *name = names[(flags >> 1) % 5];
		const char *value = flags & 4 ? "bounded" : text.data();
		const char *type = flags & 1 ? "application/octet-stream" : text.data();
		const Header headers[]{{name, value}, {name, value}};
		const auto count = size > 2 ? data[2] % 3 : 0;
		const auto method = flags & 8 ? HttpMethod::Head : HttpMethod::Get;
		const auto body_size = (status == 204 || status == 304) && (flags & 16) ? 0 : std::min(size, std::size_t{4096});
		const ByteView body(flags & 128 ? nullptr : data, body_size);
		ResponseWriter first, second;
		const auto expected = first.prepare(method, status, type, headers, count, body);
		require(expected == second.prepare(method, status, type, headers, count, body));
		if(expected != SendResult::Ok)
		{
			require(first.complete() && first.current().empty());
			return;
		}

		const auto single = emit(first, 1);
		const auto partial = emit(second, 127);
		require(single.size == partial.size && single.hash == partial.hash);
	}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
	parseInput(data, size);
	writerInput(data, size);
	return 0;
}
