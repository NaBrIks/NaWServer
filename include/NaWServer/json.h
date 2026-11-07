// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/json_config.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace NaWServer::json
{
	enum class Type : std::uint8_t { Object, Array, String, Signed, Unsigned, Real, Boolean, Null };
	enum class Error : std::uint8_t { None, Syntax, Unicode, Number, Depth, Capacity, DuplicateKey };
	struct Limits
	{
		std::size_t input_bytes = NAWSERVER_JSON_INPUT_BYTES;
		std::size_t string_bytes = NAWSERVER_JSON_STRING_BYTES;
		std::size_t container_items = NAWSERVER_JSON_CONTAINER_ITEMS;
		unsigned depth = NAWSERVER_JSON_DEPTH;
		std::size_t value_capacity = NAWSERVER_JSON_VALUE_CAPACITY;
	};
	inline bool validLimits(const Limits &limits)
	{
		return limits.input_bytes > 0 && limits.input_bytes <= 65535 && limits.string_bytes > 0
		       && limits.string_bytes <= 65535 && limits.container_items > 0 && limits.container_items <= 65535
		       && limits.depth > 0 && limits.depth <= 32 && limits.value_capacity > 0 && limits.value_capacity <= 65535;
	}

	class Document;
	class Writer;
	// Borrowed backend value; invalid after parse, destruction or workspace reuse.
	// Source offsets, native DOM types and pointer arithmetic are not this API.
	class Value
	{
		const void *native_ = nullptr;
		Type type_ = Type::Null;
		Value(const void *native, Type type)
		  : native_(native), type_(type)
		{
		}

		friend class Document;
	public:
		Value() = default;
		explicit operator bool() const {
			return native_ != nullptr;
		}
		Type type() const
		{
			return type_;
		}

	};
	struct Member { Value key; Value value; };
	class Document
	{
		struct Node;
		struct Arena;
		void *storage_;
		std::size_t storage_bytes_, capacity_, count_{};
		Limits limits_;
		std::string_view input_;
		Error error_ = Error::None;
		std::size_t position_{};
		void *native_document_ = nullptr;
		std::size_t high_water_ = 0;
		Node *nodes() const;
		std::string_view raw(const Node &node) const;
		std::string_view stringStorage(Value value) const;
		std::string_view numberStorage(Value value) const;
		Value make(const void *native) const;
		bool visit(Value value, unsigned depth);
		void whitespace();
		bool value(unsigned depth);
		bool string();
		bool number();
		bool fail(Error error);
		friend class Writer;
		friend bool validWorkspace(const void *, std::size_t);
	public:
		// Storage is caller-owned, aligned to max_align_t. Bounded uses 8 bytes
		// per value. yyjson uses only this fixed allocator arena, without fallback.
		// Input is immutable; bounded borrows bytes, yyjson stores its native
		// decoded DOM in the arena. Neither builds an additional normalized DOM.
		// RapidJSON SAX decodes one arena input copy and retains compact indexes;
		// its internal scratch allocator is disabled for the selected parse flags.
		Document(void *storage, std::size_t storage_bytes, Limits limits = {});
		~Document();
		Document(const Document &) = delete;
		Document &operator=(const Document &) = delete;
		Error parse(std::string_view input);
		Value root() const;
		Value member(Value object, std::string_view key) const;
		Member memberAt(Value object, std::size_t index) const;
		Value element(Value array, std::size_t index) const;
		std::size_t size(Value container) const;
		bool decode(Value value, char *buffer, std::size_t capacity, std::size_t &size) const;
		bool equals(Value value, std::string_view decoded) const;
		bool unsignedInteger(Value value, std::uint64_t &number) const;
		bool signedInteger(Value value, std::int64_t &number) const;
		bool real(Value value, double &number) const;
		bool boolean(Value value, bool &boolean) const;
		std::size_t count() const
		{
			return count_;
		}

		std::size_t workspaceHighWaterBytes() const
		{
			return high_water_;
		}

	};
	const char *backendName();
	bool validWorkspace(const void *storage, std::size_t storage_bytes);
	// Shared bounded text encoder. finish() validates using the selected reader;
	// unfinished fragments cannot be published as completed view().
	class Writer
	{
		char *buffer_;
		std::size_t capacity_, size_{};
		bool valid_ = true;
		bool complete_ = false;
	public:
		Writer(char *buffer, std::size_t capacity);
		bool append(std::string_view validated_json);
		bool string(std::string_view decoded_utf8);
		// Stream a borrowed string directly, without a decoded temporary buffer.
		// String-only callers need not link generic value/real serialization.
		bool string(const Document &document, Value value);
		bool integer(std::int64_t value);
		bool integer(std::uint64_t value);
		bool real(double value);
		bool value(const Document &document, Value value);
		bool finish(void *storage, std::size_t storage_bytes, Limits limits = {});
		bool valid() const
		{
			return valid_;
		}

		std::string_view fragment() const
		{
			return valid_ ? std::string_view(buffer_, size_) : std::string_view{};
		}

		std::string_view view() const
		{
			return complete_ && valid_ ? fragment() : std::string_view{};
		}

	};
}
