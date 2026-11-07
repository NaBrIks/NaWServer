// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "wire.hpp"
#include <cstring>
#include <limits>
namespace c6_demo
{
	namespace
	{
		constexpr unsigned command_depth_limit = 4;
		constexpr size_t command_field_limit = 16;
		constexpr size_t command_array_limit = 32;
		constexpr size_t command_key_limit = 64;
		bool read_key(CborValue &cursor, std::string_view &name)
		{
			if(!cbor_value_is_text_string(&cursor) || !cbor_value_is_length_known(&cursor))
				return false;

			const char *data = nullptr;
			size_t size = 0;
			CborValue view = cursor;
			if(cbor_value_begin_string_iteration(&view))
				return false;

			if(cbor_value_get_text_string_chunk(&view, &data, &size, nullptr) || size > command_key_limit)
				return false;

			name = std::string_view(data, size);
			return cbor_value_advance(&cursor) == CborNoError;
		}

	} // namespace
	const CborValue *Fields::find(std::string_view key) const
	{
		for(size_t i = 0; i < count_; i++)
			if(entries_[i].key == key)
				return &entries_[i].value;

		return nullptr;
	}

	bool Fields::validateValue(CborValue value, unsigned depth)
	{
		if(depth > command_depth_limit)
			return false;

		if(cbor_value_is_map(&value))
		{
			Fields child;
			return child.parseMap(value, depth);
		}

		if(cbor_value_is_array(&value))
		{
			size_t count = 0;
			if(!cbor_value_is_length_known(&value) || cbor_value_get_array_length(&value, &count) ||
			    count > command_array_limit)
				return false;

			CborValue cursor;
			if(cbor_value_enter_container(&value, &cursor))
				return false;

			for(size_t i = 0; i < count; i++)
				if(!validateValue(cursor, depth + 1) || cbor_value_advance(&cursor))
					return false;

			return cbor_value_at_end(&cursor) && cbor_value_leave_container(&value, &cursor) == CborNoError;
		}

		const auto type = cbor_value_get_type(&value);
		if(type != CborIntegerType && type != CborTextStringType && type != CborByteStringType &&
		    type != CborBooleanType && type != CborNullType && type != CborHalfFloatType && type != CborFloatType &&
		    type != CborDoubleType)
			return false;

		return !(cbor_value_is_text_string(&value) || cbor_value_is_byte_string(&value)) ||
		       cbor_value_is_length_known(&value);
	}

	bool Fields::parseMap(CborValue map, unsigned depth)
	{
		size_t count = 0;
		if(depth > command_depth_limit || !cbor_value_is_map(&map) || !cbor_value_is_length_known(&map) ||
		    cbor_value_get_map_length(&map, &count) || count > command_field_limit)
			return false;

		CborValue cursor;
		if(cbor_value_enter_container(&map, &cursor))
			return false;

		count_ = 0;
		for(size_t i = 0; i < count; i++)
		{
			std::string_view name;
			if(!read_key(cursor, name) || find(name) || !validateValue(cursor, depth + 1))
				return false;

			entries_[count_++] = {name, cursor};
			if(cbor_value_advance(&cursor))
				return false;
		}
		return cbor_value_at_end(&cursor) && cbor_value_leave_container(&map, &cursor) == CborNoError;
	}

	bool Fields::parse(const uint8_t *data, size_t size)
	{
		count_ = 0;
		if(!data || !size || size > frame_limit)
			return false;

		CborValue root;
		if(cbor_parser_init(data, size, 0, &parser_, &root) || !parseMap(root, 0) ||
		    cbor_value_validate(&root, CborValidateUtf8 | CborValidateFiniteFloatingPoint | CborValidateNoTags))
			return false;

		return cbor_value_advance(&root) == CborNoError && cbor_value_at_end(&root) &&
		       cbor_value_get_next_byte(&root) == data + size;
	}

	bool Fields::version() const
	{
		int version = 0;
		return integer("v", version, 0) && version == 2;
	}

	bool Fields::integer(std::string_view key, int &out, int fallback) const
	{
		auto *value = find(key);
		if(!value)
		{
			out = fallback;
			return true;
		}

		int64_t number;
		if(!cbor_value_is_integer(value) || cbor_value_get_int64_checked(value, &number) ||
		    number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
			return false;

		out = static_cast<int>(number);
		return true;
	}

	bool Fields::boolean(std::string_view key, bool &out) const
	{
		auto *value = find(key);
		return value && cbor_value_is_boolean(value) && cbor_value_get_boolean(value, &out) == CborNoError;
	}

	bool Fields::string(std::string_view key, char *out, size_t capacity) const
	{
		auto *value = find(key);
		size_t size = 0;
		if(!value || !cbor_value_is_text_string(value) || cbor_value_get_string_length(value, &size) ||
		    size >= capacity)
			return false;

		size_t length = capacity;
		if(cbor_value_copy_text_string(value, out, &length, nullptr) || std::memchr(out, 0, size))
			return false;

		out[size] = 0;
		return true;
	}

	bool Fields::child(std::string_view key, Fields &out) const
	{
		auto *value = find(key);
		return value && out.parseMap(*value, 0);
	}

	bool Fields::payload(std::string_view key, uint8_t *out, size_t &size, bool binary) const
	{
		auto *value = find(key);
		if(!value || (binary ? !cbor_value_is_byte_string(value) : !cbor_value_is_text_string(value)))
			return false;

		return (binary
		            ? cbor_value_copy_byte_string(value, out, &size, nullptr)
		            : cbor_value_copy_text_string(value, reinterpret_cast<char *>(out), &size, nullptr)) == CborNoError;
	}

} // namespace c6_demo
