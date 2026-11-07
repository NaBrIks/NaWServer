// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <string_view>

namespace NaWServer::detail
{
	// Protocol ASCII only: independent of locale; bytes >= 128 remain unchanged.
	constexpr unsigned char toLowerAscii(unsigned char character)
	{
		return character >= 'A' && character <= 'Z' ? static_cast<unsigned char>(character + ('a' - 'A')) : character;
	}

	constexpr bool asciiEqualIgnoreCase(std::string_view left, std::string_view right)
	{
		if(left.size() != right.size())
			return false;

		for(std::size_t i = 0; i < left.size(); ++i)
			if(toLowerAscii(static_cast<unsigned char>(left[i])) != toLowerAscii(static_cast<unsigned char>(right[i])))
				return false;

		return true;
	}

} // namespace NaWServer::detail
