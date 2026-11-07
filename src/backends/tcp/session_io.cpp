// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "session_io.h"
#include <algorithm>
#include <array>
#include <limits>

namespace NaWServer::tcp::detail
{

	namespace
	{

		std::uint64_t addBounded(std::uint64_t time, std::uint32_t milliseconds)
		{
			const auto available = std::numeric_limits<std::uint64_t>::max() - time;
			return time + std::min(available, static_cast<std::uint64_t>(milliseconds));
		}

	} // namespace

	SessionIo::SessionIo(Transport &transport, Handle handle, const std::atomic<bool> &stop,
	    Clock clock, std::uint64_t absolute_deadline_ms, std::uint32_t no_progress_timeout_ms,
	    std::uint32_t wait_ceiling_ms, std::uint32_t close_timeout_ms, std::size_t close_drain_bytes)
	  : transport_(transport), handle_(handle), stop_(stop), clock_(clock), deadline_(absolute_deadline_ms),
	    no_progress_timeout_(no_progress_timeout_ms), wait_ceiling_(wait_ceiling_ms),
	    close_timeout_(close_timeout_ms), close_drain_bytes_(close_drain_bytes)
	{
	}

	void SessionIo::fail(IoFailure failure, int native_error)
	{
		failure_ = failure;
		native_error_ = native_error;
	}

	bool SessionIo::begin(Phase phase)
	{
		fail(IoFailure::None);
		if(!handle_.valid || !clock_.milliseconds || !no_progress_timeout_ || !wait_ceiling_)
		{
			fail(IoFailure::Transport);
			return false;
		}

		if(phase_ != phase)
		{
			phase_ = phase;
			last_progress_ = now();
		}

		return true;
	}

	bool SessionIo::allowed(std::uint64_t deadline)
	{
		if(stop_.load(std::memory_order_acquire))
		{
			fail(IoFailure::Stopped);
			return false;
		}

		const auto time = now();
		if(time >= deadline || time >= addBounded(last_progress_, no_progress_timeout_))
		{
			fail(IoFailure::Timeout);
			return false;
		}

		return true;
	}

	bool SessionIo::wait(bool writing, std::uint64_t deadline)
	{
		if(!allowed(deadline)) return false;

		const auto time = now();
		const auto bound = std::min(deadline, addBounded(last_progress_, no_progress_timeout_));
		if(time >= bound)
		{
			fail(IoFailure::Timeout);
			return false;
		}

		const auto timeout = static_cast<std::uint32_t>(std::min(bound - time,
		    static_cast<std::uint64_t>(wait_ceiling_)));
		const Interest interest{handle_, !writing, writing};
		Ready ready;
		const auto result = transport_.wait(&interest, &ready, 1, timeout);
		const bool any = ready.readable || ready.writable || ready.error;
		if((result.status == WaitStatus::Ready &&
		    (result.ready_count != 1 || !any || (writing ? ready.readable : ready.writable))) ||
		    (result.status != WaitStatus::Ready && (result.ready_count || any)))
		{
			fail(IoFailure::Transport, result.native_error);
			return false;
		}

		if(result.status == WaitStatus::Ready || result.status == WaitStatus::Timeout ||
		    result.status == WaitStatus::Interrupted)
			return allowed(deadline);

		fail(IoFailure::Transport, result.native_error);
		return false;
	}

	BodyReadResult SessionIo::read(std::uint8_t *destination, std::size_t capacity)
	{
		return readUntil(destination, capacity, deadline_);
	}

	BodyReadResult SessionIo::readUntil(std::uint8_t *destination, std::size_t capacity,
	    std::uint64_t additional_deadline_ms)
	{
		if(!begin(Phase::Read)) return {BodyReadStatus::TransportError, 0};

		if(!destination || !capacity)
		{
			fail(IoFailure::Transport);
			return {BodyReadStatus::TransportError, 0};
		}

		const auto deadline = std::min(deadline_, additional_deadline_ms);
		for(;;)
		{
			if(!allowed(deadline))
				return {failure_ == IoFailure::Timeout ? BodyReadStatus::Timeout : BodyReadStatus::TransportError, 0};

			const auto result = transport_.readSome(handle_, destination, capacity);
			if((result.status == IoStatus::Progress && (!result.size || result.size > capacity)) ||
			    (result.status != IoStatus::Progress && result.size))
			{
				fail(IoFailure::Transport, result.native_error);
				return {BodyReadStatus::TransportError, 0};
			}

			if(result.status == IoStatus::Progress)
			{
				last_progress_ = now();
				return {BodyReadStatus::Data, result.size};
			}

			if(result.status == IoStatus::PeerClosed)
			{
				fail(IoFailure::PeerClosed, result.native_error);
				return {BodyReadStatus::Disconnected, 0};
			}

			if(result.status != IoStatus::WouldBlock && result.status != IoStatus::Interrupted)
			{
				fail(IoFailure::Transport, result.native_error);
				return {BodyReadStatus::TransportError, 0};
			}

			if(!wait(false, deadline))
				return {failure_ == IoFailure::Timeout ? BodyReadStatus::Timeout : BodyReadStatus::TransportError, 0};
		}
	}

