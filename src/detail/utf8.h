// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <cstdint>
#include <string_view>

namespace NaWServer::detail
{

	// Full-string RFC 3629 validation, shared by SSE and WebSocket. U+0000 is
	// valid here; field-specific restrictions remain with the owning protocol.
	inline bool validUtf8(std::string_view text)
	{
		std::uint32_t codepoint = 0, minimum = 0;
		unsigned remaining = 0;
		for(unsigned char character : text)
		{
			if(remaining)
			{
				if((character & 0xc0) != 0x80)
					return false;

				codepoint = (codepoint << 6) | (character & 0x3f);
				if(--remaining == 0 && (codepoint < minimum || codepoint > 0x10ffff ||
				    (codepoint >= 0xd800 && codepoint <= 0xdfff)))
					return false;
			}
			else if(character < 0x80)
			{
				continue;
			}
			else if(character >= 0xc2 && character <= 0xdf)
			{
				codepoint = character & 0x1f;
				minimum = 0x80;
				remaining = 1;
			}
			else if(character >= 0xe0 && character <= 0xef)
			{
				codepoint = character & 0x0f;
				minimum = 0x800;
				remaining = 2;
			}
			else if(character >= 0xf0 && character <= 0xf4)
			{
				codepoint = character & 0x07;
				minimum = 0x10000;
				remaining = 3;
			}
			else
				return false;
		}
		return remaining == 0;
	}

} // namespace NaWServer::detail
