// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/form_parameters.h>
#include "../tools/url_decode/utf8_fixtures.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace
{
	using namespace NaWServer;
	unsigned checked = 0, failed = 0;
	void check(bool value, const char *label)
	{
		++checked;
		if(!value && ++failed < 12)
			std::printf("PARAM_FAIL %s index=%u\n", label, checked);
	}

	void field(const ParameterMap &fields, const std::string &key, const std::string &value)
	{
		auto found = fields.find(key);
		check(found != fields.end() && found->second.value() == value, "field bytes/length");
	}

	void roundtrip(const std::string &bytes)
	{
		const auto encoded = urlbench::percent_encode_all(bytes);
		for(auto mode : {UrlDecodeMode::Path, UrlDecodeMode::Form})
		{
			std::vector<char> out(bytes.size() + 2, char(0x5a));
			auto r = urlDecode(encoded.data(), encoded.size(), out.data() + 1, bytes.size(), mode);
			check(
				r.status == UrlDecodeStatus::Ok && r.consumed == encoded.size() && r.written == bytes.size(),
				"roundtrip result");
			check(!std::memcmp(out.data() + 1, bytes.data(), bytes.size()), "roundtrip bytes");
			check(out.front() == 0x5a && out.back() == 0x5a, "roundtrip exact capacity guards");
			auto inplace = encoded;
			r = urlDecode(inplace.data(), inplace.size(), inplace.data(), bytes.size(), mode);
			check(
				r.status == UrlDecodeStatus::Ok && r.written == bytes.size() &&
				!std::memcmp(inplace.data(), bytes.data(), bytes.size()),
				"roundtrip inplace");
			if(!bytes.empty())
			{
				r = urlDecode(encoded.data(), encoded.size(), out.data() + 1, bytes.size() - 1, mode);
				check(r.status == UrlDecodeStatus::NoSpace && r.written == bytes.size() - 1, "roundtrip insufficient");
			}
		}
	}

	uint32_t seed = 0x19c3abcd;
	uint32_t random32()
	{
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		return seed;
	}

} // namespace
int run_parameter_tests()
{
	using namespace NaWServer;
	checked = failed = 0;
	seed = 0x19c3abcd;
	check(urlDecode(nullptr, 0, nullptr, 0, UrlDecodeMode::Form).status == UrlDecodeStatus::Ok, "null empty");
	char storage[16];
	std::fill(std::begin(storage), std::end(storage), char(0x5a));
	check(urlDecode(nullptr, 1, storage, 1, UrlDecodeMode::Form).status == UrlDecodeStatus::BadArgument, "null input");
	check(urlDecode("a", 1, nullptr, 1, UrlDecodeMode::Form).status == UrlDecodeStatus::BadArgument, "null output");
	check(urlDecode("a", 1, nullptr, 0, UrlDecodeMode::Form).status == UrlDecodeStatus::NoSpace, "zero capacity");
	const char *malformed[] = {"%", "%0", "%GG", "%-1", "%+1", "%0x"};
	for(auto input : malformed)
	{
		auto r = urlDecode(input, std::strlen(input), storage, 0, UrlDecodeMode::Form);
		check(
			r.status == UrlDecodeStatus::InvalidEscape && r.consumed == 0 && r.written == 0,
			"invalid before no-space");
		check(storage[0] == 0x5a, "invalid no writes");
	}
	for(auto mode : {UrlDecodeMode::Path, UrlDecodeMode::Form})
	{
		auto r = urlDecode("a+%2B%00Z", 9, storage, 5, mode);
		const std::string expected = mode == UrlDecodeMode::Form ? std::string("a +\0Z", 5) : std::string("a++\0Z", 5);
		check(
			r.status == UrlDecodeStatus::Ok && r.written == 5 && !std::memcmp(storage, expected.data(), 5),
			"plus/NUL modes");
	}
	char overlap[] = "abcdef";
	auto before = std::string(overlap, sizeof overlap);
	check(
		urlDecode(overlap, 5, overlap + 1, 5, UrlDecodeMode::Form).status == UrlDecodeStatus::Overlap,
		"overlap right");
	check(
		urlDecode(overlap + 1, 5, overlap, 5, UrlDecodeMode::Form).status == UrlDecodeStatus::Overlap,
		"overlap left");
	check(!std::memcmp(overlap, before.data(), sizeof overlap), "overlap unchanged");
	auto prefix = urlDecode("ok%GG", 5, storage, 16, UrlDecodeMode::Form);
	check(
		prefix.status == UrlDecodeStatus::InvalidEscape && prefix.consumed == 2 && prefix.written == 2 &&
		storage[0] == 'o' && storage[1] == 'k',
		"invalid prefix offset");
	const std::string alphabet = "0123456789abcdef";
	for(unsigned a = 0; a < 256; ++a)
		for(unsigned b = 0; b < 256; ++b)
		{
#ifdef ESP_PLATFORM
			if(b == 0 && a % 16 == 0)
				vTaskDelay(1);

#endif
			char input[] = {'%', char(a), char(b)};
			auto lower = [](unsigned c)
				{
					return c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c;
				};
			auto hi = alphabet.find(char(lower(a))), lo = alphabet.find(char(lower(b)));
			bool valid = hi != std::string::npos && lo != std::string::npos;
			char out = 0x5a;
			auto r = urlDecode(input, 3, &out, 1, UrlDecodeMode::Form);
			check(
				valid
				    ? (r.status == UrlDecodeStatus::Ok && r.consumed == 3 && r.written == 1 &&
				static_cast<unsigned char>(out) == hi * 16 + lo)
				    : (r.status == UrlDecodeStatus::InvalidEscape && r.consumed == 0 && r.written == 0 && out == 0x5a),
				"exhaustive hex pairs");
		}
	std::string all;
	for(unsigned b = 0; b < 256; ++b)
		all += char(b);
	roundtrip(all);
	roundtrip("");
	for(const auto &f : urlbench::unicode_fixtures())
	{
		roundtrip(f.raw);
		ParameterMap fields;
		check(parseFormParameters("value=" + f.percent, fields).status == UrlDecodeStatus::Ok, "Unicode form parse");
		field(fields, "value", f.raw);
	}
	ParameterMap fields;
	check(
		parseFormParameters("&&flag&empty=&=anonymous&a=first&a=second&A=upper&v%61lue=a%26b%3Dc%2B+%00Z&&", fields)
		.status == UrlDecodeStatus::Ok,
		"form syntax");
	field(fields, "flag", "");
	field(fields, "empty", "");
	field(fields, "", "anonymous");
	field(fields, "a", "first");
	field(fields, "A", "upper");
	field(fields, "value", std::string("a&b=c+ \0Z", 9));
	const std::string nul_key("k\0x", 3), nul_value("A\0B", 3);
	check(
		parseFormParameters(
			urlbench::percent_encode_all(nul_key) + "=" + urlbench::percent_encode_all(nul_value),
			fields)
		.status == UrlDecodeStatus::Ok,
		"NUL encoded key/value");
	field(fields, nul_key, nul_value);
	check(
		parseFormParameters(nul_key + "=" + nul_value + "&tail=yes", fields).status == UrlDecodeStatus::Ok,
		"NUL raw key/value");
	field(fields, nul_key, nul_value);
	field(fields, "tail", "yes");
	for(const auto &bad : std::vector<std::pair<std::string, size_t> >{
		{"a=ok&bad=%GG", 9},
		{"a=ok&%GG=v", 5},
		{"a=ok&a=%GG", 7},
		{"x=%", 2},
		{"x=%0", 2}})
	{
		const auto size = fields.size();
		auto r = parseFormParameters(bad.first, fields);
		check(r.status == UrlDecodeStatus::InvalidEscape && r.offset == bad.second, "form error offset");
		check(fields.size() == size, "transactional map size");
		field(fields, nul_key, nul_value);
		field(fields, "tail", "yes");
	}
	check(parseFormParameters("", fields).status == UrlDecodeStatus::Ok && fields.empty(), "empty replaces map");
	for(auto type :
	    {"application/x-www-form-urlencoded",
	     "Application/X-WWW-Form-Urlencoded; charset=UTF-8",
	     " \tapplication/x-www-form-urlencoded\t "})
		check(isFormContentType(type), "form media type");
	for(auto type :
	    {"",
	     "application/json",
	     "application/octet-stream",
	     "application/x-www-form-urlencoded-extra",
	     "multipart/form-data"})
		check(!isFormContentType(type), "non-form media type");
	for(unsigned i = 0; i < 1000; ++i)
	{
#ifdef ESP_PLATFORM
		if(i % 64 == 0)
			vTaskDelay(1);

#endif
		std::string key, value;
		for(unsigned j = random32() % 64; j > 0; --j)
			key += char(random32());
		for(unsigned j = random32() % 128; j > 0; --j)
			value += char(random32());
		roundtrip(value);
		check(
			parseFormParameters(urlbench::percent_encode_all(key) + "=" + urlbench::percent_encode_all(value), fields)
			.status == UrlDecodeStatus::Ok,
			"random form parse");
		check(fields.size() == 1, "random one field");
		field(fields, key, value);
	}
	std::printf("PARAM_TEST backend=%s checks=%u failed=%u\n", urlDecodeBackend(), checked, failed);
	return static_cast<int>(failed);
}

#ifndef ESP_PLATFORM
int main()
{
	return run_parameter_tests();
}

#endif
