// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <NaWServer/detail/synchronization.h>
#include <utility>

namespace NaWServer
{
	struct BufferAvailability
	{
		std::size_t free_bytes = 0, largest_block_bytes = 0;
	};
	enum class MemoryBlockReason : std::uint8_t { Ready, ByteLimit, LowMemory, AllocationFailure, SlotLimit };

	// Allocations must be max_align_t aligned, thread safe and non-throwing.
	// The allocator/context outlive every buffer, including disconnected WS handles.
	struct BufferAllocator
	{
		void *context = nullptr;
		void *(*allocate)(void *, std::size_t) = nullptr;
		void (*release)(void *, void *, std::size_t) = nullptr;
		// Optional platform probe. No OS-wide free-memory assumption on hosts.
		bool (*available)(void *, BufferAvailability &) = nullptr;
		void *(*allocate_guarded)(void *, std::size_t, std::size_t) = nullptr;
		bool valid() const
		{
			return allocate && release;
		}

	};

	enum class BufferGroup { Tcp, Sse };
	BufferAllocator defaultBufferAllocator(BufferGroup group) noexcept;
	std::size_t defaultBufferLimit(BufferGroup group) noexcept;

	struct BufferMemoryOptions
	{
		// Empty selects this platform's group default. No global malloc override.
		BufferAllocator allocator;
		// Requested payload/array bytes, including unused reserved pool capacity.
		// Excludes allocator overhead, small control objects, task stacks and SDK RAM.
		std::size_t max_bytes = 64 * 1024;
		std::size_t min_free_bytes = 0;
	};

	struct BufferMemoryStats
	{
		std::size_t used_bytes = 0, peak_bytes = 0, limit_bytes = 0;
		std::size_t budget_rejections = 0, allocation_failures = 0;
		MemoryBlockReason blocked = MemoryBlockReason::Ready;
	};

	namespace detail
	{
		// Intrusive lifetime: a retained WS handle can outlive the stopped runtime.
		// Its buffers must not release bytes through a destroyed budget ledger.
		class BufferAccount
		{
			std::atomic<std::size_t> references_{1}, used_{0}, peak_{0}, rejected_{0}, failed_{0};
			std::atomic<MemoryBlockReason> blocked_{MemoryBlockReason::Ready};
			NaWServer::detail::Mutex allocation_mutex_;
			const std::size_t floor_;
		public:
			const BufferAllocator allocator;
			const std::size_t limit;
			BufferAccount(BufferAllocator value, std::size_t bytes, std::size_t floor = 0)
			  : floor_(floor), allocator(value), limit(bytes)
			{
			}

			MemoryBlockReason readiness(std::size_t bytes)
			{
				MemoryBlockReason reason = MemoryBlockReason::Ready;
				const auto used = used_.load();
				if(!bytes || used > limit || bytes > limit - used) reason = MemoryBlockReason::ByteLimit;

				BufferAvailability free;
				if(reason == MemoryBlockReason::Ready && floor_ &&
				    (!allocator.available || !allocator.available(allocator.context, free) ||
				    free.free_bytes < floor_ || bytes > free.free_bytes - floor_ || bytes > free.largest_block_bytes))
					reason = MemoryBlockReason::LowMemory;

				blocked_.store(reason);
				return reason;
			}

			void retain()
			{
				references_.fetch_add(1, std::memory_order_relaxed);
			}

