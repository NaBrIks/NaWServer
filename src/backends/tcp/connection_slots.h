// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp_transport.h>
#include <limits>

namespace NaWServer::tcp::detail
{

	enum class Owner { None, Http, WebSocket, Sse, Closing };

	struct Token
	{
		std::size_t slot = std::numeric_limits<std::size_t>::max();
		std::uint64_t generation = 0;

		bool valid() const
		{
			return generation != 0;
		}

	};

	struct ConnectionSlot
	{
		Handle handle;
		std::uint64_t generation = 0;
		Owner owner = Owner::None;
		bool retired = false;
	};

	// Borrowed fixed storage. The runtime serializes table access with a short lock.
	// No method performs I/O. The I/O owner excludes concurrent operations/handoff
	// before prepareClose, closes outside the lock, then acknowledges completeClose.
	class ConnectionSlots
	{
		ConnectionSlot *slots_;
		std::size_t capacity_;

		ConnectionSlot *find(Token token, Owner owner) const
		{
			if(!slots_ || token.slot >= capacity_ || !token.valid() || owner == Owner::None)
				return nullptr;

			auto &slot = slots_[token.slot];
			if(slot.owner != owner || slot.generation != token.generation || !slot.handle.valid)
				return nullptr;

			return &slot;
		}

	public:
		ConnectionSlots(ConnectionSlot *slots, std::size_t capacity)
		  : slots_(slots), capacity_(capacity)
		{
		}

		ConnectionSlots(const ConnectionSlots &) = delete;
		ConnectionSlots &operator=(const ConnectionSlots &) = delete;

		// Transfer native ownership only on success. A full table leaves source owned.
		Token claim(Handle &source, Owner owner)
		{
			if(!slots_ || !source.valid || owner == Owner::None || owner == Owner::Closing)
				return {};

			for(std::size_t index = 0; index < capacity_; ++index)
			{
				if(slots_[index].handle.valid && slots_[index].handle.value == source.value)
					return {};
			}

			for(std::size_t index = 0; index < capacity_; ++index)
			{
				auto &slot = slots_[index];
				if(slot.owner != Owner::None || slot.retired)
					continue;

				if(slot.generation == std::numeric_limits<std::uint64_t>::max())
				{
					slot.retired = true;
					continue;
				}

				++slot.generation;
				slot.handle = source;
				slot.owner = owner;
				source = {};
				return {index, slot.generation};
			}
			return {};
		}

		// Returned native handle is borrowed and must not outlive the owner's step.
		Handle releaseHttp(Token token)
		{
			auto *slot = find(token, Owner::Http);
			if(!slot) return {};

			const auto handle = slot->handle;
			slot->handle = {};
			slot->owner = Owner::None;
			return handle;
		}

		Handle get(Token token, Owner owner) const
		{
			const auto *slot = find(token, owner);
			return slot ? slot->handle : Handle{};
		}

		bool transfer(Token token, Owner previous, Owner next)
		{
			auto *slot = find(token, previous);
			if(!slot || next == Owner::None || next == Owner::Closing || previous == Owner::Closing)
				return false;

			slot->owner = next;
			return true;
		}

		// No other owner may read/write/close after this transition. The returned
		// handle belongs only to this closing step; the slot stays reserved meanwhile.
		Handle prepareClose(Token token, Owner owner)
		{
			auto *slot = find(token, owner);
			if(!slot || owner == Owner::Closing)
				return {};

			slot->owner = Owner::Closing;
			return slot->handle;
		}

		// A retained failure pins the slot for explicit cleanup. No automatic retry.
		bool completeClose(Token token, CloseResult result)
		{
			auto *slot = find(token, Owner::Closing);
			if(!slot)
				return false;

			if(result.released)
			{
				slot->handle = {};
				slot->owner = Owner::None;
				slot->retired = slot->generation == std::numeric_limits<std::uint64_t>::max();
			}

			return true;
		}

		std::size_t active() const
		{
			std::size_t count = 0;
			if(slots_)
			{
				for(std::size_t index = 0; index < capacity_; ++index)
				{
					if(slots_[index].owner != Owner::None)
						++count;
				}
			}

			return count;
		}

	};

} // namespace NaWServer::tcp::detail
