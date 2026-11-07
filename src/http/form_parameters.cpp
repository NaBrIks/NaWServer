// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

// Length-delimited query/form parsing, designed from the documented contract.
#include <NaWServer/form_parameters.h>
#include "detail/ascii.h"

namespace NaWServer
{
	namespace
	{
		ParameterParseResult decodeComponent(std::string_view input, std::size_t offset, std::string &output)
		{
			output = std::string(input);
			auto result = urlDecode(output.data(), output.size(), output.data(), output.size(), UrlDecodeMode::Form);
			if(result.status != UrlDecodeStatus::Ok)
				return {result.status, offset + result.consumed};

			output.resize(result.written);
			return {};
		}

	} // namespace
	ParameterParseResult parseFormParameters(std::string_view source, ParameterMap &destination)
	{
		ParameterMap parsed;
		std::size_t start = 0;
		while(start < source.size())
		{
			auto end = source.find('&', start);
			if(end == std::string_view::npos)
				end = source.size();

			if(end != start)
			{
				const auto pair = source.substr(start, end - start);
				auto equal = pair.find('=');
				const auto key_size = equal == std::string_view::npos ? pair.size() : equal;
				const auto value_start = equal == std::string_view::npos ? pair.size() : equal + 1;
				std::string key, value;
				auto result = decodeComponent(pair.substr(0, key_size), start, key);
				if(result.status != UrlDecodeStatus::Ok)
					return result;

				result = decodeComponent(pair.substr(value_start), start + value_start, value);
				if(result.status != UrlDecodeStatus::Ok)
					return result;

				parsed.try_emplace(std::move(key), std::move(value));
			}

			if(end == source.size())
				break;

			start = end + 1;
		}
		destination.swap(parsed);
		return {};
	}

	bool isFormContentType(std::string_view content_type)
	{
		content_type = content_type.substr(0, content_type.find(';'));
		while(!content_type.empty() && (content_type.front() == ' ' || content_type.front() == '\t'))
			content_type.remove_prefix(1);
		while(!content_type.empty() && (content_type.back() == ' ' || content_type.back() == '\t'))
			content_type.remove_suffix(1);
		return detail::asciiEqualIgnoreCase(content_type, "application/x-www-form-urlencoded");
	}

} // namespace NaWServer
