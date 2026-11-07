// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/buffer_memory.h>

namespace NaWServer
{
	class MemoryManager;
	// Move-only lease. The manager outlives all leases; reset returns fixed
	// blocks to the pool and immediately frees additional dynamic allocations.
	class MemoryBlock
	{
		friend class MemoryManager;
		MemoryManager *owner_ = nullptr;
		std::size_t slot_ = 0;
		void *data_ = nullptr;
		std::size_t capacity_ = 0;
		MemoryBlock(MemoryManager *owner, std::size_t slot, void *data, std::size_t capacity)
		  : owner_(owner), slot_(slot), data_(data), capacity_(capacity)
		{
		}

	public:
		MemoryBlock() = default;
		~MemoryBlock()
		{
			reset();
		}

		MemoryBlock(const MemoryBlock &) = delete;
		MemoryBlock &operator=(const MemoryBlock &) = delete;
		MemoryBlock(MemoryBlock &&other) noexcept
		{
			*this = std::move(other);
		}

		MemoryBlock &operator=(MemoryBlock &&other) noexcept
		{
			if(this != &other)
			{
				reset();
				owner_ = std::exchange(other.owner_, nullptr);
				slot_ = other.slot_;
				data_ = std::exchange(other.data_, nullptr);
				capacity_ = std::exchange(other.capacity_, 0);
			}

			return *this;
		}

		void reset();
		std::uint8_t *data() const
		{
			return static_cast<std::uint8_t *>(data_);
		}

		std::size_t size() const
		{
			return capacity_;
		}

		explicit operator bool() const {
			return data_ != nullptr;
		}
	};
	struct MemoryPoolOptions
	{
		std::size_t initial = 0, maximum = 1, block_bytes = 4096;
	};
	struct MemoryPoolStats
	{
		std::size_t fixed_blocks = 0, allocated_blocks = 0, active_blocks = 0;
		std::size_t allocated_bytes = 0, active_bytes = 0, peak_bytes = 0, rejected = 0;
		MemoryBlockReason blocked = MemoryBlockReason::Ready;
	};
	// Small per-group manager over the shared byte ledger/platform allocator.
	// No HTTP, client key, singleton, task or platform policy is embedded here.
	class MemoryManager
	{
		friend class MemoryBlock;
		struct Slot { void *data = nullptr; std::size_t bytes = 0; bool busy = false; };
		detail::BufferArray<Slot> slots_;
		detail::BufferAccount *account_ = nullptr;
		MemoryPoolOptions options_;
		MemoryPoolStats stats_;
		mutable NaWServer::detail::Mutex mutex_;
		void release(std::size_t slot)
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			auto &item = slots_[slot];
			--stats_.active_blocks;
			stats_.active_bytes -= item.bytes;
			item.busy = false;
			if(slot >= options_.initial)
			{
				account_->free(item.data, item.bytes);
				--stats_.allocated_blocks;
				stats_.allocated_bytes -= item.bytes;
				item = {};
			}

			stats_.blocked = MemoryBlockReason::Ready;
		}

	public:
		MemoryManager() = default;
		MemoryManager(const MemoryManager &) = delete;
		MemoryManager &operator=(const MemoryManager &) = delete;
		~MemoryManager()
		{
			reset();
		}

		void reset()
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			if(stats_.active_blocks) std::abort();

			for(std::size_t index = 0; index < slots_.size(); ++index)
				if(slots_[index].data) account_->free(slots_[index].data, slots_[index].bytes);

			if(account_) account_->release();

			account_ = nullptr;
			slots_.reset();
			stats_ = {};
		}

		bool initialize(detail::BufferPool &parent, MemoryPoolOptions options)
		{
			if(account_ || !parent.account() || !options.maximum || options.initial > options.maximum ||
			    !options.block_bytes || !slots_.allocate(parent, options.maximum)) return false;

			account_ = parent.account();
			account_->retain();
			options_ = options;
			for(std::size_t index = 0; index < options.initial; ++index)
			{
				auto &item = slots_[index];
				item.data = account_->allocate(options.block_bytes);
				if(!item.data) return false;

				item.bytes = options.block_bytes;
				++stats_.fixed_blocks;
				++stats_.allocated_blocks;
				stats_.allocated_bytes += item.bytes;
			}
			stats_.peak_bytes = stats_.allocated_bytes;
			return true;
		}

		MemoryBlockReason readiness(std::size_t bytes)
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			if(!account_ || !bytes || bytes > options_.block_bytes) return MemoryBlockReason::ByteLimit;

			for(std::size_t index = 0; index < slots_.size(); ++index)
			{
				const auto &item = slots_[index];
				if(item.busy) continue;

				stats_.blocked = item.data ? MemoryBlockReason::Ready : account_->readiness(bytes);
				return stats_.blocked;
			}
			return stats_.blocked = MemoryBlockReason::SlotLimit;
		}

		MemoryBlock acquire(std::size_t bytes)
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			if(!account_ || !bytes || bytes > options_.block_bytes)
			{
				++stats_.rejected;
				stats_.blocked = MemoryBlockReason::ByteLimit;
				return {};
			}

			for(std::size_t index = 0; index < slots_.size(); ++index)
			{
				auto &item = slots_[index];
				if(item.busy) continue;

				if(!item.data)
				{
					if(account_->readiness(bytes) != MemoryBlockReason::Ready ||
					    !(item.data = account_->allocate(bytes)))
					{
						++stats_.rejected;
						stats_.blocked = account_->stats().blocked;
						return {};
					}

					item.bytes = bytes;
					++stats_.allocated_blocks;
					stats_.allocated_bytes += bytes;
					if(stats_.allocated_bytes > stats_.peak_bytes) stats_.peak_bytes = stats_.allocated_bytes;
				}

				item.busy = true;
				++stats_.active_blocks;
				stats_.active_bytes += item.bytes;
				stats_.blocked = MemoryBlockReason::Ready;
				return {this, index, item.data, item.bytes};
			}
			++stats_.rejected;
			stats_.blocked = MemoryBlockReason::SlotLimit;
			return {};
		}

		MemoryPoolStats stats() const
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_); return stats_;
		}

	};
	inline void MemoryBlock::reset()
	{
		if(owner_) owner_->release(slot_);

		owner_ = nullptr;
		data_ = nullptr;
		capacity_ = 0;
	}

}
