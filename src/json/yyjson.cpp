// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/json.h>
#include <yyjson.h>
#include <algorithm>
#include <cstring>
#include <new>

namespace NaWServer::json
{

	// The upstream fixed pool is the only allocator. This wrapper measures the
	// furthest allocation end, including temporary allocation ranges and the
	// arena prefix. It neither allocates nor falls back to a process allocator.
	struct alignas(std::max_align_t) Document::Arena
	{
		yyjson_alc pool{};
		Document *owner{};
		void observe(void *pointer, std::size_t bytes)
		{
			if(pointer)
			{
				const auto end = static_cast<unsigned char *>(pointer) - static_cast<unsigned char *>(owner->storage_) +
				    bytes;
				owner->high_water_ = std::max(owner->high_water_, static_cast<std::size_t>(end));
			}
		}

		static void *allocate(void *context, std::size_t bytes)
		{
			auto &arena = *static_cast<Arena *>(context);
			void *pointer = arena.pool.malloc(arena.pool.ctx, bytes);
			arena.observe(pointer, bytes);
			return pointer;
		}

		static void *resize(void *context, void *pointer, std::size_t old_bytes, std::size_t bytes)
		{
			auto &arena = *static_cast<Arena *>(context);
			void *result = arena.pool.realloc(arena.pool.ctx, pointer, old_bytes, bytes);
			arena.observe(result, bytes);
			return result;
		}

		static void release(void *context, void *pointer)
		{
			auto &arena = *static_cast<Arena *>(context);
			arena.pool.free(arena.pool.ctx, pointer);
		}

	};

	bool validWorkspace(const void *storage, std::size_t bytes)
	{
		return storage && reinterpret_cast<std::uintptr_t>(storage) % alignof(std::max_align_t) == 0
		       && bytes >= sizeof(Document::Arena) + 8 * sizeof(void *);
	}

	Document::Document(void *storage, std::size_t bytes, Limits limits)
	  : storage_(storage), storage_bytes_(bytes), capacity_(limits.value_capacity), limits_(limits)
	{
	}

	// Every byte belongs to the supplied fixed arena. Resetting its allocator
	// reclaims the whole document at once; there are no external resources to
	// release. Do not dereference a former yyjson_doc during destruction: callers
	// may already have reused this workspace after invalidating the old views.
	Document::~Document() = default;

	const char *backendName()
	{
		return "yyjson";
	}

	Value Document::make(const void *native) const
	{
		const auto *node = static_cast<const yyjson_val *>(native);
		if(!node) return {};

		Type type = Type::Null;
		if(yyjson_is_obj(node)) type = Type::Object;
		else if(yyjson_is_arr(node)) type = Type::Array;
		else if(yyjson_is_str(node)) type = Type::String;
		else if(yyjson_is_bool(node)) type = Type::Boolean;
		else if(yyjson_is_raw(node))
		{
			const std::string_view raw(yyjson_get_raw(node), yyjson_get_len(node));
			type = raw.find_first_of(".eE") != std::string_view::npos ? Type::Real
			    : !raw.empty() && raw.front() == '-' ? Type::Signed : Type::Unsigned;
		}

		return Value(node, type);
	}

	Value Document::root() const
	{
		return native_document_ && error_ == Error::None
		    ? make(yyjson_doc_get_root(static_cast<yyjson_doc *>(native_document_))) : Value{};
	}

	std::string_view Document::stringStorage(Value item) const
	{
		const auto *node = static_cast<const yyjson_val *>(item.native_);
		return {yyjson_get_str(node), yyjson_get_len(node)};
	}

	std::string_view Document::numberStorage(Value item) const
	{
		const auto *node = static_cast<const yyjson_val *>(item.native_);
		return {yyjson_get_raw(node), yyjson_get_len(node)};
	}

	std::size_t Document::size(Value container) const
	{
		return container && (container.type() == Type::Object || container.type() == Type::Array)
		    ? yyjson_get_len(static_cast<const yyjson_val *>(container.native_)) : 0;
	}

