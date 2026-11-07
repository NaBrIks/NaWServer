// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "decoder.hpp"
#include "utf8_fixtures.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace urlbench
{
	namespace
	{
		unsigned checks = 0, failures = 0;
		void expect(bool yes, const char *label)
		{
			++checks;
			if(!yes && ++failures < 12)
				std::printf("FAIL %s check=%u\n", label, checks);
		}

		// Independent oracle: alphabet search and an owning output string, not a candidate.
		Result oracle(const std::string &s, size_t cap, Mode mode, std::string &output)
		{
			const std::string alphabet = "0123456789abcdef";
			output.clear();
			size_t i = 0;
			while(i < s.size())
			{
				char c = s[i];
				size_t step = 1;
				if(c == '%')
				{
					if(s.size() - i < 3)
						return {Status::invalid_escape, i, output.size()};

					char a = s[i + 1], b = s[i + 2];
					if(a >= 'A' && a <= 'F')
						a = char(a - 'A' + 'a');

					if(b >= 'A' && b <= 'F')
						b = char(b - 'A' + 'a');

					auto hi = alphabet.find(a), lo = alphabet.find(b);
					if(hi == std::string::npos || lo == std::string::npos)
						return {Status::invalid_escape, i, output.size()};

					c = char(hi * 16 + lo);
					step = 3;
				}
				else if(c == '+' && mode == Mode::form)
					c = ' ';

				if(output.size() == cap)
					return {Status::no_space, i, output.size()};

				output.push_back(c);
				i += step;
			}
			return {Status::ok, i, output.size()};
		}

		void exercise(const std::string &input)
		{
			for(auto mode : {Mode::path, Mode::form})
			{
				std::string full;
				oracle(input, input.size(), mode, full);
				const size_t caps[] =
				{0, full.empty() ? 0 : full.size() - 1, full.size(), input.size(), input.size() + 1};
				for(size_t cap : caps)
				{
					std::string expected;
					const auto want = oracle(input, cap, mode, expected);
					for(const auto &variant : variants)
					{
						std::vector<char> out(std::max(input.size(), cap) + 4, char(0x5a));
						auto got = variant.decode(input.data(), input.size(), out.data() + 1, cap, mode);
						expect(
							got.status == want.status && got.consumed == want.consumed && got.written == want.written,
							variant.name);
						expect(!std::memcmp(out.data() + 1, expected.data(), expected.size()), "bytes/disjoint");
						expect(out.front() == 0x5a && out[cap + 1] == 0x5a, "guard/disjoint");
						expect(
							std::all_of(
								out.begin() + 1 + got.written,
								out.end(),
								[](char b)
						{
							return b == 0x5a;
						}),
							"tail/disjoint");
						std::fill(out.begin(), out.end(), char(0x5a));
						std::memcpy(out.data() + 1, input.data(), input.size());
						auto before = out;
						got = variant.decode(out.data() + 1, input.size(), out.data() + 1, cap, mode);
						expect(
							got.status == want.status && got.consumed == want.consumed && got.written == want.written,
							"result/inplace");
						expect(!std::memcmp(out.data() + 1, expected.data(), expected.size()), "bytes/inplace");
						expect(
							out.front() == before.front() && out[std::max(cap, input.size()) + 1] == 0x5a,
							"guard/inplace");
						// Everything after the produced prefix stays untouched.
						expect(
							std::equal(out.begin() + 1 + got.written, out.end(), before.begin() + 1 + got.written),
							"tail/inplace");
					}
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
	int correctness()
	{
		checks = failures = 0;
		seed = 0x19c3abcd;
		const char *fixtures[] = {
			"",
			"hello",
			"a+b%2Bc",
			"%",
			"%0",
			"%G0",
			"%0G",
			"%+1",
			"%-1",
			"%0x",
			"%ff%FF%00X",
			"%2520",
			"%C3%A9%F0%9F%98%80",
			"%E2%28%A1",
			"abc%41%",
			"a%20b/c?d=e"};
		for(auto s : fixtures)
			exercise(s);
		expect(percent_encode_all(u8"Я中日😀") == "%D0%AF%E4%B8%AD%E6%97%A5%F0%9F%98%80", "UTF8 known encoding");
		for(const auto &fixture : unicode_fixtures())
		{
			expect(fixture.raw.size() == 1024 && fixture.percent.size() == 3072, "UTF8 fixture extents");
			exercise(fixture.raw);
			exercise(fixture.percent);
			for(const auto *input : {&fixture.raw, &fixture.percent})
				for(const auto &v : variants)
				{
					std::vector<char> out(1026, char(0x5a));
					auto r = v.decode(input->data(), input->size(), out.data() + 1, 1024, Mode::form);
					expect(
						r.status == Status::ok && r.consumed == input->size() && r.written == 1024,
						"UTF8 exact output length");
					expect(!std::memcmp(out.data() + 1, fixture.raw.data(), 1024), "UTF8 exact expected bytes");
					expect(out.front() == 0x5a && out.back() == 0x5a, "UTF8 exact guards");
				}
#ifdef ESP_PLATFORM
			vTaskDelay(1);
#endif
		}
		std::string raw, encoded;
		const char *hex = "0123456789ABCDEF";
		for(unsigned b = 0; b < 256; ++b)
		{
			raw.push_back(char(b));
			encoded += '%';
			encoded += hex[b >> 4];
			encoded += hex[b & 15];
		}
		exercise(raw);
		exercise(encoded);
		for(const auto &variant : variants)
		{
			std::vector<char> out(256);
			auto r = variant.decode(encoded.data(), encoded.size(), out.data(), out.size(), Mode::form);
			expect(
				r.status == Status::ok && r.written == 256 && !std::memcmp(out.data(), raw.data(), 256),
				"all octets roundtrip");
			expect(variant.decode(nullptr, 0, nullptr, 0, Mode::path).status == Status::ok, "null empty");
			expect(variant.decode(nullptr, 1, out.data(), 1, Mode::form).status == Status::bad_argument, "null input");
			expect(variant.decode("x", 1, nullptr, 1, Mode::form).status == Status::bad_argument, "null output");
			char overlap[] = "abcdefg";
			auto before = std::string(overlap, sizeof overlap);
			expect(variant.decode(overlap, 6, overlap + 1, 6, Mode::path).status == Status::overlap, "overlap right");
			expect(variant.decode(overlap + 1, 6, overlap, 6, Mode::path).status == Status::overlap, "overlap left");
			expect(!std::memcmp(overlap, before.data(), sizeof overlap), "overlap unchanged");
			char exact[] = {'%', '4', '1'};
			char dst[1];
			r = variant.decode(exact, 3, dst, 1, Mode::path);
			expect(r.status == Status::ok && r.written == 1 && dst[0] == 'A', "no input terminator");
		}
		// Every possible pair after '%' including non-ASCII; checked against the byte-level contract.
		for(unsigned a = 0; a < 256; ++a)
			for(unsigned b = 0; b < 256; ++b)
			{
#ifdef ESP_PLATFORM
				if(b == 0 && a % 16 == 0)
					vTaskDelay(1);

#endif
				const std::string s = {'%', char(a), char(b)};
				std::string want;
				auto expected = oracle(s, 1, Mode::form, want);
				for(const auto &v : variants)
				{
					char out = 0x5a;
					auto r = v.decode(s.data(), 3, &out, 1, Mode::form);
					expect(
						r.status == expected.status && r.consumed == expected.consumed &&
						r.written == expected.written && (want.empty() ? out == 0x5a : out == want[0]),
						"exhaustive hex pair");
				}
			}
		for(unsigned iteration = 0; iteration < 1500; ++iteration)
		{
#ifdef ESP_PLATFORM
			if(iteration % 32 == 0)
				vTaskDelay(1);

#endif
			std::string s, bytes;
			unsigned n = random32() % 128;
			for(unsigned i = 0; i < n; ++i)
			{
				char b = char(random32());
				bytes += b;
				if(iteration % 2)
				{
					s += '%';
					s += hex[static_cast<unsigned char>(b) >> 4];
					s += hex[static_cast<unsigned char>(b) & 15];
				}
				else
					s += b;
			}
			exercise(s);
			if(iteration % 2)
				for(const auto &v : variants)
				{
					std::vector<char> out(n + 1);
					auto r = v.decode(s.data(), s.size(), out.data(), n, Mode::path);
					expect(
						r.status == Status::ok && r.written == n && !std::memcmp(out.data(), bytes.data(), n),
						"random roundtrip");
				}
		}
		std::printf("CORRECTNESS checks=%u failures=%u seed=19c3abcd\n", checks, failures);
		return int(failures);
	}

} // namespace urlbench
#ifndef ESP_PLATFORM
int main()
{
	return urlbench::correctness();
}

#endif
