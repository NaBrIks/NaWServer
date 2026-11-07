// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <NaWServer/detail/synchronization.h>
#include <NaWServer/buffer_memory.h>

namespace NaWServer::tcp::detail
{

	// One backend owns the backing store. Every lease must be released before the
	// pool is destroyed. Leases move from producer to queue to active output.
	class WebSocketTxPool
	{
		friend struct WebSocketTxPoolTestAccess;

	public:
		class Lease
		{
			friend class WebSocketTxPool;
			WebSocketTxPool *pool_ = nullptr;
			std::size_t index_ = 0;
			Lease(WebSocketTxPool &pool, std::size_t index) noexcept;

		public:
			Lease() = default;
			~Lease();
			Lease(const Lease &) = delete;
			Lease &operator=(const Lease &) = delete;
			Lease(Lease &&other) noexcept;
			Lease &operator=(Lease &&other) noexcept;
			std::uint8_t *data() const noexcept;
			std::size_t capacity() const noexcept;
			explicit operator bool() const noexcept;
			void reset() noexcept;
		};
		enum class AcquireStatus { Acquired, Busy, GlobalFull, InvalidState };
		struct AcquireResult
		{
			AcquireStatus status;
			Lease block;
		};

		WebSocketTxPool(std::size_t block_count, std::size_t block_size,
		    NaWServer::detail::BufferPool *buffers = nullptr, bool direct = false);
		NaWServer::detail::BufferPool &memoryPool()
		{
			return *buffers_;
		}

		WebSocketTxPool(const WebSocketTxPool &) = delete;
		WebSocketTxPool &operator=(const WebSocketTxPool &) = delete;
		bool valid() const noexcept;
		std::size_t blockCount() const noexcept;
		std::size_t blockSize() const noexcept;
		std::size_t usedBlocks() const;
		// No allocation, copy or wait for another producer. The metadata lock is
		// released before the caller copies into its exclusively owned block.
		AcquireResult tryAcquire();

	private:
		std::size_t block_count_;
		std::size_t block_size_;
		NaWServer::detail::BufferPool local_buffers_;
		NaWServer::detail::BufferPool *buffers_;
		NaWServer::detail::BufferArray<std::uint8_t> storage_;
		NaWServer::detail::BufferArray<bool> occupied_;
		mutable NaWServer::detail::Mutex mutex_;
		std::size_t used_ = 0;
		bool direct_ = false;
		void release(std::size_t index) noexcept;
	};

} // namespace NaWServer::tcp::detail
