#pragma once
#include <NaWServer/request_body.h>
#include <array>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace rp_demo
{
	// Demo protocol only: bounded CBOR with UTF-8 field names, no key registry.
	class CborWriter
	{
		std::uint8_t *bytes_;
		std::size_t capacity_, used_ = 0;
		bool valid_ = true;
		void byte(std::uint8_t value)
		{
			if(used_ < capacity_) bytes_[used_++] = value;
			else valid_ = false;
		}
		void head(unsigned type, std::uint64_t value)
		{
			if(value < 24) byte((type << 5) | value);
			else
			{
				unsigned count = value <= 0xff ? 1 : value <= 0xffff ? 2 : value <= 0xffffffff ? 4 : 8;
				byte((type << 5) | (count == 1 ? 24 : count == 2 ? 25 : count == 4 ? 26 : 27));
				for(unsigned remaining = count; remaining; --remaining) byte(value >> ((remaining - 1) * 8));
			}
		}
	public:
		CborWriter(std::uint8_t *bytes, std::size_t capacity) : bytes_(bytes), capacity_(capacity) {}
		void map(std::size_t count) { head(5, count); }
		void array(std::size_t count) { head(4, count); }
		void null() { byte(0xf6); }
		void value(bool value) { byte(value ? 0xf5 : 0xf4); }
		void value(std::string_view value)
		{
			head(3, value.size());
			for(const auto character : value) byte(static_cast<std::uint8_t>(character));
		}
		void value(const char *value) { this->value(std::string_view(value)); }
		template<class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
		void value(T value)
		{
			if constexpr(std::is_signed_v<T>)
			{
				if(value < 0) { head(1, static_cast<std::uint64_t>(-(value + 1))); return; }
			}
			head(0, static_cast<std::uint64_t>(value));
		}
		void value(double value)
		{
			std::uint64_t bits;
			std::memcpy(&bits, &value, sizeof(bits));
			byte(0xfb);
			for(unsigned remaining = 8; remaining; --remaining) byte(bits >> ((remaining - 1) * 8));
		}
		void raw(NaWServer::ByteView value)
		{
			for(std::size_t index = 0; index < value.size(); ++index) byte(value.data()[index]);
		}
		template<class T> void put(const char *key, T value) { this->value(key); this->value(value); }
		NaWServer::ByteView view() const { return valid_ ? NaWServer::ByteView(bytes_, used_) : NaWServer::ByteView{}; }
	};

	class CborFields
	{
		struct Item { std::string_view key; NaWServer::ByteView encoded; std::string_view text; std::int64_t integer; unsigned type; };
		std::array<Item, 16> items_{};
		std::size_t count_ = 0;
		static bool scan(const std::uint8_t *&cursor, const std::uint8_t *end, unsigned &type,
			std::uint64_t &argument, std::string_view &text, unsigned depth = 0)
		{
			if(cursor == end || depth > 5) return false;
			const auto first = *cursor++;
			type = first >> 5;
			const unsigned additional = first & 31;
			argument = additional;
			text = {};
			if(additional >= 24)
			{
				if(additional > 27) return false;
				const auto count = 1u << (additional - 24);
				if(std::size_t(end - cursor) < count) return false;
				argument = 0;
				for(unsigned index = 0; index < count; ++index) argument = (argument << 8) | *cursor++;
			}
			if(type == 2 || type == 3)
			{
				if(argument > std::size_t(end - cursor)) return false;
				text = {reinterpret_cast<const char *>(cursor), std::size_t(argument)};
				cursor += argument;
			}
			if(type == 4 || type == 5)
			{
				if(argument > 32) return false;
				for(std::size_t index = 0; index < argument * (type == 5 ? 2 : 1); ++index)
				{
					unsigned nested; std::uint64_t value; std::string_view ignored;
					if(!scan(cursor, end, nested, value, ignored, depth + 1)) return false;
				}
			}
			return type != 6;
		}
		const Item *find(std::string_view key) const
		{
			for(std::size_t index = 0; index < count_; ++index) if(items_[index].key == key) return &items_[index];
			return nullptr;
		}
	public:
		bool parse(NaWServer::ByteView bytes)
		{
			count_ = 0;
			if(bytes.empty() || (bytes.data()[0] >> 5) != 5 || (bytes.data()[0] & 31) > items_.size()) return false;
			const unsigned count = bytes.data()[0] & 31;
			const auto *cursor = bytes.data() + 1;
			const auto *end = bytes.data() + bytes.size();
			for(unsigned index = 0; index < count; ++index)
			{
				unsigned type; std::uint64_t argument; std::string_view key, text;
				if(!scan(cursor, end, type, argument, key) || type != 3) return false;
				for(std::size_t previous = 0; previous < count_; ++previous) if(items_[previous].key == key) return false;
				const auto *start = cursor;
				if(!scan(cursor, end, type, argument, text)) return false;
				items_[count_++] = {key, {start, std::size_t(cursor - start)}, text,
					type == 1 ? -1 - static_cast<std::int64_t>(argument) : static_cast<std::int64_t>(argument), type};
			}
			return cursor == end;
		}
		std::int64_t integer(const char *key, std::int64_t fallback) const
		{
			const auto *item = find(key);
			return item && item->type <= 1 ? item->integer : fallback;
		}
		bool boolean(const char *key, bool fallback) const
		{
			const auto *item = find(key);
			return item && item->type == 7 && (item->integer == 20 || item->integer == 21) ? item->integer == 21 : fallback;
		}
		std::string_view text(const char *key) const
		{
			const auto *item = find(key);
			return item && item->type == 3 ? item->text : std::string_view{};
		}
		NaWServer::ByteView encoded(const char *key) const
		{
			const auto *item = find(key);
			return item ? item->encoded : NaWServer::ByteView{};
		}
	};
}