	bool SessionIo::write(const std::uint8_t *bytes, std::size_t size)
	{
		return writeRange(bytes, size, false);
	}

	bool SessionIo::writeImmutable(const std::uint8_t *bytes, std::size_t size)
	{
		return writeRange(bytes, size, true);
	}

	bool SessionIo::writeRange(const std::uint8_t *bytes, std::size_t size, bool immutable)
	{
		if(!begin(Phase::Write)) return false;

		if(!bytes && size)
		{
			fail(IoFailure::Transport);
			return false;
		}

		std::size_t offset = 0;
		while(offset < size)
		{
			if(!allowed(deadline_)) return false;

			const auto result = immutable ? transport_.writeImmutableSome(handle_, bytes + offset, size - offset) :
			    transport_.writeSome(handle_, bytes + offset, size - offset);
			if((result.status == IoStatus::Progress && (!result.size || result.size > size - offset)) ||
			    (result.status != IoStatus::Progress && result.size))
			{
				fail(IoFailure::Transport, result.native_error);
				return false;
			}

			if(result.status == IoStatus::Progress)
			{
				offset += result.size;
				last_progress_ = now();
				continue;
			}

			if(result.status != IoStatus::WouldBlock && result.status != IoStatus::Interrupted)
			{
				fail(result.status == IoStatus::PeerClosed ? IoFailure::PeerClosed : IoFailure::Transport,
				    result.native_error);
				return false;
			}

			if(!wait(true, deadline_)) return false;
		}
		return size || allowed(deadline_);
	}

	WriteLease SessionIo::acquireWrite(std::size_t size, WritePolicy policy)
	{
		if(!size || !transport_.supportsWriteLease() || !begin(Phase::Write)) return {};

		while(allowed(deadline_))
		{
			auto block = transport_.acquireWrite(handle_, size, policy);
			if(block) { last_progress_ = now(); return block; }

			if(!wait(true, deadline_)) break;
		}
		return {};
	}

	bool SessionIo::writeSource(ResponseSource source)
	{
		if(!source.read && source.size) return false;

		std::size_t offset = 0;
		while(offset < source.size)
		{
			if(!begin(Phase::Write) || !allowed(deadline_)) return false;

			if(transport_.supportsWriteLease())
			{
				auto block = transport_.acquireWrite(handle_, source.size - offset);
				if(!block)
				{
					if(!wait(true, deadline_)) return false;

					continue;
				}
				const auto result = source.read(source.context, offset, block.data(), block.capacity());
				if(result.status != BodyReadStatus::Data || !result.size || result.size > block.capacity() ||
				    !block.commit(result.size)) return false;

				offset += result.size;
				last_progress_ = now();
			}
			else
			{
				std::array<std::uint8_t, 1024> scratch;
				const auto size = std::min(scratch.size(), source.size - offset);
				const auto result = source.read(source.context, offset, scratch.data(), size);
				if(result.status != BodyReadStatus::Data || !result.size || result.size > size ||
				    !write(scratch.data(), result.size)) return false;

				offset += result.size;
			}
		}
		return transport_.flushWrites(handle_, deadline_);
	}

	void SessionIo::finish()
	{
		if(stop_.load(std::memory_order_acquire) || !handle_.valid || !clock_.milliseconds ||
		    !no_progress_timeout_ || !wait_ceiling_ || now() >= deadline_) return;

		const auto saved_failure = failure_;
		const auto saved_native = native_error_;
		const auto drain_deadline = std::min(deadline_, addBounded(now(), close_timeout_));
		// Discard before the owner's one close, without a separate SHUT_WR. lwIP
		// can reclaim a TIME_WAIT PCB while a half-closed netconn still refers to
		// it. An EOF-dependent peer must not extend this bounded pre-close drain.
		std::array<std::uint8_t, 256> discard;
		std::size_t drained = 0;
		while(drained < close_drain_bytes_ && !stop_.load(std::memory_order_acquire))
		{
			const auto received = readUntil(discard.data(), std::min(discard.size(), close_drain_bytes_ - drained),
			        drain_deadline);
			if(received.status != BodyReadStatus::Data) break;

			drained += received.size;
		}

		if(saved_failure != IoFailure::None)
		{
			failure_ = saved_failure;
			native_error_ = saved_native;
		}
		else if(failure_ == IoFailure::PeerClosed || failure_ == IoFailure::Timeout)
		{
			fail(IoFailure::None);
		}
	}

	IoFailure SessionIo::failure() const
	{
		return failure_;
	}

	int SessionIo::nativeError() const
	{
		return native_error_;
	}

	std::uint64_t SessionIo::now() const
	{
		return clock_.milliseconds ? clock_.milliseconds(clock_.context) : 0;
	}

	std::uint64_t SessionIo::deadline() const
	{
		return deadline_;
	}

} // namespace NaWServer::tcp::detail
