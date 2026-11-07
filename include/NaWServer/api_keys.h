// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#ifndef NAWSERVER_API_KEYS
#define NAWSERVER_API_KEYS 1
#endif
#ifndef NAWSERVER_API_KEYS_MAX
#define NAWSERVER_API_KEYS_MAX 4
#endif
namespace NaWServer::keys
{
#if NAWSERVER_API_KEYS
	static_assert(NAWSERVER_API_KEYS_MAX > 0 && NAWSERVER_API_KEYS_MAX <= 16, "API key slots must be 1..16");
	enum class Status { Ok, NotFound, Invalid, Full, IoError, Corrupt, RandomError };
	enum class ReadStatus { Missing, Ok, Error };
	class Storage
	{
	public:
		virtual ~Storage() = default;
		virtual ReadStatus read(unsigned record, void *bytes, std::size_t capacity, std::size_t &size) = 0;
		// Success means durable publication. The core reads back before changing its live table.
		virtual bool write(unsigned record, const void *bytes, std::size_t size) = 0;
	};
	class Random
	{
	public:
		virtual ~Random() = default;
		virtual bool fill(std::uint8_t *bytes, std::size_t size) = 0;
	};
	struct Metadata
	{
		bool active = false;
		std::array<char, 25> label{};
		std::uint32_t revision = 0;
	};
	struct Secret
	{
		std::array<char, 65> value{};
		std::size_t slot = NAWSERVER_API_KEYS_MAX;
		void clear() { value.fill(0); slot = NAWSERVER_API_KEYS_MAX; }
	};
	// Calls are serialized by the application owner, including authenticate.
	// Storage/random callbacks must outlive this object. Secrets never appear in metadata.
	class Store
	{
		struct Slot { Metadata metadata; std::array<std::uint8_t, 32> secret{}; };
		using Slots = std::array<Slot, NAWSERVER_API_KEYS_MAX>;
		Storage &storage_;
		Random &random_;
		Slots slots_{};
		std::uint64_t generation_ = 0;
		unsigned current_ = 1;
		bool ready_ = false;
		Status persist(const Slots &slots);
		bool decode(const std::uint8_t *record, std::size_t size, Slots &slots, std::uint64_t &generation) const;
		void encode(const Slots &slots, std::uint64_t generation, std::uint8_t *record) const;
		Status replace(std::size_t slot, std::string_view label, Secret &secret);
	public:
		static constexpr std::size_t RecordBytes = 24 + NAWSERVER_API_KEYS_MAX * 62;
		Store(Storage &storage, Random &random) : storage_(storage), random_(random) {}
		Status load();
		Status create(std::string_view label, Secret &secret);
		Status rotate(std::size_t slot, Secret &secret);
		Status revoke(std::size_t slot);
		std::optional<std::size_t> authenticate(std::string_view secret) const;
		Metadata metadata(std::size_t slot) const;
		static std::string_view identity(std::size_t slot);
		bool ready() const { return ready_; }
		std::uint64_t generation() const { return generation_; }
	};
#endif
}
