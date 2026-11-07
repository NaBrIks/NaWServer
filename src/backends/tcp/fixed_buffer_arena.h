// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/memory_manager.h>
#include <array>
#include <NaWServer/detail/synchronization.h>

namespace NaWServer::tcp::detail
{

	// A small fixed extent table for one client's stream storage. The parent
	// lends one backing block on upgrade; other clients cannot consume it while held.
	class FixedBufferArena
	{
		struct Extent { std::size_t offset = 0, bytes = 0; };
		MemoryBlock storage_;
		std::array<Extent, 16> extents_{};
		NaWServer::detail::Mutex mutex_;
		static void *allocate(void *context, std::size_t bytes)
		{
			auto &self = *static_cast<FixedBufferArena *>(context);
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(self.mutex_);
			if(!bytes || bytes > self.storage_.size()) return nullptr;

			std::size_t slot = 0;
			while(slot < self.extents_.size() && self.extents_[slot].bytes)
				++slot;
			if(slot == self.extents_.size()) return nullptr;

			std::size_t position = 0;
			for(;;)
			{
				if(position > self.storage_.size() || bytes > self.storage_.size() - position) return nullptr;

				bool overlap = false;
				for(const auto &extent : self.extents_)
					if(extent.bytes && position < extent.offset + extent.bytes && extent.offset < position + bytes)
					{
						const auto end = extent.offset + extent.bytes;
						const auto alignment = alignof(std::max_align_t);
						position = end + (alignment - end % alignment) % alignment;
						overlap = true;
						break;
					}

				if(!overlap) break;
			}
			self.extents_[slot] = {position, bytes};
			return self.storage_.data() + position;
		}

		static void release(void *context, void *pointer, std::size_t)
		{
			auto &self = *static_cast<FixedBufferArena *>(context);
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(self.mutex_);
			for(auto &extent : self.extents_)
				if(extent.bytes && self.storage_.data() + extent.offset == pointer)
				{
					extent = {};
					return;
				}
		}

	public:
		bool initialize(MemoryManager &pool, std::size_t bytes)
		{
			storage_ = pool.acquire(bytes);
			return static_cast<bool>(storage_);
		}

		void reset()
		{
			extents_ = {}; storage_.reset();
		}

		BufferAllocator allocator()
		{
			return {this, allocate, release};
		}

	};

} // namespace NaWServer::tcp::detail
