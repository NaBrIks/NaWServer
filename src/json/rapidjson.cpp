// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/json.h>
#include <rapidjson/reader.h>
#include <algorithm>
#include <cstring>
#include <new>

namespace NaWServer::json
{

	struct Document::Node
	{
		std::uint16_t begin{}, end{}, next{};
		Type type = Type::Null;
	};

	// In-situ SAX parsing decodes strings into one caller-owned input copy.
	// Only compact navigation descriptors are retained; there is no second DOM.
	// The recursive reader path with raw numbers never uses RapidJSON's dynamic
	// scratch stack. Supplying this allocator prevents a hidden default allocator
	// from being selected if this pinned implementation is instantiated differently.
	struct RapidJsonNoScratchAllocator
	{
		static constexpr bool kNeedFree = false;
		bool requested = false;

		void *Malloc(std::size_t)
		{
			requested = true; return nullptr;
		}

		void *Realloc(void *, std::size_t, std::size_t)
		{
			requested = true; return nullptr;
		}

		static void Free(void *)
		{
		}

	};

	struct Document::Arena
	{
		Document &document;
		std::uint16_t parents[32]{};
		std::uint16_t items[32]{};
		unsigned depth = 0;

		bool beforeValue()
		{
			if(depth && document.nodes()[parents[depth - 1]].type == Type::Array)
			{
				if(++items[depth - 1] > document.limits_.container_items)
					return document.fail(Error::Capacity);
			}

			return true;
		}

		bool append(Type type, const char *bytes = nullptr, std::size_t length = 0)
		{
			if(document.count_ == document.capacity_) return document.fail(Error::Capacity);

			const auto begin = bytes ? static_cast<std::size_t>(bytes -
			    static_cast<const char *>(document.native_document_)) : 0;
			const auto index = document.count_++;
			::new (document.nodes() + index) Node{static_cast<std::uint16_t>(begin),
				                                  static_cast<std::uint16_t>(begin + length),
				                                  static_cast<std::uint16_t>(document.count_), type};
			document.high_water_ = std::max(document.high_water_, document.position_ + document.count_ * sizeof(Node));
			return true;
		}

		bool Null()
		{
			return beforeValue() && append(Type::Null);
		}

		bool Bool(bool value)
		{
			if(!beforeValue() || !append(Type::Boolean)) return false;

			document.nodes()[document.count_ - 1].begin = value ? 1 : 0;
			return true;
		}

		// These callbacks are unreachable with NumbersAsStrings. Rejecting them
		// makes an accidental parser-flag change fail instead of losing precision.
		bool Int(int)
		{
			return document.fail(Error::Number);
		}

		bool Uint(unsigned)
		{
			return document.fail(Error::Number);
		}

		bool Int64(std::int64_t)
		{
			return document.fail(Error::Number);
		}

		bool Uint64(std::uint64_t)
		{
			return document.fail(Error::Number);
		}

		bool Double(double)
		{
			return document.fail(Error::Number);
		}

		bool RawNumber(const char *bytes, rapidjson::SizeType length, bool)
		{
			if(!beforeValue()) return false;

			const std::string_view text(bytes, length);
			const Type type = text.find_first_of(".eE") != std::string_view::npos ? Type::Real
			    : !text.empty() && text.front() == '-' ? Type::Signed : Type::Unsigned;
			if(!append(type, bytes, length)) return false;

			const auto value = document.make(document.nodes() + document.count_ - 1);
			std::int64_t signed_value;
			std::uint64_t unsigned_value;
			double real_value;
			const bool valid = type == Type::Real ? document.real(value, real_value)
			    : type == Type::Signed ? document.signedInteger(value, signed_value)
			    : document.unsignedInteger(value, unsigned_value);
			return valid || document.fail(Error::Number);
		}

		bool String(const char *bytes, rapidjson::SizeType length, bool)
		{
			if(length > document.limits_.string_bytes) return document.fail(Error::Capacity);

			return beforeValue() && append(Type::String, bytes, length);
		}

		bool Key(const char *bytes, rapidjson::SizeType length, bool)
		{
			if(length > document.limits_.string_bytes || ++items[depth - 1] > document.limits_.container_items)
				return document.fail(Error::Capacity);

			const std::string_view key(bytes, length);
			for(std::size_t previous = parents[depth - 1] + 1; previous < document.count_;)
			{
				if(document.raw(document.nodes()[previous]) == key) return document.fail(Error::DuplicateKey);

				previous = document.nodes()[previous + 1].next;
			}
			return append(Type::String, bytes, length);
		}

		bool start(Type type)
		{
			if(depth >= document.limits_.depth) return document.fail(Error::Depth);

			if(!beforeValue() || !append(type)) return false;

			parents[depth] = static_cast<std::uint16_t>(document.count_ - 1);
			items[depth++] = 0;
			return true;
		}

		bool StartObject()
		{
			return start(Type::Object);
		}

		bool StartArray()
		{
			return start(Type::Array);
		}

		bool end()
		{
			document.nodes()[parents[--depth]].next = static_cast<std::uint16_t>(document.count_);
			return true;
		}

		bool EndObject(rapidjson::SizeType)
		{
			return end();
		}

		bool EndArray(rapidjson::SizeType)
		{
			return end();
		}

	};

