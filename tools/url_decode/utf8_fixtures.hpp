// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace urlbench
{
	// Original test text, UTF-8 source literals; no normalization/grapheme parsing.
	static_assert(
		sizeof(u8"Я") == 3 && sizeof(u8"中") == 4 && sizeof(u8"😀") == 5,
		"UTF-8 source/execution encoding required");
	struct UnicodeFixture
	{
		const char *raw_name;
		const char *percent_name;
		std::string raw;
		std::string percent;
	};
	inline std::string percent_encode_all(const std::string &bytes)
	{
		constexpr char hex[] = "0123456789ABCDEF";
		std::string out;
		out.reserve(bytes.size() * 3);
		for(unsigned char b : bytes)
		{
			out += '%';
			out += hex[b >> 4];
			out += hex[b & 15];
		}
		return out;
	}

	inline std::string repeat_utf8(const std::string &seed, const std::string &pad)
	{
		// Repeat complete phrases, then complete filler code points; only the final
		// 0..3 bytes can be ASCII spaces. Never truncate a UTF-8 sequence or phrase.
		std::string out;
		while(out.size() + seed.size() <= 1024)
			out += seed;
		while(out.size() + pad.size() <= 1024)
			out += pad;
		out.append(1024 - out.size(), ' ');
		return out;
	}

	inline std::vector<UnicodeFixture> unicode_fixtures()
	{
		struct Seed
		{
			const char *raw_name;
			const char *percent_name;
			const char *text;
			const char *pad;
		};
		const Seed seeds[] = {
			{"utf8_ru_raw", "utf8_ru_pct", u8"Съешь ещё этих мягких французских булок, да выпей чаю. ", u8"Я"},
			{"utf8_zh_raw", "utf8_zh_pct", u8"中文字符解码测试，汉字和标点符号保持完整。", u8"中"},
			{"utf8_ja_raw", "utf8_ja_pct", u8"日本語の長い文字列です。ひらがな、カタカナ、漢字を保持します。", u8"日"},
			{"utf8_emoji_raw", "utf8_emoji_pct", u8"😀🚀🌍🎉👩‍💻🏳️‍🌈👍🏽❤️", u8"😀"}};
		std::vector<UnicodeFixture> fixtures;
		for(const auto &s : seeds)
		{
			auto raw = repeat_utf8(s.text, s.pad);
			auto percent = percent_encode_all(raw);
			fixtures.push_back({s.raw_name, s.percent_name, std::move(raw), std::move(percent)});
		}
		return fixtures;
	}

	inline uint32_t byte_checksum(const std::string &bytes)
	{
		uint32_t hash = 2166136261u;
		for(unsigned char b : bytes)
			hash = (hash ^ b) * 16777619u;
		return hash;
	}

} // namespace urlbench
