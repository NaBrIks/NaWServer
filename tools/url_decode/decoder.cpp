// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

// New experimental implementations, authored against CONTRACT.md, 2026-10-19.
#include "decoder.hpp"
#include <array>
#include <charconv>
#include <cstring>
#include <algorithm>
static_assert(__cplusplus == 201703L, "Benchmark candidates must use C++17");

namespace urlbench
{
	namespace
	{
		Status check(const char *s, size_t n, char *d, size_t cap)
		{
			if((n && !s) || (cap && !d))
				return Status::bad_argument;

			const auto a = reinterpret_cast<uintptr_t>(s), b = reinterpret_cast<uintptr_t>(d);
			if(s != d && n && cap && ((a < b && b - a < n) || (b < a && a - b < cap)))
				return Status::overlap;

			return Status::ok;
		}

		int nibble(unsigned char c)
		{
			if(c >= '0' && c <= '9')
				return c - '0';

			const unsigned lower = c | 32u;
			return lower >= 'a' && lower <= 'f' ? int(lower - 'a' + 10) : -1;
		}

		struct Direct
		{
			int operator()(const char *p) const
			{
				const int a = nibble(static_cast<unsigned char>(p[0]));
				const int b = nibble(static_cast<unsigned char>(p[1]));
				return a < 0 || b < 0 ? -1 : a * 16 + b;
			}

		};
		constexpr std::array<uint8_t, 256> make_table()
		{
			std::array<uint8_t, 256> t{};
			for(auto &v : t)
				v = 255;
			for(unsigned i = 0; i < 10; ++i)
				t['0' + i] = i;
			for(unsigned i = 0; i < 6; ++i)
			{
				t['a' + i] = i + 10;
				t['A' + i] = i + 10;
			}
			return t;
		}

		constexpr auto hex_table = make_table();
		struct Lookup
		{
			int operator()(const char *p) const
			{
				const unsigned a = hex_table[static_cast<unsigned char>(p[0])];
				const unsigned b = hex_table[static_cast<unsigned char>(p[1])];
				return (a | b) > 15 ? -1 : int(a * 16 + b);
			}

		};
		struct Standard
		{
			int operator()(const char *p) const
			{
				unsigned v = 0;
				const auto r = std::from_chars(p, p + 2, v, 16);
				return r.ec == std::errc{} && r.ptr == p + 2 ? int(v) : -1;
			}

		};
		template <class Hex> Result scalar(const char *s, size_t n, char *d, size_t cap, Mode m, Hex hex)
		{
			const Status status = check(s, n, d, cap);
			if(status != Status::ok)
				return {status, 0, 0};

			size_t r = 0, w = 0;
			while(r < n)
			{
				unsigned char value = static_cast<unsigned char>(s[r]);
				size_t step = 1;
				if(value == '%')
				{
					if(n - r < 3)
						return {Status::invalid_escape, r, w};

					const int decoded = hex(s + r + 1);
					if(decoded < 0)
						return {Status::invalid_escape, r, w};

					value = static_cast<unsigned char>(decoded);
					step = 3;
				}
				else if(value == '+' && m == Mode::form)
					value = ' ';

				if(w == cap)
					return {Status::no_space, r, w};

				d[w++] = static_cast<char>(value);
				r += step;
			}
			return {Status::ok, r, w};
		}

	} // namespace
#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
	NOINLINE Result direct(const char *s, size_t n, char *d, size_t c, Mode m)
	{
		return scalar(s, n, d, c, m, Direct{});
	}

	NOINLINE Result lookup(const char *s, size_t n, char *d, size_t c, Mode m)
	{
		return scalar(s, n, d, c, m, Lookup{});
	}

	NOINLINE Result standard(const char *s, size_t n, char *d, size_t c, Mode m)
	{
		return scalar(s, n, d, c, m, Standard{});
	}

	NOINLINE Result spans(const char *s, size_t n, char *d, size_t cap, Mode m)
	{
		const Status status = check(s, n, d, cap);
		if(status != Status::ok)
			return {status, 0, 0};

		size_t r = 0, w = 0;
		while(r < n)
		{
			const auto percent = static_cast<const char *>(std::memchr(s + r, '%', n - r));
			size_t end = n;
			if(percent)
				end = size_t(percent - s);

			const size_t count = std::min(end - r, cap - w);
			if(count)
			{
				std::memmove(d + w, s + r, count);
				if(m == Mode::form)
					std::replace(d + w, d + w + count, '+', ' ');
			}

			r += count;
			w += count;
			if(r < end)
				return {Status::no_space, r, w};

			if(r == n)
				break;

			int value = ' ';
			size_t step = 1;
			if(s[r] == '%')
			{
				if(n - r < 3 || (value = Direct{}(s + r + 1)) < 0)
					return {Status::invalid_escape, r, w};

				step = 3;
			}

			if(w == cap)
				return {Status::no_space, r, w};

			d[w++] = static_cast<char>(value);
			r += step;
		}
		return {Status::ok, r, w};
	}

} // namespace urlbench
