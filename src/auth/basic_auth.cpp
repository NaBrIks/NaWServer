// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

// Bounded comparison of an RFC 7617 token, decoded group by group.
#include "auth/basic_auth.h"
#include "detail/ascii.h"
namespace NaWServer
{
	namespace
	{
		int digit(unsigned char c)
		{
			if(c >= 'A' && c <= 'Z')
				return c - 'A';

			if(c >= 'a' && c <= 'z')
				return c - 'a' + 26;

			if(c >= '0' && c <= '9')
				return c - '0' + 52;

			return c == '+' ? 62 : c == '/' ? 63 : -1;
		}

	} // namespace
	bool verifyBasicAuthorization(std::string_view input, std::string_view user, std::string_view password)
	{
		if(user.empty() || password.empty() || user.find(':') != user.npos || input.size() < 7)
			return false;

		if(!detail::asciiEqualIgnoreCase(input.substr(0, 5), "basic"))
			return false;

		if(input[5] != ' ')
			return false;

		input.remove_prefix(6);
		while(!input.empty() && input.front() == ' ')
			input.remove_prefix(1);
		if(input.empty() || input.size() % 4)
			return false;

		const std::size_t expected = user.size() + 1 + password.size();
		if(expected < user.size() || expected < password.size())
			return false;

		// Decode exactly the credential size, rejecting prefix/suffix matches.
		std::size_t position = 0;
		unsigned difference = 0;
		auto compare = [&](unsigned char byte)
			{
				if(position >= expected)
				{
					difference |= 1;
					++position;
					return;
				}

				const unsigned char wanted = position < user.size() ? user[position]
				                         : position == user.size() ? ':'
				                                                   : password[position - user.size() - 1];
				difference |= byte ^ wanted;
				++position;
			};
		for(std::size_t i = 0; i < input.size(); i += 4)
		{
			const int a = digit(input[i]), b = digit(input[i + 1]);
			if(a < 0 || b < 0)
				return false;

			const bool pad2 = input[i + 2] == '=', pad3 = input[i + 3] == '=';
			const int c = pad2 ? 0 : digit(input[i + 2]);
			const int d = pad3 ? 0 : digit(input[i + 3]);
			if(c < 0 || d < 0 || (pad2 && !pad3) || ((pad2 || pad3) && i + 4 != input.size()) || (pad2 && (b & 15)) ||
			    (!pad2 && pad3 && (c & 3)))
				return false;

			compare(static_cast<unsigned char>((a << 2) | (b >> 4)));
			if(!pad2)
				compare(static_cast<unsigned char>((b << 4) | (c >> 2)));

			if(!pad3)
				compare(static_cast<unsigned char>((c << 6) | d));
		}
		return position == expected && difference == 0;
	}

} // namespace NaWServer
