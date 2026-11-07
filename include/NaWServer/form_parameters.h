// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/url_decode.h>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace NaWServer
{
	class ParameterValue
	{
		std::string value_;

	public:
		explicit ParameterValue(std::string value)
		  : value_(std::move(value))
		{
		}

		const std::string &value() const
		{
			return value_;
		}

	};
	using ParameterMap = std::map<std::string, ParameterValue>;
	struct ParameterParseResult
	{
		UrlDecodeStatus status{UrlDecodeStatus::Ok};
		std::size_t offset{}; // Byte offset within the raw query/form source.
	};
	// '&' separates pairs, first '=' separates key/value, missing '=' means empty.
	// Decode both key and value once; exact/case-sensitive keys, first duplicate wins.
	// Invalid escape anywhere (including duplicates) leaves destination unchanged.
	[[nodiscard]] ParameterParseResult parseFormParameters(std::string_view source, ParameterMap &destination);
	bool isFormContentType(std::string_view content_type);
} // namespace NaWServer
