// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/api_keys.h>
#include <algorithm>
#include <cstring>
#include <limits>
#if NAWSERVER_API_KEYS
namespace NaWServer::keys
{
	namespace
	{
		std::uint32_t checksum(const std::uint8_t *data, std::size_t size)
		{
			std::uint32_t value = 0xffffffffu;
			while(size--) { value ^= *data++; for(unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u))); }
			return ~value;
		}
		void put(std::uint8_t *data, std::uint64_t value, unsigned size)
		{
			for(unsigned i = 0; i < size; ++i) { data[i] = static_cast<std::uint8_t>(value); value >>= 8; }
		}
		std::uint64_t get(const std::uint8_t *data, unsigned size)
		{
			std::uint64_t value = 0;
			for(unsigned i = 0; i < size; ++i) value |= std::uint64_t(data[i]) << (8 * i);
			return value;
		}
		int hex(char value)
		{
			if(value >= '0' && value <= '9') return value - '0';
			if(value >= 'a' && value <= 'f') return value - 'a' + 10;
			return -1;
		}
	}
	void Store::encode(const Slots &slots, std::uint64_t generation, std::uint8_t *record) const
	{
		std::memset(record, 0, RecordBytes);
		std::memcpy(record, "NAWKEYS1", 8); put(record + 8, 1, 2); put(record + 10, slots.size(), 2); put(record + 12, generation, 8);
		for(std::size_t i = 0; i < slots.size(); ++i)
		{
			auto *out = record + 20 + i * 62;
			out[0] = slots[i].metadata.active ? 1 : 0; put(out + 1, slots[i].metadata.revision, 4);
			std::memcpy(out + 5, slots[i].metadata.label.data(), 25); std::memcpy(out + 30, slots[i].secret.data(), 32);
		}
		put(record + RecordBytes - 4, checksum(record, RecordBytes - 4), 4);
	}
	bool Store::decode(const std::uint8_t *record, std::size_t size, Slots &slots, std::uint64_t &generation) const
	{
		if(size != RecordBytes || std::memcmp(record, "NAWKEYS1", 8) || get(record + 8, 2) != 1 ||
		    get(record + 10, 2) != slots.size() || get(record + RecordBytes - 4, 4) != checksum(record, RecordBytes - 4)) return false;
		generation = get(record + 12, 8);
		if(!generation) return false;
		for(std::size_t i = 0; i < slots.size(); ++i)
		{
			const auto *in = record + 20 + i * 62;
			if(in[0] > 1 || in[29]) return false;
			slots[i].metadata.active = in[0] != 0; slots[i].metadata.revision = static_cast<std::uint32_t>(get(in + 1, 4));
			std::memcpy(slots[i].metadata.label.data(), in + 5, 25); std::memcpy(slots[i].secret.data(), in + 30, 32);
		}
		return true;
	}
	Status Store::load()
	{
		Slots best{}; std::uint64_t best_generation = 0; unsigned best_index = 1; bool invalid = false, failed = false;
		for(unsigned i = 0; i < 2; ++i)
		{
			std::array<std::uint8_t, RecordBytes> record{}; std::size_t size = 0;
			const auto status = storage_.read(i, record.data(), record.size(), size);
			if(status == ReadStatus::Error) { failed = true; continue; }
			if(status == ReadStatus::Missing) continue;
			Slots candidate{}; std::uint64_t generation = 0;
			if(!decode(record.data(), size, candidate, generation)) { invalid = true; continue; }
			if(generation > best_generation) { best = candidate; best_generation = generation; best_index = i; }
		}
		if(!best_generation && (failed || invalid)) { ready_ = false; return failed ? Status::IoError : Status::Corrupt; }
		slots_ = best; generation_ = best_generation; current_ = best_index; ready_ = true;
		return Status::Ok;
	}
	Status Store::persist(const Slots &slots)
	{
		if(!ready_ || generation_ == std::numeric_limits<std::uint64_t>::max()) return Status::Invalid;
		std::array<std::uint8_t, RecordBytes> record{}, readback{};
		encode(slots, generation_ + 1, record.data()); const auto target = 1u - current_;
		if(!storage_.write(target, record.data(), record.size())) { ready_ = false; return Status::IoError; }
		std::size_t size = 0;
		if(storage_.read(target, readback.data(), readback.size(), size) != ReadStatus::Ok || size != record.size() ||
		    std::memcmp(record.data(), readback.data(), size)) { ready_ = false; return Status::IoError; }
		slots_ = slots; ++generation_; current_ = target;
		return Status::Ok;
	}
	Status Store::replace(std::size_t slot, std::string_view label, Secret &secret)
	{
		secret.clear();
		if(!ready_ || slot >= slots_.size() || label.empty() || label.size() > 24 || slots_[slot].metadata.revision == std::numeric_limits<std::uint32_t>::max()) return Status::Invalid;
		for(unsigned char c : label) if(c < 32 || c == 127) return Status::Invalid;
		Slots next = slots_;
		if(!random_.fill(next[slot].secret.data(), next[slot].secret.size())) return Status::RandomError;
		// An all-zero provider failure must not silently create a predictable key.
		if(std::all_of(next[slot].secret.begin(), next[slot].secret.end(), [](auto b) { return b == 0; })) return Status::RandomError;
		for(std::size_t other = 0; other < slots_.size(); ++other) if(slots_[other].metadata.active && next[slot].secret == slots_[other].secret) return Status::RandomError;
		next[slot].metadata.active = true; next[slot].metadata.label.fill(0);
		std::memcpy(next[slot].metadata.label.data(), label.data(), label.size()); ++next[slot].metadata.revision;
		const auto status = persist(next);
		if(status != Status::Ok) return status;
		constexpr char digits[] = "0123456789abcdef";
		for(std::size_t i = 0; i < next[slot].secret.size(); ++i) { secret.value[i * 2] = digits[next[slot].secret[i] >> 4]; secret.value[i * 2 + 1] = digits[next[slot].secret[i] & 15]; }
		secret.slot = slot; return Status::Ok;
	}
	Status Store::create(std::string_view label, Secret &secret)
	{
		secret.clear(); if(!ready_) return Status::Invalid;
		for(std::size_t i = 0; i < slots_.size(); ++i) if(!slots_[i].metadata.active) return replace(i, label, secret);
		return Status::Full;
	}
	Status Store::rotate(std::size_t slot, Secret &secret)
	{
		secret.clear(); if(slot >= slots_.size() || !slots_[slot].metadata.active) return Status::NotFound;
		const auto label = slots_[slot].metadata.label;
		return replace(slot, label.data(), secret);
	}
	Status Store::revoke(std::size_t slot)
	{
		if(!ready_) return Status::Invalid;
		if(slot >= slots_.size() || !slots_[slot].metadata.active) return Status::NotFound;
		Slots next = slots_; next[slot].metadata.active = false; next[slot].secret.fill(0);
		return persist(next);
	}
	std::optional<std::size_t> Store::authenticate(std::string_view secret) const
	{
		if(!ready_ || secret.size() != 64) return {};
		std::array<std::uint8_t, 32> bytes{};
		for(std::size_t i = 0; i < bytes.size(); ++i) { const auto high = hex(secret[i * 2]), low = hex(secret[i * 2 + 1]); if(high < 0 || low < 0) return {}; bytes[i] = static_cast<std::uint8_t>((high << 4) | low); }
		std::optional<std::size_t> matched;
		for(std::size_t i = 0; i < slots_.size(); ++i)
		{
			unsigned difference = 0;
			for(std::size_t j = 0; j < bytes.size(); ++j) difference |= bytes[j] ^ slots_[i].secret[j];
			if(difference == 0 && slots_[i].metadata.active) matched = i;
		}
		return matched;
	}
	Metadata Store::metadata(std::size_t slot) const { return ready_ && slot < slots_.size() ? slots_[slot].metadata : Metadata{}; }
	std::string_view Store::identity(std::size_t slot)
	{
		constexpr std::string_view names[]{"naw-api-slot-0", "naw-api-slot-1", "naw-api-slot-2", "naw-api-slot-3", "naw-api-slot-4", "naw-api-slot-5", "naw-api-slot-6", "naw-api-slot-7", "naw-api-slot-8", "naw-api-slot-9", "naw-api-slot-10", "naw-api-slot-11", "naw-api-slot-12", "naw-api-slot-13", "naw-api-slot-14", "naw-api-slot-15"};
		return slot < NAWSERVER_API_KEYS_MAX ? names[slot] : std::string_view{};
	}
}
#endif
