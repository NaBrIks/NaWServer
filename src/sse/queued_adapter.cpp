// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/detail/sse/queued_adapter.hpp>
#include <NaWServer/request.h>
#include <cstdlib>

namespace NaWServer::sse
{
	void QueuedAdapter::abortExternal(void *context)
	{
		auto &slot = *static_cast<AbortSlot *>(context);
		slot.requested.store(true, std::memory_order_release);
		if(slot.owner) slot.owner->wakeRuntime();
	}

	QueuedAdapter::~QueuedAdapter()
	{
		if(started_) std::abort(); // A failed stop retains ownership; never destroy it.

	}

	bool QueuedAdapter::beginRuntime(bool allow_restart, bool direct_storage)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(started_ || (stopping_ && !allow_restart) || !sessions_.valid_limits()) return false;

		if(allow_restart && !sessions_.restart()) return false;

		if(!sessions_.prepareStorage(direct_storage)) return false;

		stopping_ = false;
		started_ = true;
		return true;
	}

	void QueuedAdapter::requestStop()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!started_) return;

		stopping_ = true;
		sessions_.shutdown();
		wakeRuntime();
	}

	bool QueuedAdapter::finishRuntime()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(sessions_.active()) return false;

		started_ = false;
		sessions_.releaseStorage();
		return true;
	}

	Admit QueuedAdapter::reserve(Request &request, const Frame &header, Reservation &reservation)
	{
		if(!available(reservation)) return Admit::InvalidRequest;

		reset(reservation);
		if(request.method() != HttpMethod::Get || request.contentLength() || !header.size ||
		    header.size > frame_capacity)
			return Admit::InvalidRequest;

		auto &last = reservation.last_id;
		const auto read = request.readHeader("Last-Event-ID", last.bytes.data(), last.bytes.size());
		if(read.status == HeaderReadStatus::BufferTooSmall) return Admit::InvalidLastId;

		if(read.status != HeaderReadStatus::Ok && read.status != HeaderReadStatus::NotFound)
			return Admit::InvalidRequest;

		last.present = read.status == HeaderReadStatus::Ok;
		last.size = last.present ? read.size : 0;
		if(last.size >= last.bytes.size() || !valid_field({last.bytes.data(), last.size})) return Admit::InvalidLastId;

		auto *exchange = static_cast<Exchange *>(request.transportExtension(&exchange_key));
		if(!exchange || !exchange->owner()) return Admit::InvalidRequest;

		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!started_ || stopping_) return Admit::Stopping;

		time_ = std::max(time_, runtimeMilliseconds());

		if(owner_ && owner_ != exchange->owner()) return Admit::InvalidRequest;

		// Sessions needs a unique integer connection key, not a platform socket.
		std::size_t slot = 0;
		while(slot < streams_.size() && streams_[slot])
			++slot;
		if(slot == streams_.size() || !sessions_.hasCapacity()) return Admit::Full;

		auto stream = exchange->retain();
		if(!stream) return Admit::NoMemory;

		WriteBuffer direct_header;
		const bool direct = stream->supportsWriteLease();
		if(direct)
		{
			direct_header = stream->acquireWrite(header.size);
			if(!direct_header || direct_header.capacity() < header.size)
			{
				stream->rollback();
				return Admit::NoMemory;
			}
			std::memcpy(direct_header.data(), header.bytes.data(), header.size);
		}
		const auto token = direct ? sessions_.reserveDirect(static_cast<int>(slot), header.size, time_) :
		    sessions_.reserve(static_cast<int>(slot), header, time_);
		if(token.slot == client_capacity) { stream->rollback(); return Admit::Full; }

		owner_ = exchange->owner();
		direct_headers_[token.slot] = std::move(direct_header);
		streams_[token.slot] = std::move(stream);
		tokens_[token.slot] = token;
		aborts_[token.slot].owner = this;
		aborts_[token.slot].requested = false;
		++begun_;
		bind(reservation, token, {&aborts_[token.slot], abortExternal});
		return Admit::Owned;
	}

	bool QueuedAdapter::activate(Reservation &reservation)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!owns(reservation)) return false;

		time_ = std::max(time_, runtimeMilliseconds());

		const auto token = reservation.token();
		streams_[token.slot]->activate();
		if(aborts_[token.slot].requested.exchange(false)) sessions_.close(token);

		const auto ready = sessions_.activate(token, time_);
		release(reservation);
		wakeRuntime();
		return ready;
	}

	void QueuedAdapter::rollback(Reservation &reservation)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!owns(reservation)) return;

		const auto token = reservation.token();
		if(!sessions_.rollback(token)) std::abort();

		direct_headers_[token.slot].reset();
		streams_[token.slot]->rollback();
		streams_[token.slot].reset();
		tokens_[token.slot] = {};
		aborts_[token.slot].requested = false;
		++completed_;
		++rolled_back_;
		release(reservation);
		wakeRuntime();
	}

	Publish QueuedAdapter::publish(Token token, const Frame &frame)
	{
		NaWServer::detail::UniqueLock<NaWServer::detail::Mutex> lock(mutex_, NaWServer::detail::tryToLock);
		if(!lock.owns_lock()) return Publish::Busy;

		time_ = std::max(time_, runtimeMilliseconds());
		if(!sessions_.direct(token))
		{
			const auto result = sessions_.publish(token, frame);
			if(result == Publish::Queued || result == Publish::IndividualOverflow) wakeRuntime();

			return result;
		}

		auto &stream = streams_[token.slot];
		if(stream->readyForWrite() && !direct_headers_[token.slot])
		{
			const auto status = stream->writeStatus();
			sessions_.directProgress(token, status.bytes, status.failed, time_);
		}
		const auto ready = sessions_.directReadiness(token, frame.size);
		if(ready != Publish::Queued)
		{
			if(ready == Publish::IndividualOverflow) wakeRuntime();

			return ready;
		}

		if(!stream->readyForWrite() || direct_headers_[token.slot]) return Publish::Busy;

		auto buffer = stream->acquireWrite(frame.size);
		if(!buffer || buffer.capacity() < frame.size) return Publish::GlobalFull;

		std::memcpy(buffer.data(), frame.bytes.data(), frame.size);
		if(!buffer.commit(frame.size)) return Publish::GlobalFull;

		sessions_.directQueued(token, frame.size, true, time_);
		wakeRuntime();
		return Publish::Queued;
	}

	bool QueuedAdapter::close(Token token)
	{
		NaWServer::detail::UniqueLock<NaWServer::detail::Mutex> lock(mutex_, NaWServer::detail::tryToLock);
		if(!lock.owns_lock()) return false;

		sessions_.close(token);
		wakeRuntime();
		return true;
	}

	AdapterStats QueuedAdapter::stats()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		return {sessions_.active(), sessions_.queued_bytes(), completed_, sessions_.sent_frames(), begun_, rolled_back_,
		        closed_by_reason_, sessions_.managedMemoryStats()};
	}

	std::uint32_t QueuedAdapter::pollDelay(std::uint64_t milliseconds) const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		return sessions_.pollDelay(milliseconds);
	}

	void QueuedAdapter::poll(std::uint64_t milliseconds)
	{
		for(std::size_t index = 0; index < client_capacity; ++index)
		{
			Step step;
			std::shared_ptr<Stream> stream;
			CloseReason reason;
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
				if(!started_) return;

				if(milliseconds > time_) time_ = milliseconds;

				if(aborts_[index].requested.exchange(false)) sessions_.close(tokens_[index]);

				const auto token = tokens_[index];
				const auto &direct_stream = streams_[index];
				if(sessions_.direct(token) && sessions_.reason(token) == CloseReason::None &&
				    direct_stream && direct_stream->readyForWrite())
				{
					if(direct_headers_[index])
					{
						if(!direct_headers_[index].commit(sessions_.directHeaderSize(token)))
							sessions_.close(token, CloseReason::Disconnected);
					}
					const auto status = direct_stream->writeStatus();
					sessions_.directProgress(token, status.bytes, status.failed, time_);
					if(sessions_.directHeartbeat(token, time_))
					{
						Frame heartbeat;
						comment("heartbeat", heartbeat);
						auto buffer = direct_stream->acquireWrite(heartbeat.size);
						if(buffer && buffer.capacity() >= heartbeat.size)
						{
							std::memcpy(buffer.data(), heartbeat.bytes.data(), heartbeat.size);
							if(buffer.commit(heartbeat.size))
								sessions_.directQueued(token, heartbeat.size, false, time_);
							else sessions_.directHeartbeatBlocked(token, time_);
						}
						else sessions_.directHeartbeatBlocked(token, time_);
					}
				}

				if(close_pending_[index])
				{
					step.kind = StepKind::Close;
					step.token = tokens_[index];
					step.serial = close_serials_[index];
				}
				else
				{
					step = sessions_.next(index, time_);
					if(step.kind == StepKind::Close)
					{
						close_pending_[index] = true;
						close_serials_[index] = step.serial;
					}
				}

				stream = streams_[index];
				reason = sessions_.reason(step.token);
			}
			if(step.kind == StepKind::None) continue;

			if(step.kind == StepKind::Write)
			{
				const auto written = stream ? stream->write(step.bytes.data(), step.size) : -1;
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
				(void)sessions_.written(step, written, time_);
			}
			else
			{
				direct_headers_[index].reset();
				if(stream) stream->finish();

				// finish is a request, not proof that the owner released the socket.
				// Retain the same inflight close step until completion is observable.
				if(stream && stream->isOpen()) continue;

				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
				if(!sessions_.completed(step)) std::abort();

				streams_[index].reset();
				tokens_[index] = {};
				close_pending_[index] = false;
				close_serials_[index] = 0;
				++completed_;
				++closed_by_reason_[static_cast<std::size_t>(reason)];
			}
		}
	}

}
