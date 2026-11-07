// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/detail/sse/response_bridge.hpp>
#include <cstring>
#include <cstdint>

namespace NaWServer::sse
{
	namespace
	{
		bool same(std::string_view a, std::string_view b)
		{
			if(a.size() != b.size())
				return false;

			for(std::size_t i = 0; i < a.size(); ++i)
			{
				auto lower = [](unsigned char c)
					{
						return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
					};
				if(lower(a[i]) != lower(b[i]))
					return false;
			}
			return true;
		}

		bool bounded(const char *input, std::string_view &out)
		{
			if(!input)
				return false;

			std::size_t size = 0;
			while(size <= frame_capacity && input[size])
				++size;
			if(size > frame_capacity)
				return false;

			out = {input, size};
			return true;
		}

		bool valid_name(std::string_view name)
		{
			if(name.empty())
				return false;

			for(unsigned char c : name)
			{
				if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
					continue;

				if(!c || !std::strchr("!#$%&'*+-.^_`|~", c))
					return false;
			}
			return true;
		}

		bool valid_value(std::string_view value)
		{
			for(unsigned char c : value)
				if((c < 32 && c != '\t') || c == 127)
					return false;

			return true;
		}

	} // namespace
	SendResult encode_headers(const Header *headers, std::size_t count, Frame &out)
	{
		if(count > Response::max_headers)
			return SendResult::HeaderLimit;

		if(count && !headers)
			return SendResult::InvalidArgument;

		constexpr std::string_view
		    prefix = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n";
		constexpr std::string_view no_cache = "Cache-Control: no-cache\r\n";
		const auto output_begin = reinterpret_cast<std::uintptr_t>(&out);
		const auto output_end = output_begin + sizeof(out);
		const auto overlaps = [&](const void *ptr, std::size_t size)
			{
				const auto begin = reinterpret_cast<std::uintptr_t>(ptr);
				return begin < output_end && (begin >= output_begin || size > output_begin - begin);
			};
		if(count && overlaps(headers, count * sizeof(Header)))
			return SendResult::InvalidArgument;

		// Validate and size everything before writing, without a second 768-byte
		// Frame on the HTTPD stack. Inputs must remain disjoint from the output.
		std::size_t needed = prefix.size() + 2;
		bool cache = false;
		for(std::size_t i = 0; i < count; ++i)
		{
			std::string_view name, value;
			if(!headers[i].name || !headers[i].value)
				return SendResult::InvalidArgument;

			if(!bounded(headers[i].name, name) || !bounded(headers[i].value, value))
				return SendResult::BufferTooSmall;

			if(overlaps(name.data(), name.size() + 1) || overlaps(value.data(), value.size() + 1))
				return SendResult::InvalidArgument;

			if(!valid_name(name) || !valid_value(value) || same(name, "Content-Length") || same(name, "Content-Type") ||
			    same(name, "Transfer-Encoding") || same(name, "Connection") || same(name, "Content-Encoding") ||
			    same(name, "Trailer"))
				return SendResult::InvalidArgument;

			for(std::size_t j = 0; j < i; ++j)
			{
				std::string_view prior;
				if(!bounded(headers[j].name, prior) || same(name, prior))
					return SendResult::InvalidArgument;
			}
			cache = cache || same(name, "Cache-Control");
			const auto added = name.size() + value.size() + 4;
			if(added > frame_capacity - needed)
				return SendResult::BufferTooSmall;

			needed += added;
		}
		if(!cache && no_cache.size() > frame_capacity - needed)
			return SendResult::BufferTooSmall;

		out.size = 0;
		const auto append = [&](std::string_view value)
			{
				if(!value.empty())
					std::memcpy(out.bytes.data() + out.size, value.data(), value.size());

				out.size += value.size();
			};
		append(prefix);
		for(std::size_t i = 0; i < count; ++i)
		{
			append(headers[i].name);
			append(": ");
			append(headers[i].value);
			append("\r\n");
		}
		if(!cache)
			append(no_cache);

		append("\r\n");
		return SendResult::Ok;
	}

} // namespace NaWServer::sse
