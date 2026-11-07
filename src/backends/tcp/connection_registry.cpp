// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "connection_registry.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <limits>
#include <new>

namespace NaWServer::tcp::detail
{
	Handle ConnectionRegistry::releaseHttp(Token token)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(token.slot >= capacity_ || close_states_[token.slot].sse_registered) return {};

		return slots_.releaseHttp(token);
	}

#if NAWSERVER_TCP_SSE
	class SseLease final : public TcpSseLease
	{
		friend class ConnectionRegistry;
		ConnectionRegistry &registry_;
		Token token_;
		bool registered_ = false;
		bool committed_ = false;
		bool callback_complete_ = false;
		bool released_ = false;
		bool attempted_ = false;
		CloseResult last_close_;
		std::uint64_t cleanup_epoch_ = 0;

	public:
		SseLease(ConnectionRegistry &registry, Token token)
		  : registry_(registry), token_(token)
		{
		}

		~SseLease() override
		{
			if(!registered_)
				return;

			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
			// The queue retains a committed lease until its native close releases.
			if(committed_ && !released_)
				std::abort();

			if(registry_.storage_[token_.slot].generation == token_.generation)
				registry_.close_states_[token_.slot].sse_registered = false;

			--registry_.sse_registrations_;
		}

		void activate() override
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
			if(registered_ && !released_)
				committed_ = true;
		}

		void rollback() override
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
			if(!registered_ || committed_)
				return;

			registered_ = false;
			if(registry_.storage_[token_.slot].generation == token_.generation)
				registry_.close_states_[token_.slot].sse_registered = false;

			--registry_.sse_registrations_;
		}

		void callbackComplete() override
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
			if(!registered_ || callback_complete_)
				return;

			if(committed_ && !registry_.slots_.transfer(token_, Owner::Http, Owner::Sse))
				released_ = true; // Stale lifetime: never touch a replacement connection.

			callback_complete_ = true;
		}

		int write(const char *bytes, std::size_t size) override
		{
			Handle handle;
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
				if(!registered_ || !committed_ || released_ || attempted_ || (!bytes && size))
					return -1;

				if(!callback_complete_ || !size)
					return 0;

				handle = registry_.slots_.get(token_, Owner::Sse);
			}
			if(!handle.valid)
				return -1;

			const auto requested = std::min(size, static_cast<std::size_t>(INT_MAX));
			const auto result = registry_.transport_.writeSome(handle,
			        reinterpret_cast<const std::uint8_t *>(bytes), requested);
			if(result.status == IoStatus::Progress)
				return result.size && result.size <= requested ? static_cast<int>(result.size) : -1;

			if(result.size)
				return -1;

			return result.status == IoStatus::WouldBlock || result.status == IoStatus::Interrupted ? 0 : -1;
		}

		bool supportsWriteLease() const override
		{
			return registry_.transport_.supportsWriteLease();
		}

		WriteBuffer acquireWrite(std::size_t size) override
		{
			Handle handle;
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
				if(!registered_ || released_ || attempted_) return {};

				handle = registry_.slots_.get(token_, callback_complete_ ? Owner::Sse : Owner::Http);
			}
			return handle.valid ? registry_.transport_.acquireWrite(handle, size) : WriteBuffer{};
		}

		bool readyForWrite() const override
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
			return registered_ && committed_ && callback_complete_ && !released_ && !attempted_;
		}

		WriteStatus writeStatus() const override
		{
			Handle handle;
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
				handle = registry_.slots_.get(token_, callback_complete_ ? Owner::Sse : Owner::Http);
			}
			if(!handle.valid) return {0, true};

			const auto status = registry_.transport_.writePressure(handle);
			return {status.bytes, status.failed};
		}

		void finish() override
		{
			Handle handle;
			Owner owner;
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
				if(!registered_ || !committed_ || !callback_complete_ || released_)
					return;

				if(attempted_ && (last_close_.status != Status::WouldBlock ||
				    cleanup_epoch_ == registry_.sse_cleanup_epoch_))
					return;

				owner = attempted_ ? Owner::Closing : Owner::Sse;
				handle = registry_.slots_.get(token_, owner);
				attempted_ = true;
				cleanup_epoch_ = registry_.sse_cleanup_epoch_;
				if(!handle.valid)
					released_ = true;
			}
			if(!handle.valid)
				return;

			// The SSE owner closes once through the registry. A separate SHUT_WR
			// can leave lwIP's live netconn pointing at a recycled TIME_WAIT PCB.
			// Retained close outcomes keep the existing explicit retry contract.
			const auto result = registry_.close(token_, owner);
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
				last_close_ = result;
				released_ = result.released;
			}
		}

		bool isOpen() const override
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(registry_.mutex_);
			return registered_ && committed_ && !released_;
		}

	};