	Value Document::element(Value array, std::size_t index) const
	{
		if(!array || array.type() != Type::Array) return {};

		return make(yyjson_arr_get(static_cast<const yyjson_val *>(array.native_), index));
	}

	Member Document::memberAt(Value object, std::size_t index) const
	{
		if(!object || object.type() != Type::Object) return {};

		auto iterator = yyjson_obj_iter_with(static_cast<const yyjson_val *>(object.native_));
		while(auto *key = yyjson_obj_iter_next(&iterator))
		{
			if(!index--) return {make(key), make(yyjson_obj_iter_get_val(key))};
		}
		return {};
	}

	bool Document::boolean(Value item, bool &result) const
	{
		if(!item || item.type() != Type::Boolean) return false;

		result = yyjson_get_bool(static_cast<const yyjson_val *>(item.native_));
		return true;
	}

	bool Document::visit(Value item, unsigned depth)
	{
		if(++count_ > capacity_) return fail(Error::Capacity);

		if(item.type() == Type::String && stringStorage(item).size() > limits_.string_bytes)
			return fail(Error::Capacity);

		std::uint64_t unsigned_value;
		std::int64_t signed_value;
		double real_value;
		if((item.type() == Type::Signed && !signedInteger(item, signed_value))
		    || (item.type() == Type::Unsigned && !unsignedInteger(item, unsigned_value))
		    || (item.type() == Type::Real && !real(item, real_value))) return fail(Error::Number);

		if(item.type() != Type::Object && item.type() != Type::Array) return true;

		if(depth >= limits_.depth) return fail(Error::Depth);

		if(size(item) > limits_.container_items) return fail(Error::Capacity);

		for(std::size_t index = 0; index < size(item); ++index)
		{
			if(item.type() == Type::Array)
			{
				if(!visit(element(item, index), depth + 1)) return false;

				continue;
			}

			const auto entry = memberAt(item, index);
			for(std::size_t previous = 0; previous < index; ++previous)
			{
				if(stringStorage(entry.key) == stringStorage(memberAt(item, previous).key))
					return fail(Error::DuplicateKey);
			}
			if(!visit(entry.key, depth + 1) || !visit(entry.value, depth + 1)) return false;
		}
		return true;
	}

	Error Document::parse(std::string_view input)
	{
		native_document_ = nullptr;
		input_ = input;
		count_ = 0;
		high_water_ = 0;
		error_ = Error::None;
		if(!validLimits(limits_) || input.size() > limits_.input_bytes || !validWorkspace(storage_, storage_bytes_))
		{
			fail(Error::Capacity);
			return error_;
		}

		auto *arena = ::new (storage_) Arena{};
		arena->owner = this;
		high_water_ = sizeof(Arena);
		if(!yyjson_alc_pool_init(&arena->pool, static_cast<unsigned char *>(storage_) + sizeof(Arena),
		    storage_bytes_ - sizeof(Arena)))
		{
			fail(Error::Capacity);
			return error_;
		}

		const yyjson_alc allocator{Arena::allocate, Arena::resize, Arena::release, arena};
		yyjson_read_err error{};
		// Raw number lexemes are converted by the shared checked charconv path.
		// This rejects both integer overflow and underflow-to-zero consistently;
		// yyjson otherwise owns the sole grammar/Unicode parse, without INSITU.
		native_document_ = yyjson_read_opts(const_cast<char *>(input.data()), input.size(),
		        YYJSON_READ_NUMBER_AS_RAW, &allocator, &error);
		if(!native_document_)
		{
			if(error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) fail(Error::Capacity);
			else if(error.code == YYJSON_READ_ERROR_INVALID_NUMBER) fail(Error::Number);
			else if(error.code == YYJSON_READ_ERROR_INVALID_STRING) fail(Error::Unicode);
			else if(error.code == YYJSON_READ_ERROR_DEPTH) fail(Error::Depth);
			else fail(Error::Syntax);
		}
		else visit(root(), 0);

		if(error_ != Error::None) count_ = 0;

		return error_;
	}

}
