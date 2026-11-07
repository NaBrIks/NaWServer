// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/json.h>
#include "utf8.h"
#include <algorithm>
#include <new>
namespace NaWServer::json
{
	using namespace detail;
	struct Document::Node
	{
		std::uint16_t begin{}, end{}, next{};
		Type type = Type::Null;
	};

	Document::Document(void *storage, std::size_t bytes, Limits limits)
	  : storage_(storage), storage_bytes_(bytes), capacity_(std::min(bytes / sizeof(Node), limits.value_capacity)),
	    limits_(limits)
	{
		static_assert(sizeof(Node) == 8, "Bounded workspace accounting changed");
		if(reinterpret_cast<std::uintptr_t>(storage) % alignof(std::max_align_t)) capacity_ = 0;
	}

	Document::~Document() = default;
	Document::Node *Document::nodes() const
	{
		return static_cast<Node *>(storage_);
	}

	std::string_view Document::raw(const Node &node) const
	{
		return input_.substr(node.begin, node.end - node.begin);
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

		result = raw(*static_cast<const Node *>(item.native_)) == "true";
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

	const char *backendName()
	{
		return "nawserver_bounded";
	}

	bool validWorkspace(const void *storage, std::size_t bytes)
	{
		return storage && bytes >= 8 && reinterpret_cast<std::uintptr_t>(storage) % alignof(std::max_align_t) == 0;
	}

	void Document::whitespace()
	{
		while(position_ < input_.size())
		{
			char c = input_[position_];
			if(c != ' ' && c != '\t' && c != '\r' && c != '\n') break;

			++position_;
		}
	}

	bool Document::string()
	{
		::new (nodes() + count_) Node{};
		high_water_ = std::max(high_water_, (count_ + 1) * sizeof(Node));
		std::size_t begin = ++position_;
		std::size_t decoded_size = 0;
		while(position_ < input_.size() && input_[position_] != '"')
		{
			std::uint32_t v;
			if(!scalar(input_, position_, v, true)) return fail(Error::Unicode);

			decoded_size += v < 0x80 ? 1 : v < 0x800 ? 2 : v < 0x10000 ? 3 : 4;
			if(decoded_size > limits_.string_bytes) return fail(Error::Capacity);
		}
		if(position_ == input_.size()) return fail(Error::Syntax);

		Node &t = nodes()[count_++];
		t = {static_cast<std::uint16_t>(begin), static_cast<std::uint16_t>(position_),
			 static_cast<std::uint16_t>(count_), Type::String};
		++position_;
		return true;
	}

	bool Document::number()
	{
		std::size_t begin = position_;
		bool negative = input_[position_] == '-';
		if(negative && ++position_ == input_.size()) return fail(Error::Number);

		if(input_[position_] == '0') ++position_;
		else
		{
			if(input_[position_] < '1' || input_[position_] > '9') return fail(Error::Number);

			while(position_ < input_.size() && digit(input_[position_]))
				++position_;
		}

		bool floating = false;
		if(position_ < input_.size() && input_[position_] == '.')
		{
			floating = true; ++position_;
			std::size_t start = position_;
			while(position_ < input_.size() && digit(input_[position_]))
				++position_;
			if(start == position_) return fail(Error::Number);
		}

		if(position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E'))
		{
			floating = true; ++position_;
			if(position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;

			std::size_t start = position_;
			while(position_ < input_.size() && digit(input_[position_]))
				++position_;
			if(start == position_) return fail(Error::Number);
		}

		Node t{static_cast<std::uint16_t>(begin), static_cast<std::uint16_t>(position_),
			   static_cast<std::uint16_t>(count_ + 1),
			   floating ? Type::Real : negative ? Type::Signed : Type::Unsigned};
		std::int64_t si; std::uint64_t ui; double fp;
		if(!(floating ? real(Value(&t, t.type), fp) : negative ? signedInteger(Value(&t, t.type),
		    si) : unsignedInteger(Value(&t, t.type), ui))) return fail(Error::Number);

		nodes()[count_++] = t;
		return true;
	}

	bool Document::value(unsigned depth)
	{
		whitespace();
		if(position_ == input_.size()) return fail(Error::Syntax);

		if(count_ == capacity_) return fail(Error::Capacity);

		::new (nodes() + count_) Node{};
		high_water_ = std::max(high_water_, (count_ + 1) * sizeof(Node));
		char c = input_[position_];
		if(c == '"') return string();

		if(c == '-' || digit(c)) return number();

		std::size_t index = count_++;
		Node &t = nodes()[index];
		t.begin = static_cast<std::uint16_t>(position_);
		if(c == '{' || c == '[')
		{
			if(depth >= limits_.depth) return fail(Error::Depth);

			t.type = c == '{' ? Type::Object : Type::Array;
			char close = c == '{' ? '}' : ']';
			++position_; whitespace();
			std::size_t items = 0;
			if(position_ < input_.size() && input_[position_] != close)
			{
				while(true)
				{
					if(++items > limits_.container_items) return fail(Error::Capacity);

					if(c == '{')
					{
						whitespace();
						if(position_ == input_.size() || input_[position_] != '"') return fail(Error::Syntax);

						if(count_ == capacity_) return fail(Error::Capacity);

						std::size_t key = count_;
						if(!string()) return false;

						// Compare decoded scalars without allocating/copying string keys.
						for(std::size_t previous = index + 1; previous < key;)
						{
							auto left = raw(nodes()[previous]); auto right = raw(nodes()[key]);
							std::size_t a = 0, b = 0;
							bool equal = true;
							while(a < left.size() && b < right.size())
							{
								std::uint32_t va, vb;
								if(!scalar(left, a, va, true) || !scalar(right, b, vb, true) || va != vb)
								{
									equal = false; break;
								}
							}
							if(equal && a == left.size() && b == right.size()) return fail(Error::DuplicateKey);

							previous = nodes()[previous + 1].next;
						}
						whitespace();
						if(position_ == input_.size() || input_[position_++] != ':') return fail(Error::Syntax);
					}

					if(!value(depth + 1)) return false;

					whitespace();
					if(position_ == input_.size()) return fail(Error::Syntax);

					if(input_[position_] != ',') break;

					++position_;
				}
			}

			if(position_ == input_.size() || input_[position_++] != close) return fail(Error::Syntax);
		}
		else
		{
			std::string_view literal;
			if(c == 't') { literal = "true"; t.type = Type::Boolean; }
			else if(c == 'f') { literal = "false"; t.type = Type::Boolean; }
			else if(c == 'n') { literal = "null"; t.type = Type::Null; }
			else return fail(Error::Syntax);

			if(input_.substr(position_, literal.size()) != literal) return fail(Error::Syntax);

			position_ += literal.size();
		}

		t.end = static_cast<std::uint16_t>(position_);
		t.next = static_cast<std::uint16_t>(count_);
		return true;
	}

	Error Document::parse(std::string_view input)
	{
		input_ = input; count_ = 0; position_ = 0; error_ = Error::None; high_water_ = 0;
		if(input.size() > limits_.input_bytes || input.size() > 65535 || !validLimits(limits_) || !nodes() ||
		    !capacity_)
			fail(Error::Capacity);
		else if(value(0))
		{
			whitespace();
			if(position_ != input.size()) fail(Error::Syntax);
		}

		if(error_ != Error::None) count_ = 0;

		return error_;
	}

}
