// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstring>
#include <string_view>

namespace NaWServer::detail
{
	// Syntax only. Reserved names, duplicate policy and storage limits belong to
	// the response/stream that owns the headers.
	inline bool isValidHeaderName(std::string_view name)
	{
		if(name.empty())
			return false;

		for(unsigned char character : name)
		{
			if((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			    (character >= '0' && character <= '9'))
				continue;

			if(!character || !std::strchr("!#$%&'*+-.^_`|~", character))
				return false;
		}
		return true;
	}

	inline bool isValidHeaderValue(std::string_view value)
	{
		for(unsigned char character : value)
			if((character < 32 && character != '\t') || character == 127)
				return false;

		return true;
	}

} // namespace NaWServer::detail
