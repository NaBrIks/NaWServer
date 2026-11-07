// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "detail/ascii.h"
#include "http/header_validation.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{
	unsigned checks;
#define CHECK(expression)                                                                                              \
		do                                                                                                                 \
		{                                                                                                                  \
			++checks;                                                                                                      \
			if(!(expression))                                                                                              \
			{                                                                                                              \
				std::printf("HEADER_VALIDATION_FAIL line=%d expression=%s\n", __LINE__, #expression);                      \
				std::abort();                                                                                              \
			}                                                                                                              \
		} while(false)
} // namespace

int main()
{
	using namespace NaWServer::detail;
	// The explicit protocol alphabets are the oracle, including every byte
	// outside ASCII. Neither locale nor signed char may widen these alphabets.
	constexpr std::string_view uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	constexpr std::string_view lowercase = "abcdefghijklmnopqrstuvwxyz";
	constexpr std::string_view
	    token_characters = "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	std::array<unsigned char, 256> folded{};
	for(unsigned byte = 0; byte < folded.size(); ++byte)
	{
		const char character = static_cast<char>(byte);
		const auto position = uppercase.find(character);
		folded[byte] = position == uppercase.npos ? static_cast<unsigned char>(byte)
		                                          : static_cast<unsigned char>(lowercase[position]);
		CHECK(toLowerAscii(static_cast<unsigned char>(byte)) == folded[byte]);
		const std::string_view one_byte(&character, 1);
		CHECK(isValidHeaderName(one_byte) == (token_characters.find(character) != token_characters.npos));
		CHECK(isValidHeaderValue(one_byte) == (byte == 9 || (byte >= 32 && byte != 127)));
	}
	for(unsigned left = 0; left < folded.size(); ++left)
	{
		for(unsigned right = 0; right < folded.size(); ++right)
		{
			const char left_character = static_cast<char>(left), right_character = static_cast<char>(right);
			CHECK(asciiEqualIgnoreCase({&left_character, 1}, {&right_character, 1}) == (folded[left] == folded[right]));
		}
	}
	CHECK(isValidHeaderName(token_characters));
	CHECK(!isValidHeaderName({}));
	CHECK(isValidHeaderValue({}));
	CHECK(isValidHeaderValue(" \tvalue\t "));
	CHECK(!isValidHeaderName(std::string_view("X\0Key", 5)));
	CHECK(!isValidHeaderValue(std::string_view("a\0b", 3)));
	CHECK(!isValidHeaderValue("value\r\nInjected: yes"));
	CHECK(asciiEqualIgnoreCase({}, {}));
	CHECK(asciiEqualIgnoreCase("Content-TYPE", "content-type"));
	CHECK(!asciiEqualIgnoreCase("X-Key", "X-Keys"));
	CHECK(!asciiEqualIgnoreCase(std::string_view("a\0b", 3), std::string_view("A\0c", 3)));
	CHECK(asciiEqualIgnoreCase(std::string_view("a\0b", 3), std::string_view("A\0B", 3)));
	std::printf("HEADER_VALIDATION_CHECKS passed=%u\n", checks);
}
