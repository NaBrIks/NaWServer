// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "websocket_tx_pool.h"
#include <limits>
#include <new>
#include <utility>

namespace NaWServer::tcp::detail
{

	WebSocketTxPool::Lease::Lease(WebSocketTxPool &pool, std::size_t index) noexcept
	  : pool_(&pool), index_(index)
	{
	}

	WebSocketTxPool::Lease::~Lease()
	{
		reset();
	}

	WebSocketTxPool::Lease::Lease(Lease &&other) noexcept
	  : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_)
	{
	}

	WebSocketTxPool::Lease &WebSocketTxPool::Lease::operator=(Lease &&other) noexcept
	{
		if(this != &other)
		{
			reset();
			pool_ = std::exchange(other.pool_, nullptr);
			index_ = other.index_;
		}

		return *this;
	}

	std::uint8_t *WebSocketTxPool::Lease::data() const noexcept
	{
		return pool_ ? pool_->storage_.get() + index_ * pool_->block_size_ : nullptr;
	}

	std::size_t WebSocketTxPool::Lease::capacity() const noexcept
	{
		return pool_ ? pool_->block_size_ : 0;
	}

	WebSocketTxPool::Lease::operator bool() const noexcept
	{
		return pool_ != nullptr;
	}

	void WebSocketTxPool::Lease::reset() noexcept
	{
		if(auto *pool = std::exchange(pool_, nullptr))
			pool->release(index_);
	}

	WebSocketTxPool::WebSocketTxPool(std::size_t block_count, std::size_t block_size,
	    NaWServer::detail::BufferPool *buffers, bool direct)
	  : block_count_(block_count), block_size_(block_size), buffers_(buffers ? buffers : &local_buffers_), direct_(direct)
	{
		if(direct_) return;

		const auto maximum = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
		if(!block_count || !block_size || block_count > maximum / block_size || block_count > maximum / sizeof(bool))
			return;

		if(!buffers && !local_buffers_.initialize({}, BufferGroup::Tcp)) return;

		(void)storage_.allocate(*buffers_, block_count * block_size);
		if(!storage_)
		{
			if(!buffers) local_buffers_.reset();

			return;
		}

		(void)occupied_.allocate(*buffers_, block_count);
		if(!occupied_)
		{
			storage_.reset();
			if(!buffers) local_buffers_.reset();
		}
	}

	bool WebSocketTxPool::valid() const noexcept
	{
		return direct_ || (storage_ && occupied_);
	}

	std::size_t WebSocketTxPool::blockCount() const noexcept
	{
		return block_count_;
	}

	std::size_t WebSocketTxPool::blockSize() const noexcept
	{
		return block_size_;
	}

	std::size_t WebSocketTxPool::usedBlocks() const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		return used_;
	}

	WebSocketTxPool::AcquireResult WebSocketTxPool::tryAcquire()
	{
		if(!valid())
			return {AcquireStatus::InvalidState, {}};

		NaWServer::detail::UniqueLock<NaWServer::detail::Mutex> lock(mutex_, NaWServer::detail::tryToLock);
		if(!lock.owns_lock())
			return {AcquireStatus::Busy, {}};

		for(std::size_t index = 0; index < block_count_; ++index)
		{
			if(occupied_[index])
				continue;

			occupied_[index] = true;
			++used_;
			return {AcquireStatus::Acquired, Lease(*this, index)};
		}
		return {AcquireStatus::GlobalFull, {}};
	}

	void WebSocketTxPool::release(std::size_t index) noexcept
	{
		// Release cannot fail: no native operation, callback or payload copy holds
		// this metadata lock. Every occupied index has exactly one move-only lease.
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		occupied_[index] = false;
		--used_;
	}

} // namespace NaWServer::tcp::detail
