// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstddef>
#include <cstdint>

namespace urlbench
{
	enum class Mode
	{
		path,
		form
	};
	enum class Status
	{
		ok,
		invalid_escape,
		no_space,
		bad_argument,
		overlap
	};
	struct Result
	{
		Status status;
		size_t consumed;
		size_t written;
	};
	using Decoder = Result (*)(const char *, size_t, char *, size_t, Mode);
	Result direct(const char *, size_t, char *, size_t, Mode);
	Result lookup(const char *, size_t, char *, size_t, Mode);
	Result standard(const char *, size_t, char *, size_t, Mode);
	Result spans(const char *, size_t, char *, size_t, Mode);
	struct Variant
	{
		const char *name;
		Decoder decode;
	};
	inline constexpr Variant variants[] = {
		{"direct", direct},
		{"lookup", lookup},
		{"from_chars", standard},
		{"spans", spans}};
	// Tests run unchanged on host and device; zero means PASS.
	int correctness();
} // namespace urlbench
