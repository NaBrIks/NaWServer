// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "websocket/wire/handshake.h"
#include "detail/utf8.h"
#include <NaWServer/detail/sse/encoder.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace
{

	using namespace NaWServer;
	using namespace NaWServer::websocket::wire;
	unsigned checks = 0;
	unsigned oracle_vectors = 0;

	void check(bool condition, int line)
	{
		++checks;
		if(!condition)
		{
			std::printf("WEBSOCKET_HANDSHAKE_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	void checkHandshake(const std::string &wire, int expected)
	{
		std::array<char, 8192> storage;
		std::array<http::wire::HeaderEntry, 32> entries;
		CHECK(wire.size() <= storage.size());
		std::memcpy(storage.data(), wire.data(), wire.size());
		http::wire::RequestParser parser(storage.data(), storage.size(), entries.data(), entries.size(), {});
		CHECK(parser.advance(wire.size()).state == http::wire::ParseStatus::Complete);
		CHECK(validateHandshake(parser) == expected);
	}

	void handshakeCorpus()
	{
		const std::string start = "GET /ws HTTP/1.1\r\nHost: test\r\n";
		const std::string fields = "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n";
		const std::string key = "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
		checkHandshake(start + fields + key + "\r\n", 0);
		checkHandshake(start + "uPgRaDe: WebSocket\r\nConnection: keep-alive, Upgrade\r\n" +
		    "Sec-WebSocket-Version: 13\r\n" + key + "\r\n", 0);
		checkHandshake(start + fields + key + "Sec-WebSocket-Extensions: permessage-deflate\r\n" +
		    "Sec-WebSocket-Protocol: arbitrary\r\n\r\n", 0);
		checkHandshake("POST /ws HTTP/1.1\r\nHost: test\r\n" + fields + key + "\r\n", 400);
		checkHandshake("GET /ws HTTP/1.0\r\nHost: test\r\n" + fields + key + "\r\n", 400);
		checkHandshake(start + fields + key + "Content-Length: 1\r\n\r\nx", 400);
		for(const char *field : {"Upgrade: websocket\r\n", "Connection: Upgrade\r\n",
		                         "Sec-WebSocket-Version: 13\r\n", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"})
		{
			checkHandshake(start + fields + key + field + "\r\n", 400);
			std::string missing = start + fields + key;
			missing.erase(missing.find(field), std::strlen(field));
			checkHandshake(missing + "\r\n", 400);
		}

		checkHandshake(start + "Upgrade: websocketeer\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n" + key +
		    "\r\n", 400);
		checkHandshake(start + "Upgrade: websocket\r\nConnection: UpgradeExtra\r\nSec-WebSocket-Version: 13\r\n" + key +
		    "\r\n", 400);
		checkHandshake(start + "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 12\r\n" + key +
		    "\r\n", 400);
		checkHandshake(start + fields + "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAB==\r\n\r\n", 400);
	}

	void keyAndAccept()
	{
		constexpr std::string_view sample = "dGhlIHNhbXBsZSBub25jZQ==";
		CHECK(validWebSocketKey(sample));
		std::array<char, 31> output;
		output.fill('!');
		CHECK(makeWebSocketAccept(sample, output.data() + 1, 29));
		CHECK(std::strcmp(output.data() + 1, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
		CHECK(output.front() == '!' && output.back() == '!');
		output.fill('!');
		CHECK(!makeWebSocketAccept(sample, output.data(), 28));
		CHECK(std::all_of(output.begin(), output.end(), [](char value) {
			return value == '!';
		}));
		CHECK(!makeWebSocketAccept(sample, nullptr, 29));
		for(const char *invalid : {"", "dGhlIHNhbXBsZSBub25jZQ=", "dGhlIHNhbXBsZSBub25jZQ===",
		                           "dGhlIHNhbXBsZSBub25jZR==", "dGhlIHNhbXBsZSBub25jZQ  ", "dGhlIHNhbXBsZSBub25j-Q==",
		                           "dGhlIHNhbXBsZSBub25j_Q==", "dGhlIHNhbXBsZSBub25jZ===", "=GhlIHNhbXBsZSBub25jZQ=="})
		{
			CHECK(!validWebSocketKey(invalid));
			CHECK(!makeWebSocketAccept(invalid, output.data(), output.size()));
			CHECK(output.front() == '!');
		}

		for(char last : {'A', 'Q', 'g', 'w'})
		{
			std::string nonce(21, 'A');
			nonce += last;
			nonce += "==";
			CHECK(validWebSocketKey(nonce));
		}
	}

	void utf8Corpus()
	{
		const auto valid = [](std::string_view bytes)
			{
				CHECK(detail::validUtf8(bytes));
				CHECK(sse::valid_utf8(bytes));
			};
		valid({});
		valid(std::string_view("a\0b", 3));
		std::array<char, 4> encoded;
		for(std::uint32_t codepoint = 0; codepoint <= 0x10ffff; ++codepoint)
		{
			if(codepoint >= 0xd800 && codepoint <= 0xdfff)
				continue;

			std::size_t size;
			if(codepoint < 0x80)
			{
				encoded[0] = static_cast<char>(codepoint);
				size = 1;
			}
			else if(codepoint < 0x800)
			{
				encoded[0] = static_cast<char>(0xc0 | (codepoint >> 6));
				encoded[1] = static_cast<char>(0x80 | (codepoint & 63));
				size = 2;
			}
			else if(codepoint < 0x10000)
			{
				encoded[0] = static_cast<char>(0xe0 | (codepoint >> 12));
				encoded[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 63));
				encoded[2] = static_cast<char>(0x80 | (codepoint & 63));
				size = 3;
			}
			else
			{
				encoded[0] = static_cast<char>(0xf0 | (codepoint >> 18));
				encoded[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 63));
				encoded[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 63));
				encoded[3] = static_cast<char>(0x80 | (codepoint & 63));
				size = 4;
			}

			valid({encoded.data(), size});
		}

		for(const std::string &invalid : {std::string("\xc0\x80", 2), std::string("\xc1\xbf", 2),
		                                  std::string("\xe0\x80\x80", 3), std::string("\xed\xa0\x80", 3),
		                                  std::string("\xf0\x80\x80\x80", 4), std::string("\xf4\x90\x80\x80", 4),
		                                  std::string("\xf5\x80\x80\x80", 4), std::string("\x80", 1),
		                                  std::string("\xff", 1),
		                                  std::string("\xc2", 1), std::string("\xe2\x82", 2),
		                                  std::string("\xf0\x9f\x98", 3),
		                                  std::string("\xe2x\xac", 3)})
		{
			CHECK(!detail::validUtf8(invalid));
			CHECK(!sse::valid_utf8(invalid));
		}
		CHECK(!sse::valid_field(std::string_view("a\0b", 3)));
	}

	void oracle()
	{
		std::string line;
		while(std::getline(std::cin, line))
		{
			const auto delimiter = line.find('\t');
			CHECK(delimiter == 24 && line.size() == 53);
			std::array<char, 29> accept;
			CHECK(makeWebSocketAccept(std::string_view(line).substr(0, delimiter), accept.data(), accept.size()));
			CHECK(std::string_view(accept.data(), 28) == std::string_view(line).substr(delimiter + 1));
			++oracle_vectors;
		}
		CHECK(oracle_vectors > 0);
	}

} // namespace

int main(int argc, char **argv)
{
	keyAndAccept();
	handshakeCorpus();
	utf8Corpus();
	if(argc == 2 && std::strcmp(argv[1], "--oracle") == 0)
		oracle();

	std::printf("WEBSOCKET_HANDSHAKE_PASS checks=%u oracle_vectors=%u utf8_scalars=1112064\n", checks, oracle_vectors);
	return 0;
}
