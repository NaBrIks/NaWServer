// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "http/wire/request_parser.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace
{

	bool count_allocations = false;
	std::size_t allocations = 0;

} // namespace

void *operator new(std::size_t size)
{
	if(count_allocations)
		++allocations;

	if(void *storage = std::malloc(size ? size : 1))
		return storage;

	throw std::bad_alloc();
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void *storage) noexcept
{
	std::free(storage);
}

void operator delete[](void *storage) noexcept
{
	std::free(storage);
}

void operator delete(void *storage, std::size_t) noexcept
{
	std::free(storage);
}

void operator delete[](void *storage, std::size_t) noexcept
{
	std::free(storage);
}

namespace
{

	using namespace NaWServer;
	using namespace NaWServer::http::wire;
	unsigned checks = 0;
	unsigned split_runs = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("HTTP_WIRE_PARSER_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	struct Fixture
	{
		std::array<char, 16384> storage{};
		std::array<HeaderEntry, 32> headers{};
		HttpLimits limits;

		void copy(std::string_view bytes)
		{
			CHECK(bytes.size() < storage.size());
			std::memcpy(storage.data(), bytes.data(), bytes.size());
			storage[bytes.size()] = static_cast<char>(0xa5);
		}

	};

	void checkResult(ParseResult result, int expected)
	{
		CHECK(result.state == (expected ? ParseStatus::Reject : ParseStatus::Complete));
		CHECK(result.status == expected);
	}

	void everySplit(std::string_view wire, int expected, HttpLimits limits = {})
	{
		for(std::size_t split = 0; split <= wire.size(); ++split)
		{
			Fixture fixture;
			fixture.limits = limits;
			fixture.copy(wire);
			RequestParser parser(fixture.storage.data(), fixture.storage.size(),
			    fixture.headers.data(), fixture.headers.size(), fixture.limits);
			(void)parser.advance(split);
			const ParseResult result = parser.advance(wire.size());
			checkResult(result, expected);
			CHECK(std::memcmp(fixture.storage.data(), wire.data(), wire.size()) == 0);
			CHECK(static_cast<unsigned char>(fixture.storage[wire.size()]) == 0xa5);
			if(!expected)
			{
				CHECK(result.head_bytes == wire.find("\r\n\r\n") + 4);
				CHECK(parser.rawHead().data() == fixture.storage.data());
				CHECK(parser.rawHead() == wire.substr(0, wire.find("\r\n\r\n") + 4));
			}
			else
				CHECK(parser.rawHead().empty());

			++split_runs;
		}

		Fixture fixture;
		fixture.limits = limits;
		fixture.copy(wire);
		RequestParser parser(fixture.storage.data(), fixture.storage.size(),
		    fixture.headers.data(), fixture.headers.size(), fixture.limits);
		ParseResult result;
		for(std::size_t used = 0; used <= wire.size(); ++used)
			result = parser.advance(used);

		checkResult(result, expected);
		++split_runs;
	}

	void requestCorpus()
	{
		for(const char *method : {"GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD", "CUSTOM", "get"})
			everySplit(std::string(method) + " /raw%2Fpath?a=%00 HTTP/1.1\r\nHost: test\r\n\r\n", 0);

		everySplit("GET / HTTP/1.0\r\n\r\n", 0);
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 3\r\n\r\nabc", 0);
		everySplit("GET / HTTP/1.1\r\nHost: test\r\nX-Empty:\r\nX-Tabs: \ta\tb\t \r\n\r\n", 0);
		everySplit("G@T / HTTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit(" / HTTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("GET\t/ HTTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("GET  / HTTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1 \r\nHost: test\r\n\r\n", 400);
		everySplit("GET / HTTP/2.0\r\nHost: test\r\n\r\n", 505);
		everySplit("GET / HTTP/1.2\r\nHost: test\r\n\r\n", 505);
		everySplit("GET / HTTP/9.9\r\nHost: test\r\n\r\n", 505);
		everySplit("GET / HTTP/1.10\r\nHost: test\r\n\r\n", 400);
		everySplit("GET / FTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("GET / http/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("GET /#fragment HTTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("GET http://test/ HTTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("CONNECT test:80 HTTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("OPTIONS * HTTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("GET /bad\tpath HTTP/1.1\r\nHost: test\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\nHost: test\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\rHost: test\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\r\nHost: test\n\r\n", 400);
		everySplit("GET / HTTP/1.1\r\nHost: test\r\n folded: value\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\r\nHost: test\r\n\tfolded: value\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\r\nHost : test\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\r\nHost: test\r\n: empty-name\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\r\nHost: test\r\nNo-Colon\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\r\nHost: \t \r\n\r\n", 400);
		everySplit("GET / HTTP/1.1\r\nHost: test\r\nhOsT: test\r\n\r\n", 400);
		everySplit("GET / HTTP/1.0\r\nHost: test\r\nHost: test\r\n\r\n", 400);

		std::string nul_head = "GET / HTTP/1.1\r\nHost: te";
		nul_head.push_back('\0');
		nul_head += "st\r\n\r\n";
		everySplit(nul_head, 400);
		for(unsigned character = 1; character <= 127; ++character)
		{
			if(character >= 32 && character != 127)
				continue;

			if(character == '\t')
				continue;

			std::string invalid = "GET / HTTP/1.1\r\nHost: test\r\nX-Invalid: a";
			invalid.push_back(static_cast<char>(character));
			invalid += "b\r\n\r\n";
			everySplit(invalid, 400);
		}
	}

	void lengthCorpus()
	{
		for(const char *length : {"", "-1", "+1", "1,1", "1 1", "1x", "0x1", "184467440737095516160"})
		{
			everySplit(std::string("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: ") + length + "\r\n\r\n", 400);
		}

		for(const char *length : {"0", "1", "0003", "4096"})
			everySplit(std::string("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: ") + length + "\r\n\r\n", 0);

		everySplit("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 4097\r\n\r\n", 400);
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 1\r\ncontent-length: 1\r\n\r\n", 400);
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n", 400);
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: identity\r\n\r\n", 501);
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nTransfer-Encoding:\r\n\r\n", 501);
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nExpect: 100-continue\r\n\r\n", 417);
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nExpect:\r\n\r\n", 417);

		HttpLimits unlimited;
		unlimited.max_body_size = std::numeric_limits<std::size_t>::max();
		everySplit(std::string("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: ") +
		    std::to_string(unlimited.max_body_size) + "\r\n\r\n", 0, unlimited);
	}

	void limitsAndTail()
	{
		HttpLimits limits;
		limits.max_target_size = 16;
		for(std::size_t length : {15u, 16u, 17u})
			everySplit(std::string("GET /") + std::string(length - 1, 'a') + " HTTP/1.1\r\nHost: test\r\n\r\n",
			    length > limits.max_target_size ? 414 : 0, limits);

		limits = {};
		limits.max_headers = 2;
		everySplit("GET / HTTP/1.1\r\nHost: test\r\n\r\n", 0, limits);
		everySplit("GET / HTTP/1.1\r\nHost: test\r\nX: a\r\n\r\n", 0, limits);
		everySplit("GET / HTTP/1.1\r\nHost: test\r\nX: a\r\nY: b\r\n\r\n", 431, limits);
		for(std::size_t count : {31u, 32u, 33u})
		{
			std::string wire = "GET / HTTP/1.1\r\nHost: test\r\n";
			for(std::size_t index = 1; index < count; ++index)
				wire += "X: a\r\n";

			wire += "\r\n";
			everySplit(wire, count > 32 ? 431 : 0);
		}

		const std::string prefix = "GET / HTTP/1.1\r\nHost: test\r\nX: ";
		const std::string suffix = "\r\n\r\n";
		for(std::size_t head_size : {8191u, 8192u, 8193u})
		{
			Fixture fixture;
			const std::string wire = prefix + std::string(head_size - prefix.size() - suffix.size(), 'a') + suffix;
			fixture.copy(wire);
			RequestParser parser(fixture.storage.data(), fixture.storage.size(),
			    fixture.headers.data(), fixture.headers.size(), fixture.limits);
			for(std::size_t used = 0; used < wire.size(); ++used)
				(void)parser.advance(used);

			checkResult(parser.advance(wire.size()), head_size > 8192 ? 431 : 0);
		}

		limits = {};
		const std::string head = "POST /raw?x=%00 HTTP/1.1\r\nHost: test\r\nContent-Length: 3\r\n\r\n";
		limits.max_header_bytes = head.size();
		std::string wire = head;
		wire.append("a\0b", 3);
		wire += "GET /next HTTP/1.1\r\nHost: test\r\n\r\n";
		everySplit(wire, 0, limits);
		Fixture fixture;
		fixture.copy(wire);
		RequestParser parser(fixture.storage.data(), fixture.storage.size(),
		    fixture.headers.data(), fixture.headers.size(), limits);
		const auto result = parser.advance(wire.size());
		CHECK(result.head_bytes == head.size());
		CHECK(parser.head().target == "/raw?x=%00" && parser.head().content_length == 3);
		CHECK(std::memcmp(fixture.storage.data() + result.head_bytes, "a\0b", 3) == 0);
		CHECK(parser.headerCount() == 2);
		CHECK(parser.headerValue("Host").value() == "test");
		limits.max_body_size = 0;
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n", 0, limits);
		everySplit("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 1\r\n\r\n", 400, limits);

		// Every pair of split points crosses CRLF and delimiter boundaries.
		const std::string small = "GET / HTTP/1.1\r\nHost: t\r\n\r\n";
		for(std::size_t first = 0; first <= small.size(); ++first)
		{
			for(std::size_t second = first; second <= small.size(); ++second)
			{
				parser.reset();
				std::memcpy(fixture.storage.data(), small.data(), small.size());
				(void)parser.advance(first);
				(void)parser.advance(second);
				checkResult(parser.advance(small.size()), 0);
				++split_runs;
			}
		}
	}

	void metadataAndReuse()
	{
		Fixture fixture;
		const std::string wire =
		    "CUSTOM /raw%2f?q=%00 HTTP/1.1\r\nHost: test\r\nX-Mixed: first\r\nx-mixed: second\r\nX-Empty:\r\n\r\n";
		fixture.copy(wire);
		RequestParser parser(fixture.storage.data(), fixture.storage.size(),
		    fixture.headers.data(), fixture.headers.size(), fixture.limits);
		checkResult(parser.advance(wire.size()), 0);
		CHECK(parser.head().method == HttpMethod::Unknown && parser.http11());
		CHECK(parser.head().target == "/raw%2f?q=%00");
		CHECK(parser.head().target.data() == fixture.storage.data() + 7);
		CHECK(parser.headerCount() == 4 && parser.headerCount("X-MIXED") == 2);
		CHECK(parser.headerCount("missing") == 0);
		CHECK(parser.headerValue("x-MiXeD").value() == "first");
		CHECK(!parser.headerValue("missing"));
		std::array<char, 9> destination;
		destination.fill('!');
		CHECK(parser.readHeader("X-Mixed", destination.data() + 1, 5).status == HeaderReadStatus::InvalidArgument);
		CHECK(std::all_of(destination.begin(), destination.end(), [](char value) {
			return value == '!';
		}));
		CHECK(parser.readHeader("x-mixed", nullptr, 0).status == HeaderReadStatus::InvalidArgument);
		const auto copied = parser.readHeader("Host", destination.data() + 1, 5);
		CHECK(copied.status == HeaderReadStatus::Ok && copied.size == 4);
		CHECK(std::strcmp(destination.data() + 1, "test") == 0 && destination.front() == '!' && destination[6] == '!');
		CHECK(parser.readHeader("X-Empty", nullptr, 0).status == HeaderReadStatus::BufferTooSmall);
		CHECK(parser.readHeader("X-Empty", destination.data(), 1).status == HeaderReadStatus::Ok);
		CHECK(destination[0] == '\0');
		CHECK(parser.readHeader("missing", nullptr, 0).status == HeaderReadStatus::NotFound);
		CHECK(parser.readHeader(nullptr, nullptr, 0).status == HeaderReadStatus::InvalidArgument);
		CHECK(parser.readHeader("", nullptr, 0).status == HeaderReadStatus::InvalidArgument);
		CHECK(parser.readHeader("Host", nullptr, 1).status == HeaderReadStatus::InvalidArgument);
		CHECK(parser.readHeader("Host", nullptr, 0).size == 4);
		const auto host = parser.headerValue("Host");
		CHECK(host->data() >= fixture.storage.data() &&
		    host->data() + host->size() <= fixture.storage.data() + wire.size());

		parser.reset();
		CHECK(parser.headerCount() == 0 && !parser.headerValue("Host") && parser.rawHead().empty());
		CHECK(parser.head().target.empty() && parser.head().content_length == 0);
		const std::string second = "HEAD /second HTTP/1.0\r\n\r\n";
		fixture.copy(second);
		checkResult(parser.advance(second.size()), 0);
		CHECK(!parser.http11() && parser.head().method == HttpMethod::Head && parser.head().target == "/second");
		CHECK(parser.headerCount() == 0);

		// Parsing and bounded header copies allocate no storage of their own.
		fixture.copy(wire);
		allocations = 0;
		count_allocations = true;
		for(unsigned repeat = 0; repeat < 100; ++repeat)
		{
			parser.reset();
			for(std::size_t used = 0; used <= wire.size(); ++used)
				(void)parser.advance(used);

			CHECK(parser.readHeader("Host", destination.data(), destination.size()).status == HeaderReadStatus::Ok);
			CHECK(parser.headerValue("x-mixed").value() == "first");
		}
		count_allocations = false;
		CHECK(allocations == 0);
	}

	void invalidApi()
	{
		Fixture fixture;
		RequestParser parser(fixture.storage.data(), fixture.storage.size(),
		    fixture.headers.data(), fixture.headers.size(), fixture.limits);
		CHECK(parser.advance(0).state == ParseStatus::NeedMore);
		CHECK(parser.advance(fixture.storage.size() + 1).state == ParseStatus::InvalidState);
		CHECK(parser.advance(0).state == ParseStatus::InvalidState);
		parser.reset();
		fixture.copy("GET / HTTP/1.0\r\n\r\n");
		CHECK(parser.advance(5).state == ParseStatus::NeedMore);
		CHECK(parser.advance(4).state == ParseStatus::InvalidState);
		CHECK(parser.advance(18).state == ParseStatus::InvalidState);

		RequestParser no_storage(nullptr, 16384, fixture.headers.data(), fixture.headers.size(), fixture.limits);
		CHECK(no_storage.advance(0).state == ParseStatus::InvalidState);
		RequestParser small_storage(fixture.storage.data(), 8191, fixture.headers.data(), fixture.headers.size(),
		    fixture.limits);
		CHECK(small_storage.advance(0).state == ParseStatus::InvalidState);
		RequestParser no_index(fixture.storage.data(), fixture.storage.size(), nullptr, 32, fixture.limits);
		CHECK(no_index.advance(0).state == ParseStatus::InvalidState);
		RequestParser small_index(fixture.storage.data(), fixture.storage.size(), fixture.headers.data(), 31,
		    fixture.limits);
		CHECK(small_index.advance(0).state == ParseStatus::InvalidState);
		fixture.limits.max_headers = 0;
		RequestParser zero_headers(fixture.storage.data(), fixture.storage.size(), nullptr, 0, fixture.limits);
		checkResult(zero_headers.advance(18), 0);
		fixture.limits.max_header_bytes = 0;
		RequestParser no_limit(fixture.storage.data(), fixture.storage.size(), nullptr, 0, fixture.limits);
		CHECK(no_limit.advance(0).state == ParseStatus::InvalidState);

		fixture.limits = {};
		fixture.limits.max_headers = 1;
		const std::string_view over_index = "GET / HTTP/1.1\r\nHost: test\r\nExtra: value\r\n\r\n";
		fixture.copy(over_index);
		std::array<HeaderEntry, 3> guarded_index;
		guarded_index[0] = {11, 12, 13, 14};
		guarded_index[2] = {21, 22, 23, 24};
		RequestParser one_entry(fixture.storage.data(), fixture.storage.size(), guarded_index.data() + 1, 1,
		    fixture.limits);
		checkResult(one_entry.advance(over_index.size()), 431);
		CHECK(guarded_index[0].name_offset == 11 && guarded_index[0].name_size == 12 &&
		    guarded_index[0].value_offset == 13 && guarded_index[0].value_size == 14);
		CHECK(guarded_index[2].name_offset == 21 && guarded_index[2].name_size == 22 &&
		    guarded_index[2].value_offset == 23 && guarded_index[2].value_size == 24);
	}

} // namespace

int main()
{
	requestCorpus();
	lengthCorpus();
	limitsAndTail();
	metadataAndReuse();
	invalidApi();
	std::printf(
		"HTTP_WIRE_PARSER_PASS checks=%u split_runs=%u parser_bytes=%zu header_entry_bytes=%zu index32_bytes=%zu allocations=%zu\n",
		checks, split_runs, sizeof(RequestParser), sizeof(HeaderEntry), sizeof(HeaderEntry) * 32, allocations);
	return 0;
}
