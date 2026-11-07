// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "wire.hpp"
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <functional>
using namespace showcase;
int checks = 0;
void check(bool ok, const char *label)
{
	if(!ok)
	{
		std::cerr << "FAIL " << label << '\n';
		std::exit(1);
	}

	std::cout << "PASS " << label << '\n';
	++checks;
}

std::vector<uint8_t> read(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), {}};
}

bool accepted(const std::function<void(Writer &)> &encode)
{
	uint8_t bytes[4096];
	Writer writer(bytes, sizeof(bytes));
	encode(writer);
	const auto size = writer.size(bytes);
	Fields fields;
	return size && fields.parse(bytes, size);
}

int main(int argc, char **argv)
{
	if(argc != 2)
		return 2;

	std::string dir = argv[1];
	auto start = read(dir + "/start.cbor");
	Fields fields;
	check(fields.parse(start.data(), start.size()) && fields.version(), ".NET -> TinyCBOR command");
	volatile uint8_t disturbance[1024]{};
	for(auto &n : disturbance)
		n = 0x55;
	int duration = 0;
	char action[12];
	check(
		fields.integer("duration_ms", duration, 0) && duration == 5000 &&
		fields.string("action", action, sizeof(action)) && !std::strcmp(action, "start"),
		"parser survives parse return / stack reuse");
	auto strip = read(dir + "/strip.cbor");
	Fields rgb;
	check(rgb.parse(strip.data(), strip.size()) && rgb.version(), ".NET -> TinyCBOR RGB command");
	int color = 0;
	check(rgb.integer("color", color, 0) && color == 0xffa030, "packed RGB24 integer");
	auto echo = read(dir + "/echo.cbor");
	Fields root, payload;
	check(root.parse(echo.data(), echo.size()) && root.child("data", payload), "nested WS data map lifetime");
	uint8_t bytes[10]{};
	size_t n = sizeof(bytes);
	check(
		payload.payload("bytes", bytes, n, true) && n == 5 && bytes[0] == 0 && bytes[2] == 128 && bytes[3] == 255 &&
		bytes[4] == 0,
		"binary/NUL payload");
	auto extension = read(dir + "/extension.cbor");
	Fields future, extra;
	check(
		future.parse(extension.data(), extension.size()) && future.version() &&
		future.child("not_registered_anywhere", extra) && extra.find("температура") && extra.find("samples"),
		"unknown UTF-8 names / nested arrays without registry");
	uint8_t scalar[] = {0xa1, 0x61, 'v', 2};
	Fields text_keys;
	check(text_keys.parse(scalar, sizeof(scalar)) && text_keys.version(), "actual CBOR text key accepted");
	for(auto bad : std::vector<std::vector<uint8_t> >{
		{},
		{0xa2, 0x61, 'v', 2, 0x61, 'v', 2},
		{0xa0, 0},
		{0xbf, 0xff},
		{0xa1, 0, 2},
		{0xa1, 0, 0x9f, 0xff},
		{0xa1, 0, 0xa0},
		{0xa1, 0x18, 75, 0x61, 0xff}})
	{
		Fields f;
		check(
			!f.parse(bad.data(), bad.size()),
			"reject malformed / duplicate / trailing / indefinite / wrong types / UTF8");
	}
	for(size_t len = 0; len < echo.size(); len++)
	{
		Fields f;
		check(!f.parse(echo.data(), len), "reject every truncated WS prefix");
	}
	for(unsigned count : {16u, 17u})
		check(
			accepted(
				[&](Writer &w)
		{
			w.map(count);
			for(unsigned i = 0; i < count; i++)
				w.put("field" + std::to_string(i), i);
			w.end();
		}) == (count == 16),
			"map field bound: 16 accepted, 17 rejected");
	for(unsigned count : {32u, 33u})
		check(
			accepted(
				[&](Writer &w)
		{
			w.map(1);
			w.key("samples");
			w.array(count);
			for(unsigned i = 0; i < count; i++)
				w.value(i);
			w.end();
			w.end();
		}) == (count == 32),
			"array bound: 32 accepted, 33 rejected");
	for(unsigned count : {64u, 65u})
		check(
			accepted(
				[&](Writer &w)
		{
			w.map(1);
			w.put(std::string(count, 'x'), true);
			w.end();
		}) == (count == 64),
			"UTF-8 key byte bound: 64 accepted, 65 rejected");
	for(unsigned count : {3u, 4u})
		check(
			accepted(
				[&](Writer &w)
		{
			w.map(1);
			w.key("future");
			for(unsigned i = 0; i < count; i++)
				w.array(1);
			w.value(0);
			for(unsigned i = 0; i < count; i++)
				w.end();
			w.end();
		}) == (count == 3),
			"unknown nested values respect depth bound");
	check(
		!accepted(
			[](Writer &w)
	{
		w.map(1);
		w.put("future", std::numeric_limits<double>::infinity());
		w.end();
	}),
		"reject nonfinite unknown value");
	uint8_t invalid_text[] = {0xa1, 0x61, 'x', 0x61, 0xff};
	Fields invalid_utf8;
	check(
		!invalid_utf8.parse(invalid_text, sizeof(invalid_text)),
		"reject invalid UTF-8 text value with valid field name");
	uint8_t output[4096];
	Writer w(output, sizeof(output));
	w.map(4);
	w.put("v", 2);
	w.put("iterations", std::numeric_limits<uint64_t>::max());
	w.put("pi", std::acos(-1.0));
	w.key("data");
	w.map(1);
	w.key("bytes");
	w.bytes(bytes, n);
	w.end();
	w.end();
	auto size = w.size(output);
	check(size > 0, "bounded writer");
	std::ofstream file(dir + "/device.cbor", std::ios::binary);
	file.write(reinterpret_cast<char *>(output), size);
	file.close();
	uint8_t small[2];
	Writer bounded(small, sizeof(small));
	bounded.map(2);
	bounded.put("v", 2);
	bounded.put("text", "oversized");
	bounded.end();
	check(bounded.size(small) == 0, "writer rejects buffer overflow");
	Writer incorrect(output, sizeof(output));
	incorrect.map(2);
	incorrect.put("v", 2);
	incorrect.end();
	check(incorrect.size(output) == 0, "writer detects wrong map count");
	std::cout << "RESULT " << checks << " C++ checks passed\n";
}