	Document::Document(void *storage, std::size_t bytes, Limits limits)
	  : storage_(storage), storage_bytes_(bytes), capacity_(limits.value_capacity), limits_(limits)
	{
		static_assert(sizeof(Node) == 8, "RapidJSON descriptor accounting changed");
	}

	Document::~Document() = default;

	const char *backendName()
	{
		return "rapidjson";
	}

	bool validWorkspace(const void *storage, std::size_t bytes)
	{
		return storage && bytes >= 16 && reinterpret_cast<std::uintptr_t>(storage) % alignof(std::max_align_t) == 0;
	}

	Document::Node *Document::nodes() const
	{
		return reinterpret_cast<Node *>(static_cast<unsigned char *>(storage_) + position_);
	}

	std::string_view Document::raw(const Node &node) const
	{
		return {static_cast<const char *>(native_document_) + node.begin,
		        static_cast<std::size_t>(node.end - node.begin)};
	}

	Value Document::make(const void *node) const
	{
		return node ? Value(node, static_cast<const Node *>(node)->type) : Value{};
	}

	Value Document::root() const
	{
		return count_ ? make(nodes()) : Value{};
	}

	std::string_view Document::stringStorage(Value item) const
	{
		return raw(*static_cast<const Node *>(item.native_));
	}

	std::string_view Document::numberStorage(Value item) const
	{
		return stringStorage(item);
	}

	bool Document::boolean(Value item, bool &result) const
	{
		if(!item || item.type() != Type::Boolean) return false;

		result = static_cast<const Node *>(item.native_)->begin != 0;
		return true;
	}

	std::size_t Document::size(Value item) const
	{
		if(!item || (item.type() != Type::Object && item.type() != Type::Array)) return 0;

		const auto *node = static_cast<const Node *>(item.native_);
		std::size_t count = 0;
		for(std::size_t index = static_cast<std::size_t>(node - nodes()) + 1; index < node->next; ++count)
			index = nodes()[index + (item.type() == Type::Object ? 1 : 0)].next;
		return count;
	}

	Member Document::memberAt(Value object, std::size_t wanted) const
	{
		if(!object || object.type() != Type::Object) return {};

		const auto *node = static_cast<const Node *>(object.native_);
		for(std::size_t index = static_cast<std::size_t>(node - nodes()) + 1; index < node->next;
		    index = nodes()[index + 1].next)
		{
			if(!wanted--) return {make(nodes() + index), make(nodes() + index + 1)};
		}
		return {};
	}

	Value Document::element(Value array, std::size_t wanted) const
	{
		if(!array || array.type() != Type::Array) return {};

		const auto *node = static_cast<const Node *>(array.native_);
		for(std::size_t index = static_cast<std::size_t>(node - nodes()) + 1; index < node->next;
		    index = nodes()[index].next)
		{
			if(!wanted--) return make(nodes() + index);
		}
		return {};
	}

	Error Document::parse(std::string_view input)
	{
		input_ = input;
		count_ = 0;
		position_ = 0;
		error_ = Error::None;
		high_water_ = 0;
		native_document_ = nullptr;
		if(!validLimits(limits_) || input.size() > limits_.input_bytes || input.size() > 65535
		    || !validWorkspace(storage_, storage_bytes_))
		{
			fail(Error::Capacity);
			return error_;
		}

		// The source remains immutable. The arena copy is shared by all decoded
		// strings and raw numeric views, including embedded decoded NUL bytes.
		position_ = (input.size() + 1 + alignof(Node) - 1) / alignof(Node) * alignof(Node);
		if(position_ > storage_bytes_ || storage_bytes_ - position_ < sizeof(Node))
		{
			fail(Error::Capacity);
			return error_;
		}

		capacity_ = std::min(limits_.value_capacity, (storage_bytes_ - position_) / sizeof(Node));
		native_document_ = storage_;
		if(!input.empty()) std::memmove(storage_, input.data(), input.size());

		static_cast<char *>(storage_)[input.size()] = '\0';
		high_water_ = position_;
		Arena handler{*this};
		RapidJsonNoScratchAllocator allocator;
		rapidjson::GenericReader<rapidjson::UTF8<>, rapidjson::UTF8<>, RapidJsonNoScratchAllocator> reader(&allocator,
		    0);
		rapidjson::InsituStringStream stream(static_cast<char *>(storage_));
		constexpr unsigned flags = rapidjson::kParseInsituFlag | rapidjson::kParseValidateEncodingFlag
		    | rapidjson::kParseNumbersAsStringsFlag;
		const auto result = reader.Parse<flags>(stream, handler);
		if(!result && error_ == Error::None)
		{
			switch(result.Code())
			{
			case rapidjson::kParseErrorStringUnicodeEscapeInvalidHex:
			case rapidjson::kParseErrorStringUnicodeSurrogateInvalid:
			case rapidjson::kParseErrorStringEscapeInvalid:
			case rapidjson::kParseErrorStringInvalidEncoding:
				fail(Error::Unicode);
				break;
			case rapidjson::kParseErrorNumberTooBig:
			case rapidjson::kParseErrorNumberMissFraction:
			case rapidjson::kParseErrorNumberMissExponent:
				fail(Error::Number);
				break;
			default: fail(Error::Syntax); break;
			}
		}

		if(error_ == Error::None && (stream.Tell() != input.size() || allocator.requested)) fail(Error::Syntax);

		if(error_ != Error::None) count_ = 0;

		return error_;
	}

} // namespace NaWServer::json
