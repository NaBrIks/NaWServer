// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/json.h>
#include "utf8.h"
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
namespace NaWServer::json
{
	using namespace detail;
	bool Document::fail(Error error)
	{
		if(error_ == Error::None) error_ = error;

		return false;
	}

	Value Document::member(Value object, std::string_view key) const
	{
		if(!object || object.type() != Type::Object) return {};

		for(std::size_t index = 0; index < size(object); ++index)
		{
			const auto entry = memberAt(object, index);
			if(equals(entry.key, key)) return entry.value;
		}
		return {};
	}

	bool Document::decode(Value value, char *buffer, std::size_t capacity, std::size_t &size) const
	{
		size = 0;
		if(!value || value.type() != Type::String) return false;

		const auto bytes = stringStorage(value);
		for(std::size_t position = 0; position < bytes.size();)
		{
			std::uint32_t scalar_value;
			char encoded[4];
			if(!scalar(bytes, position, scalar_value, NAWSERVER_JSON_BOUNDED != 0)) return false;

			const auto count = encode(scalar_value, encoded);
			if(count > capacity - size || !buffer) { size = 0; return false; }

			std::memcpy(buffer + size, encoded, count);
			size += count;
		}
		return true;
	}

	bool Document::equals(Value value, std::string_view decoded) const
	{
		if(!value || value.type() != Type::String) return false;

		const auto bytes = stringStorage(value);
		std::size_t left = 0, right = 0;
		while(left < bytes.size() && right < decoded.size())
		{
			std::uint32_t a, b;
			if(!scalar(bytes, left, a, NAWSERVER_JSON_BOUNDED != 0) || !scalar(decoded, right, b, false) || a != b)
				return false;
		}
		return left == bytes.size() && right == decoded.size();
	}

	bool Document::unsignedInteger(Value value, std::uint64_t &number) const
	{
		if(!value || value.type() != Type::Unsigned) return false;

		const auto bytes = numberStorage(value);
		const auto converted = std::from_chars(bytes.data(), bytes.data() + bytes.size(), number);
		return converted.ec == std::errc{} && converted.ptr == bytes.data() + bytes.size();
	}

	bool Document::signedInteger(Value value, std::int64_t &number) const
	{
		if(!value || (value.type() != Type::Unsigned && value.type() != Type::Signed)) return false;

		const auto bytes = numberStorage(value);
		const auto converted = std::from_chars(bytes.data(), bytes.data() + bytes.size(), number);
		return converted.ec == std::errc{} && converted.ptr == bytes.data() + bytes.size();
	}

	bool Document::real(Value value, double &number) const
	{
		if(!value || value.type() != Type::Real) return false;

		const auto bytes = numberStorage(value);
		const auto converted = std::from_chars(bytes.data(), bytes.data() + bytes.size(), number,
		        std::chars_format::general);
		return converted.ec == std::errc{} && converted.ptr == bytes.data() + bytes.size() && std::isfinite(number);
	}

	bool Writer::string(const Document &document, Value item)
	{
		if(!item || item.type() != Type::String) return valid_ = false;

		// Keep this path separate from value(): JSON-RPC IDs only need strings and
		// integers. Referencing generic value() also pulls double formatting into
		// small read-only firmware even when the application never emits a real.
		if(!append("\"")) return false;

		const auto bytes = document.stringStorage(item);
		for(std::size_t position = 0; position < bytes.size();)
		{
			std::uint32_t scalar_value;
			char encoded[4];
			if(!scalar(bytes, position, scalar_value, NAWSERVER_JSON_BOUNDED != 0)) return valid_ = false;

			const auto count = encode(scalar_value, encoded);
			// string() owns escaping; append only its quoted scalar's interior.
			char quoted[8];
			Writer temporary(quoted, sizeof(quoted));
			if(!temporary.string({encoded, count})) return valid_ = false;

			const auto fragment = temporary.fragment();
			if(!append(fragment.substr(1, fragment.size() - 2))) return false;
		}
		return append("\"");
	}

