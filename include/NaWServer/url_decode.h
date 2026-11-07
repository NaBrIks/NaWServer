// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstddef>

#define NAWSERVER_URL_DECODE_LOOKUP 1
#define NAWSERVER_URL_DECODE_FROM_CHARS 2
#ifndef NAWSERVER_URL_DECODE_BACKEND
#define NAWSERVER_URL_DECODE_BACKEND NAWSERVER_URL_DECODE_LOOKUP
#endif
#if NAWSERVER_URL_DECODE_BACKEND != NAWSERVER_URL_DECODE_LOOKUP &&                                                     \
	NAWSERVER_URL_DECODE_BACKEND != NAWSERVER_URL_DECODE_FROM_CHARS
#error "NAWSERVER_URL_DECODE_BACKEND must be 1 (Lookup) or 2 (from_chars)"
#endif

namespace NaWServer
{
	enum class UrlDecodeMode
	{
		Path,
		Form
	};
	enum class UrlDecodeStatus
	{
		Ok,
		InvalidEscape,
		NoSpace,
		BadArgument,
		Overlap
	};
	struct UrlDecodeResult
	{
		UrlDecodeStatus status;
		std::size_t consumed;
		std::size_t written;
	};
	// Byte API: no terminator, raw/decoded NUL preserved, exact in-place supported.
	// On error only the complete written prefix is valid; other overlap is rejected.
	[[nodiscard]] UrlDecodeResult
	urlDecode(const char *source, std::size_t length, char *destination, std::size_t capacity, UrlDecodeMode mode);
	constexpr const char *urlDecodeBackend()
	{
#if NAWSERVER_URL_DECODE_BACKEND == NAWSERVER_URL_DECODE_LOOKUP
		return "lookup";
#else
		return "from_chars";
#endif
	}

} // namespace NaWServer
