// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include "cbor.h"
#include <string_view>
namespace showcase
{
	constexpr size_t frame_limit = 4096;
	// Fixed output buffer supplied by the caller. No heap and no indefinite containers.
	class Writer
	{
		CborEncoder root_{}, stack_[8]{};
		unsigned depth_ = 0;
		int error_ = 0;
		CborEncoder *current()
		{
			return depth_ ? &stack_[depth_ - 1] : &root_;
		}

	public:
		Writer(uint8_t *data, size_t capacity)
		{
			cbor_encoder_init(&root_, data, capacity, 0);
		}

		void map(size_t n)
		{
			if(depth_ >= 8)
			{
				error_ = 1;
				return;
			}

			auto *parent = current();
			error_ |= cbor_encoder_create_map(parent, &stack_[depth_++], n);
		}

		void array(size_t n)
		{
			if(depth_ >= 8)
			{
				error_ = 1;
				return;
			}

			auto *parent = current();
			error_ |= cbor_encoder_create_array(parent, &stack_[depth_++], n);
		}

		void end()
		{
			if(!depth_)
			{
				error_ = 1;
				return;
			}

			auto *child = &stack_[--depth_];
			error_ |= cbor_encoder_close_container_checked(current(), child);
		}

		void key(std::string_view name)
		{
			error_ |= cbor_encode_text_string(current(), name.data(), name.size());
		}

		void value(const char *s)
		{
			error_ |= cbor_encode_text_stringz(current(), s);
		}

		void value(bool b)
		{
			error_ |= cbor_encode_boolean(current(), b);
		}

		template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0> void value(T n)
		{
			if constexpr(std::is_signed_v<T>)
			{
				error_ |= cbor_encode_int(current(), n);
			}
			else
				error_ |= cbor_encode_uint(current(), n);
		}

		void value(double n)
		{
			error_ |= cbor_encode_double(current(), n);
		}

		void null()
		{
			error_ |= cbor_encode_null(current());
		}

		void bytes(const uint8_t *p, size_t n)
		{
			error_ |= cbor_encode_byte_string(current(), p, n);
		}

		void text(const char *p, size_t n)
		{
			error_ |= cbor_encode_text_string(current(), p, n);
		}

		template <class T> void put(std::string_view k, T v)
		{
			key(k);
			value(v);
		}

		size_t size(const uint8_t *buffer) const
		{
			return !error_ && !depth_ ? cbor_encoder_get_buffer_size(&root_, buffer) : 0;
		}

	};
	// Bounded text-key command maps. Unknown names need no registry.
	// Values borrow the body exclusively during the callback.
	class Fields
	{
		CborParser parser_{};
		struct Entry
		{
			std::string_view key;
			CborValue value;
		};
		Entry entries_[16]{};
		size_t count_ = 0;
		bool parseMap(CborValue map, unsigned depth);
		static bool validateValue(CborValue value, unsigned depth);

	public:
		Fields() = default;
		Fields(const Fields &) = delete;
		Fields &operator=(const Fields &) = delete;
		bool parse(const uint8_t *data, size_t size);
		const CborValue *find(std::string_view key) const;
		bool version() const;
		bool integer(std::string_view key, int &out, int fallback) const;
		bool boolean(std::string_view key, bool &out) const;
		bool string(std::string_view key, char *out, size_t capacity) const;
		bool child(std::string_view key, Fields &out) const;
		bool payload(std::string_view key, uint8_t *out, size_t &size, bool binary) const;
	};
} // namespace showcase