	bool Writer::value(const Document &document, Value item)
	{
		if(!item) return valid_ = false;

		switch(item.type())
		{
		case Type::String: return string(document, item);
		case Type::Signed:
		{
			std::int64_t number;
			if(!document.signedInteger(item, number)) return valid_ = false;

			return integer(number);
		}
		case Type::Unsigned:
		{
			std::uint64_t number;
			if(!document.unsignedInteger(item, number)) return valid_ = false;

			return integer(number);
		}
		case Type::Real:
		{
			double number;
			if(!document.real(item, number)) return valid_ = false;

			return real(number);
		}
		case Type::Boolean:
		{
			bool boolean_value;
			if(!document.boolean(item, boolean_value)) return valid_ = false;

			return append(boolean_value ? "true" : "false");
		}
		case Type::Null: return append("null");
		case Type::Object:
		case Type::Array:
		{
			const bool object = item.type() == Type::Object;
			if(!append(object ? "{" : "[")) return false;

			for(std::size_t index = 0; index < document.size(item); ++index)
			{
				if(index && !append(",")) return false;

				if(object)
				{
					const auto entry = document.memberAt(item, index);
					if(!value(document, entry.key) || !append(":") || !value(document, entry.value)) return false;
				}
				else if(!value(document, document.element(item, index))) return false;
			}
			return append(object ? "}" : "]");
		}
		}
		return valid_ = false;
	}

	Writer::Writer(char *buffer, std::size_t capacity)
	  : buffer_(buffer), capacity_(capacity)
	{
	}

	bool Writer::append(std::string_view s)
	{
		complete_ = false;
		if(!valid_ || s.size() > capacity_ - size_ || (!buffer_ && !s.empty())) return valid_ = false;

		if(!s.empty()) std::memcpy(buffer_ + size_, s.data(), s.size());

		size_ += s.size(); return true;
	}

	bool Writer::finish(void *storage, std::size_t storage_bytes, Limits limits)
	{
		complete_ = false;
		if(!valid_) return false;

		Document document(storage, storage_bytes, limits);
		if(document.parse(fragment()) != Error::None) return valid_ = false;

		complete_ = true;
		return true;
	}

	bool Writer::string(std::string_view s)
	{
		if(!append("\"")) return false;

		for(std::size_t p = 0; p < s.size();)
		{
			std::size_t begin = p; std::uint32_t v;
			if(!scalar(s, p, v, false)) return valid_ = false;

			if(v == '"' || v == '\\')
			{
				char bytes[2] = {'\\', static_cast<char>(v)};
				if(!append({bytes, 2})) return false;
			}
			else if(v < 32)
			{
				const char *hexadecimal = "0123456789abcdef";
				char bytes[6] = {'\\', 'u', '0', '0', hexadecimal[v >> 4], hexadecimal[v & 15]};
				if(!append({bytes, 6})) return false;
			}
			else if(!append(s.substr(begin, p - begin))) return false;
		}
		return append("\"");
	}

	bool Writer::integer(std::int64_t v)
	{
		char bytes[24]; auto r = std::to_chars(bytes, bytes + sizeof(bytes), v);
		if(r.ec != std::errc{}) return valid_ = false;

		return append({bytes, static_cast<std::size_t>(r.ptr - bytes)});
	}

	bool Writer::integer(std::uint64_t v)
	{
		char bytes[24]; auto r = std::to_chars(bytes, bytes + sizeof(bytes), v);
		if(r.ec != std::errc{}) return valid_ = false;

		return append({bytes, static_cast<std::size_t>(r.ptr - bytes)});
	}

	bool Writer::real(double v)
	{
		if(!std::isfinite(v)) return valid_ = false;

		char bytes[32]; auto r = std::to_chars(bytes, bytes + sizeof(bytes), v, std::chars_format::general,
		        std::numeric_limits<double>::max_digits10);
		if(r.ec != std::errc{}) return valid_ = false;

		std::string_view encoded(bytes, static_cast<std::size_t>(r.ptr - bytes));
		if(!append(encoded)) return false;

		// A real-valued writer preserves a real lexical representation even for 1.0.
		return encoded.find_first_of(".eE") != std::string_view::npos || append(".0");
	}

}
