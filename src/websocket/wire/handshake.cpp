// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "handshake.h"
#include "detail/ascii.h"
#include <array>
#include <cstring>

namespace NaWServer::websocket::wire
{

	namespace
	{

		constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		constexpr char websocket_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

		int base64Value(unsigned char character)
		{
			if(character >= 'A' && character <= 'Z')
				return character - 'A';

			if(character >= 'a' && character <= 'z')
				return character - 'a' + 26;

			if(character >= '0' && character <= '9')
				return character - '0' + 52;

			if(character == '+')
				return 62;

			return character == '/' ? 63 : -1;
		}

		std::uint32_t rotateLeft(std::uint32_t word, unsigned count)
		{
			return (word << count) | (word >> (32 - count));
		}

		// Written from RFC 3174 sections 4-6's algorithm, not its sample C code.
		// The only input is the protocol's fixed 60-byte key+GUID, padded into two
		// blocks on demand. Sixteen schedule words avoid an 80-word/full-input copy.
		std::array<std::uint8_t, 20> handshakeDigest(std::string_view key)
		{
			std::array<std::uint32_t, 5> state{{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0}};
			const auto inputByte = [&key](std::size_t index) -> std::uint8_t
				{
					if(index < 24)
						return static_cast<std::uint8_t>(key[index]);

					if(index < 60)
						return static_cast<std::uint8_t>(websocket_guid[index - 24]);

					if(index == 60)
						return 0x80;

					// 60 * 8 = 480 bits, encoded in the final two big-endian bytes.
					if(index == 126)
						return 1;

					return index == 127 ? 0xe0 : 0;
				};

			for(std::size_t block = 0; block < 2; ++block)
			{
				std::array<std::uint32_t, 16> words;
				for(std::size_t index = 0; index < words.size(); ++index)
				{
					const auto offset = block * 64 + index * 4;
					words[index] = (static_cast<std::uint32_t>(inputByte(offset)) << 24) |
					    (static_cast<std::uint32_t>(inputByte(offset + 1)) << 16) |
					    (static_cast<std::uint32_t>(inputByte(offset + 2)) << 8) | inputByte(offset + 3);
				}

				std::uint32_t first = state[0], second = state[1], third = state[2], fourth = state[3],
				    fifth = state[4];
				for(unsigned round = 0; round < 80; ++round)
				{
					const unsigned slot = round & 15;
					if(round >= 16)
						words[slot] = rotateLeft(words[(round - 3) & 15] ^ words[(round - 8) & 15] ^
						        words[(round - 14) & 15] ^ words[slot], 1);

					std::uint32_t function, constant;
					if(round < 20)
					{
						function = (second & third) | (~second & fourth);
						constant = 0x5a827999;
					}
					else if(round < 40)
					{
						function = second ^ third ^ fourth;
						constant = 0x6ed9eba1;
					}
					else if(round < 60)
					{
						function = (second & third) | (second & fourth) | (third & fourth);
						constant = 0x8f1bbcdc;
					}
					else
					{
						function = second ^ third ^ fourth;
						constant = 0xca62c1d6;
					}

					const std::uint32_t next = rotateLeft(first, 5) + function + fifth + words[slot] + constant;
					fifth = fourth;
					fourth = third;
					third = rotateLeft(second, 30);
					second = first;
					first = next;
				}

				state[0] += first;
				state[1] += second;
				state[2] += third;
				state[3] += fourth;
				state[4] += fifth;
			}

			std::array<std::uint8_t, 20> digest;
			for(std::size_t index = 0; index < digest.size(); ++index)
				digest[index] = static_cast<std::uint8_t>(state[index / 4] >> (24 - (index % 4) * 8));

			return digest;
		}

		bool hasToken(std::string_view values, std::string_view token)
		{
			while(!values.empty())
			{
				const auto comma = values.find(',');
				auto value = values.substr(0, comma);
				while(!value.empty() && (value.front() == ' ' || value.front() == '\t'))
					value.remove_prefix(1);

				while(!value.empty() && (value.back() == ' ' || value.back() == '\t'))
					value.remove_suffix(1);

				if(NaWServer::detail::asciiEqualIgnoreCase(value, token))
					return true;

				if(comma == values.npos)
					break;

				values.remove_prefix(comma + 1);
			}
			return false;
		}

	} // namespace

	bool validWebSocketKey(std::string_view key)
	{
		if(key.size() != 24 || key.substr(22) != "==")
			return false;

		for(std::size_t index = 0; index < 22; ++index)
			if(base64Value(static_cast<unsigned char>(key[index])) < 0)
				return false;

		// A 16-byte nonce ends in two useful bits and four canonical zero pad bits.
		return (base64Value(static_cast<unsigned char>(key[21])) & 15) == 0;
	}

	bool makeWebSocketAccept(std::string_view key, char *destination, std::size_t capacity)
	{
		if(!destination || capacity < 29 || !validWebSocketKey(key))
			return false;

		const auto digest = handshakeDigest(key);
		std::size_t cursor = 0;
		for(std::size_t index = 0; index < 18; index += 3)
		{
			const std::uint32_t packed = (static_cast<std::uint32_t>(digest[index]) << 16) |
			    (static_cast<std::uint32_t>(digest[index + 1]) << 8) | digest[index + 2];
			destination[cursor++] = alphabet[(packed >> 18) & 63];
			destination[cursor++] = alphabet[(packed >> 12) & 63];
			destination[cursor++] = alphabet[(packed >> 6) & 63];
			destination[cursor++] = alphabet[packed & 63];
		}
		destination[cursor++] = alphabet[digest[18] >> 2];
		destination[cursor++] = alphabet[((digest[18] & 3) << 4) | (digest[19] >> 4)];
		destination[cursor++] = alphabet[(digest[19] & 15) << 2];
		destination[cursor++] = '=';
		destination[cursor] = '\0';
		return true;
	}

	int validateHandshake(const http::wire::RequestParser &request)
	{
		if(request.head().method != HttpMethod::Get || !request.http11() || request.head().content_length != 0)
			return 400;

		for(const char *name : {"Upgrade", "Connection", "Sec-WebSocket-Version", "Sec-WebSocket-Key"})
			if(request.headerCount(name) != 1)
				return 400;

		return hasToken(*request.headerValue("Upgrade"), "websocket") &&
		       hasToken(*request.headerValue("Connection"), "upgrade") &&
		       *request.headerValue("Sec-WebSocket-Version") == "13" &&
		       validWebSocketKey(*request.headerValue("Sec-WebSocket-Key")) ? 0 : 400;
	}

} // namespace NaWServer::websocket::wire
