// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstdint>
#include <string_view>
namespace NaWServer::json::detail
{
	inline bool digit(char c)
	{
		return c >= '0' && c <= '9';
	}

	inline int hex(char c)
	{
		if(c >= '0' && c <= '9') return c - '0';

		if(c >= 'a' && c <= 'f') return c - 'a' + 10;

		if(c >= 'A' && c <= 'F') return c - 'A' + 10;

		return -1;
	}

	inline bool hex4(std::string_view s, std::size_t &p, std::uint32_t &v)
	{
		v = 0;
		if(s.size() - p < 4) return false;

		for(unsigned n = 0; n < 4; ++n)
		{
			int h = hex(s[p++]);
			if(h < 0) return false;

			v = (v << 4) | static_cast<unsigned>(h);
		}
		return true;
	}

	// One Unicode scalar from a decoded UTF-8 string or a JSON string interior.
	inline bool scalar(std::string_view s, std::size_t &p, std::uint32_t &v, bool escaped)
	{
		if(p == s.size()) return false;

		unsigned char c = static_cast<unsigned char>(s[p++]);
		if(escaped && c == '\\')
		{
			if(p == s.size()) return false;

			switch(s[p++])
			{
			case '"': v = '"'; return true;
			case '\\': v = '\\'; return true;
			case '/': v = '/'; return true;
			case 'b': v = 8; return true;
			case 'f': v = 12; return true;
			case 'n': v = 10; return true;
			case 'r': v = 13; return true;
			case 't': v = 9; return true;
			case 'u': break;
			default: return false;
			}
			if(!hex4(s, p, v)) return false;

			if(v >= 0xdc00 && v <= 0xdfff) return false;

			if(v >= 0xd800 && v <= 0xdbff)
			{
				if(s.size() - p < 6 || s[p] != '\\' || s[p + 1] != 'u') return false;

				p += 2;
				std::uint32_t low;
				if(!hex4(s, p, low) || low < 0xdc00 || low > 0xdfff) return false;

				v = 0x10000 + ((v - 0xd800) << 10) + low - 0xdc00;
			}

			return true;
		}

		if(c < 0x80)
		{
			v = c;
			return !escaped || c >= 0x20;
		}

		unsigned extra;
		std::uint32_t minimum;
		if(c >= 0xc2 && c <= 0xdf) { extra = 1; v = c & 31; minimum = 0x80; }
		else if(c >= 0xe0 && c <= 0xef) { extra = 2; v = c & 15; minimum = 0x800; }
		else if(c >= 0xf0 && c <= 0xf4) { extra = 3; v = c & 7; minimum = 0x10000; }
		else return false;

		if(s.size() - p < extra) return false;

		while(extra--)
		{
			unsigned char next = static_cast<unsigned char>(s[p++]);
			if((next & 0xc0) != 0x80) return false;

			v = (v << 6) | (next & 63);
		}
		return v >= minimum && v <= 0x10ffff && !(v >= 0xd800 && v <= 0xdfff);
	}

	inline std::size_t encode(std::uint32_t v, char *out)
	{
		if(v < 0x80) { out[0] = static_cast<char>(v); return 1; }

		if(v < 0x800)
		{
			out[0] = static_cast<char>(0xc0 | (v >> 6)); out[1] = static_cast<char>(0x80 | (v & 63)); return 2;
		}

		if(v < 0x10000)
		{
			out[0] = static_cast<char>(0xe0 | (v >> 12)); out[1] = static_cast<char>(0x80 | ((v >> 6) & 63));
			out[2] = static_cast<char>(0x80 | (v & 63)); return 3;
		}

		out[0] = static_cast<char>(0xf0 | (v >> 18)); out[1] = static_cast<char>(0x80 | ((v >> 12) & 63));
		out[2] = static_cast<char>(0x80 | ((v >> 6) & 63)); out[3] = static_cast<char>(0x80 | (v & 63)); return 4;
	}

}
