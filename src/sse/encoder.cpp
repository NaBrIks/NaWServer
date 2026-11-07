// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/detail/sse/encoder.hpp>
#include "../detail/utf8.h"
#include <charconv>
#include <cstring>

namespace NaWServer::sse
{
	bool valid_utf8(std::string_view s)
	{
		return NaWServer::detail::validUtf8(s);
	}

	bool valid_field(std::string_view s)
	{
		return s.find_first_of(std::string_view("\r\n\0", 3)) == s.npos && valid_utf8(s);
	}

	namespace
	{
		// Saturating count gives a transactional capacity check before writing.
		struct Writer
		{
			Frame *out;
			std::size_t size = 0;
			void add(std::string_view s)
			{
				if(size > frame_capacity || s.size() > frame_capacity - size)
				{
					size = frame_capacity + 1;
					return;
				}

				if(out && !s.empty())
					std::memcpy(out->bytes.data() + size, s.data(), s.size());

				size += s.size();
			}

			void field(std::string_view key, std::string_view value)
			{
				add(key);
				add(value);
				add("\n");
			}

			void lines(std::string_view key, std::string_view text)
			{
				std::size_t start = 0;
				for(std::size_t i = 0; i < text.size(); ++i)
				{
					if(text[i] != '\r' && text[i] != '\n')
						continue;

					field(key, text.substr(start, i - start));
					if(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
						++i;

					start = i + 1;
				}
				field(key, text.substr(start)); // includes empty/trailing data line
				add("\n");
			}

		};
		void render(Writer &w, const Event &e)
		{
			if(e.name)
				w.field("event: ", *e.name);

			if(e.id)
				w.field("id: ", *e.id);

			if(e.retry_ms)
			{
				char digits[10];
				auto end = std::to_chars(digits, digits + sizeof(digits), *e.retry_ms).ptr;
				w.field("retry: ", {digits, static_cast<std::size_t>(end - digits)});
			}

			w.lines("data: ", e.data);
		}

	} // namespace
	EncodeResult encode(const Event &e, Frame &out)
	{
		if(e.data.size() > frame_capacity || (e.name && e.name->size() > frame_capacity) ||
		    (e.id && e.id->size() > frame_capacity))
			return EncodeResult::TooLarge;

		if((e.name && !valid_field(*e.name)) || (e.id && !valid_field(*e.id)))
			return EncodeResult::InvalidField;

		if(!valid_utf8(e.data))
			return EncodeResult::InvalidUtf8;

		Writer count{nullptr};
		render(count, e);
		if(count.size > frame_capacity)
			return EncodeResult::TooLarge;

		Writer write{&out};
		render(write, e);
		out.size = write.size;
		return EncodeResult::Ok;
	}

	EncodeResult comment(std::string_view text, Frame &out)
	{
		if(text.size() > frame_capacity)
			return EncodeResult::TooLarge;

		if(!valid_utf8(text))
			return EncodeResult::InvalidUtf8;

		Writer count{nullptr};
		count.lines(": ", text);
		if(count.size > frame_capacity)
			return EncodeResult::TooLarge;

		Writer write{&out};
		write.lines(": ", text);
		out.size = write.size;
		return EncodeResult::Ok;
	}

} // namespace NaWServer::sse