			void release()
			{
				if(references_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
			}

			void *allocate(std::size_t bytes)
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(allocation_mutex_);
				if(readiness(bytes) != MemoryBlockReason::Ready) { ++rejected_; return nullptr; }

				auto used = used_.load(std::memory_order_relaxed);
				do
				{
					if(!bytes || used > limit || bytes > limit - used)
					{
						rejected_.fetch_add(1, std::memory_order_relaxed);
						return nullptr;
					}
				} while(!used_.compare_exchange_weak(used, used + bytes, std::memory_order_acq_rel));
				void *storage = floor_ && allocator.allocate_guarded ?
				    allocator.allocate_guarded(allocator.context, bytes, floor_) : allocator.allocate(allocator.context,
				        bytes);
				if(storage && reinterpret_cast<std::uintptr_t>(storage) % alignof(std::max_align_t))
				{
					allocator.release(allocator.context, storage, bytes);
					storage = nullptr;
				}

				if(!storage)
				{
					used_.fetch_sub(bytes, std::memory_order_release);
					failed_.fetch_add(1, std::memory_order_relaxed);
					blocked_.store(MemoryBlockReason::AllocationFailure);
					return nullptr;
				}

				auto peak = peak_.load(std::memory_order_relaxed);
				const auto current = used + bytes;
				while(peak < current && !peak_.compare_exchange_weak(peak, current, std::memory_order_relaxed))
				{
				}
				return storage;
			}

			void free(void *storage, std::size_t bytes)
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(allocation_mutex_);
				allocator.release(allocator.context, storage, bytes);
				used_.fetch_sub(bytes, std::memory_order_release);
				(void)readiness(1);
			}

			BufferMemoryStats stats() const
			{
				return {used_.load(), peak_.load(), limit, rejected_.load(), failed_.load(), blocked_.load()};
			}

		};

		class BufferPool
		{
			BufferAccount *account_ = nullptr;
			BufferMemoryStats last_stats_;
		public:
			BufferPool() = default;
			BufferPool(const BufferPool &) = delete;
			BufferPool &operator=(const BufferPool &) = delete;
			~BufferPool()
			{
				if(account_)
					account_->release();
			}

			void reset()
			{
				if(account_)
				{
					last_stats_ = account_->stats();
					account_->release();
				}

				account_ = nullptr;
			}

			bool initialize(BufferMemoryOptions options, BufferGroup group)
			{
				if(account_) return true;

				if(!options.max_bytes) return false;

				if(!options.allocator.allocate && !options.allocator.release)
					options.allocator = defaultBufferAllocator(group);

				if(!options.allocator.valid()) return false;

				if(options.min_free_bytes && !options.allocator.available) return false;

				account_ = new(std::nothrow) BufferAccount(options.allocator, options.max_bytes,
				        options.min_free_bytes);
				return account_ != nullptr;
			}

			BufferAccount *account() const
			{
				return account_;
			}

			BufferMemoryStats stats() const
			{
				return account_ ? account_->stats() : last_stats_;
			}

		};

		template <class T> class BufferArray
		{
			static_assert(alignof(T) <= alignof(std::max_align_t), "Over-aligned buffers need a different allocator");
			BufferAccount *account_ = nullptr;
			T *storage_ = nullptr;
			std::size_t count_ = 0;
		public:
			BufferArray() = default;
			BufferArray(BufferPool &pool, std::size_t count)
			{
				allocate(pool, count);
			}

			BufferArray(const BufferArray &) = delete;
			BufferArray &operator=(const BufferArray &) = delete;
			~BufferArray()
			{
				reset();
			}

			bool allocate(BufferPool &pool, std::size_t count)
			{
				if(storage_ || !pool.account() || !count || count > std::numeric_limits<std::size_t>::max() / sizeof(T))
					return false;

				auto *account = pool.account();
				auto *storage = static_cast<T *>(account->allocate(count * sizeof(T)));
				if(!storage) return false;

				account->retain();
				account_ = account;
				storage_ = storage;
				count_ = count;
				for(std::size_t index = 0; index < count; ++index)
					new(storage + index) T{};
				return true;
			}

			void reset()
			{
				if(!storage_) return;

				for(std::size_t index = count_; index; --index)
					storage_[index - 1].~T();
				account_->free(storage_, count_ * sizeof(T));
				account_->release();
				storage_ = nullptr;
				account_ = nullptr;
				count_ = 0;
			}

			T *get() const
			{
				return storage_;
			}

			std::size_t size() const
			{
				return count_;
			}

			T &operator[](std::size_t index) const
			{
				return storage_[index];
			}

			explicit operator bool() const
			{
				return storage_ != nullptr;
			}

		};
	}
}
