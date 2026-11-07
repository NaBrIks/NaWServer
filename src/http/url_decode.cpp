// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

// Only the selected hex-pair strategy is compiled.
#include <NaWServer/url_decode.h>
#include <cstdint>
#if NAWSERVER_URL_DECODE_BACKEND == NAWSERVER_URL_DECODE_LOOKUP
#include <array>
#else
#include <charconv>
#endif

namespace NaWServer
{
	namespace
	{
#if NAWSERVER_URL_DECODE_BACKEND == NAWSERVER_URL_DECODE_LOOKUP
		constexpr std::array<uint8_t, 256> makeHexTable()
		{
			std::array<uint8_t, 256> table{};
			for(auto &entry : table)
				entry = 255;
			for(unsigned i = 0; i < 10; ++i)
				table['0' + i] = static_cast<uint8_t>(i);
			for(unsigned i = 0; i < 6; ++i)
			{
				table['a' + i] = static_cast<uint8_t>(i + 10);
				table['A' + i] = static_cast<uint8_t>(i + 10);
			}
			return table;
		}

		constexpr auto hexTable = makeHexTable();
		int decodePair(const char *p)
		{
			const unsigned a = hexTable[static_cast<unsigned char>(p[0])];
			const unsigned b = hexTable[static_cast<unsigned char>(p[1])];
			return (a | b) > 15 ? -1 : static_cast<int>(a * 16 + b);
		}

#else
		int decodePair(const char *p)
		{
			unsigned value = 0;
			const auto parsed = std::from_chars(p, p + 2, value, 16);
			return parsed.ec == std::errc{} && parsed.ptr == p + 2 ? static_cast<int>(value) : -1;
		}

#endif
	} // namespace
	UrlDecodeResult
	urlDecode(const char *source, std::size_t length, char *destination, std::size_t capacity, UrlDecodeMode mode)
	{
		if((length && !source) || (capacity && !destination))
			return {UrlDecodeStatus::BadArgument, 0, 0};

		const auto a = reinterpret_cast<uintptr_t>(source), b = reinterpret_cast<uintptr_t>(destination);
		if(source != destination && length && capacity && ((a < b && b - a < length) || (b < a && a - b < capacity)))
			return {UrlDecodeStatus::Overlap, 0, 0};

		std::size_t read = 0, written = 0;
		while(read < length)
		{
			unsigned char value = static_cast<unsigned char>(source[read]);
			std::size_t step = 1;
			if(value == '%')
			{
				if(length - read < 3)
					return {UrlDecodeStatus::InvalidEscape, read, written};

				const int decoded = decodePair(source + read + 1);
				if(decoded < 0)
					return {UrlDecodeStatus::InvalidEscape, read, written};

				value = static_cast<unsigned char>(decoded);
				step = 3;
			}
			else if(value == '+' && mode == UrlDecodeMode::Form)
				value = ' ';

			if(written == capacity)
				return {UrlDecodeStatus::NoSpace, read, written};

			destination[written++] = static_cast<char>(value);
			read += step;
		}
		return {UrlDecodeStatus::Ok, read, written};
	}

} // namespace NaWServer