#endif
	ConnectionRegistry::ConnectionRegistry(Transport &transport, const void *identity,
	    ConnectionSlot *storage, CloseState *close_states, std::size_t capacity)
	  : transport_(transport),
#if NAWSERVER_TCP_SSE
	    identity_(identity),
#endif
	    storage_(storage), close_states_(close_states),
	    capacity_(capacity), slots_(storage, capacity)
	{
#if !NAWSERVER_TCP_SSE
		(void)identity;
#endif
	}

	Token ConnectionRegistry::claim(Handle &source, Owner owner)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(admission_closed_)
			return {};

		const auto token = slots_.claim(source, owner);
		if(token.valid())
			close_states_[token.slot] = {};

		return token;
	}

	Handle ConnectionRegistry::get(Token token, Owner owner) const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		return slots_.get(token, owner);
	}

	bool ConnectionRegistry::transfer(Token token, Owner previous, Owner next)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(admission_closed_ && previous == Owner::Http && next == Owner::WebSocket)
			return false;

		return slots_.transfer(token, previous, next);
	}

	CloseResult ConnectionRegistry::close(Token token, Owner owner, bool abort)
	{
		Handle handle;
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			if(owner == Owner::Closing)
				handle = slots_.get(token, owner);
			else
				handle = slots_.prepareClose(token, owner);

			if(!handle.valid)
				return {Status::InvalidState, 0, false};

			auto &state = close_states_[token.slot];
			if(owner != Owner::Closing)
				state.previous_owner = owner;

			state.attempted = true;
			state.abort = state.abort || abort;
			abort = state.abort;
		}
		const auto result = abort ? transport_.abort(handle) : transport_.close(handle);
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			(void)slots_.completeClose(token, result);
			auto &state = close_states_[token.slot];
			state.result = result;
#if NAWSERVER_TCP_SSE
			if(state.previous_owner == Owner::Sse && result.status != Status::Ok)
				++sse_close_errors_;

#endif
		}
		return result;
	}

	std::size_t ConnectionRegistry::active() const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		return slots_.active();
	}

	bool ConnectionRegistry::beginStop()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
#if NAWSERVER_TCP_SSE
		if(sse_registrations_)
			return false;

#endif

		admission_closed_ = true;
		return true;
	}

	bool ConnectionRegistry::cleanupAfterJoin()
	{
		// This runs only after the HTTP/WS owner joined and SSE registrations drained.
		for(std::size_t index = 0; index < capacity_; ++index)
		{
			Token retry;
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
				if(storage_[index].owner == Owner::Closing && close_states_[index].attempted &&
				    close_states_[index].result.status == Status::WouldBlock)
					retry = {index, storage_[index].generation};
			}
			if(retry.valid())
				(void)close(retry, Owner::Closing);
		}
		return active() == 0;
	}

#if NAWSERVER_TCP_SSE
	const void *ConnectionRegistry::identity() const
	{
		return identity_;
	}

	std::shared_ptr<TcpSseLease> ConnectionRegistry::retainSse(Token token)
	{
		// Admission allocations are bounded by slots. shared_ptr control storage still
		// requires heap reserve; this is not recoverable global OOM on exceptionless IDF.
		auto *raw = new(std::nothrow) SseLease(*this, token);
		if(!raw)
			return {};

		std::shared_ptr<SseLease> lease(raw);
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			if(admission_closed_ || !slots_.get(token, Owner::Http).valid ||
			    close_states_[token.slot].sse_registered)
				return {};

			lease->registered_ = true;
			close_states_[token.slot].sse_registered = true;
			++sse_registrations_;
		}
		return lease;
	}

	void ConnectionRegistry::retrySseClose()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(sse_cleanup_epoch_ != std::numeric_limits<std::uint64_t>::max())
			++sse_cleanup_epoch_;
	}

	std::size_t ConnectionRegistry::sseCloseErrors() const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		return sse_close_errors_;
	}

#endif

} // namespace NaWServer::tcp::detail
