// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include "queued_transport.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace NaWServer::tcp::detail
{
	std::size_t QueuedTransport::metadataBytes(std::size_t connection_capacity, std::size_t job_capacity) noexcept
	{
		const auto maximum = std::numeric_limits<std::size_t>::max();
		if(!connection_capacity || !job_capacity || job_capacity > maximum_job_capacity ||
		    connection_capacity > maximum / sizeof(Connection) || job_capacity > maximum / sizeof(Job)) return 0;
		const auto connection_bytes = connection_capacity * sizeof(Connection);
		const auto job_bytes = job_capacity * sizeof(Job);
		const auto padding = (alignof(Job) - connection_bytes % alignof(Job)) % alignof(Job);
		if(connection_bytes > maximum - padding || job_bytes > maximum - connection_bytes - padding) return 0;
		return connection_bytes + padding + job_bytes;
	}

	QueuedTransport::QueuedTransport(Transport &native, TransmissionOptions options, std::size_t connection_capacity)
	  : native_(native), options_(options), allocator_(options.allocator), metadata_allocator_(options.metadata_allocator)
	{
		if((options_.execution != TransmissionExecution::DedicatedWorker &&
		    options_.execution != TransmissionExecution::Cooperative) ||
		    !connection_capacity || !options_.maximum_blocks || options_.maximum_blocks > maximum_job_capacity ||
		    options_.initial_blocks > options_.maximum_blocks || !options_.block_bytes ||
		    !options_.maximum_blocks_per_connection || !options_.stall_timeout_ms ||
		    !options_.close_timeout_ms || !options_.step_bytes ||
		    options_.reserved_important_blocks >= options_.maximum_blocks) return;

		if(!allocator_.allocate && !allocator_.release) allocator_ = defaultBufferAllocator(BufferGroup::Tcp);
		if(!allocator_.valid()) return;
		if(options_.min_free_bytes && !allocator_.available) return;

		if(!options_.reserved_important_bytes)
		{
			if(options_.reserved_important_blocks > options_.maximum_bytes / options_.block_bytes) return;

			options_.reserved_important_bytes = options_.reserved_important_blocks * options_.block_bytes;
		}
		if(options_.reserved_important_bytes >= options_.maximum_bytes) return;

		const auto static_blocks = std::min(options_.maximum_blocks,
		    options_.static_payload ? options_.static_payload_bytes / options_.block_bytes : 0);
		if(options_.block_bytes > options_.maximum_bytes ||
		    std::max(static_blocks, options_.initial_blocks) > options_.maximum_bytes / options_.block_bytes) return;

		if(!metadata_allocator_.allocate && !metadata_allocator_.release)
			metadata_allocator_ = defaultBufferAllocator(BufferGroup::Tcp);
		if(!metadata_allocator_.valid()) return;

		const auto metadata_bytes = metadataBytes(connection_capacity, options_.maximum_blocks);
		if(!metadata_bytes || metadata_bytes > options_.metadata_max_bytes) return;
		const auto connection_bytes = connection_capacity * sizeof(Connection);
		const auto padding = (alignof(Job) - connection_bytes % alignof(Job)) % alignof(Job);
		metadata_ = metadata_allocator_.allocate(metadata_allocator_.context, metadata_bytes);
		if(!metadata_) return;
		if(reinterpret_cast<std::uintptr_t>(metadata_) % alignof(std::max_align_t))
		{
			metadata_allocator_.release(metadata_allocator_.context, metadata_, metadata_bytes);
			metadata_ = nullptr;
			return;
		}
		metadata_bytes_ = metadata_bytes;
		connections_.data = static_cast<Connection *>(metadata_);
		connections_.count = connection_capacity;
		jobs_.data = reinterpret_cast<Job *>(static_cast<std::uint8_t *>(metadata_) + connection_bytes + padding);
		jobs_.count = options_.maximum_blocks;
		for(std::size_t index = 0; index < connections_.size(); ++index)
			new(connections_.data + index) Connection{};
		for(std::size_t index = 0; index < jobs_.size(); ++index)
			new(jobs_.data + index) Job{};

		for(std::size_t index = 0; index < std::max(static_blocks, options_.initial_blocks); ++index)
		{
			auto &job = jobs_[index];
			job.bytes = index < static_blocks ? options_.static_payload + index * options_.block_bytes :
			    allocatePayload(options_.block_bytes);
			if(!job.bytes) return;

			job.capacity = options_.block_bytes;
			job.fixed = true;
			stats_.allocated_bytes += job.capacity;
		}
		stats_.peak_bytes = stats_.allocated_bytes;
		valid_ = true;
	}

	std::uint8_t *QueuedTransport::allocatePayload(std::size_t bytes)
	{
		if(options_.min_free_bytes)
		{
			BufferAvailability available;
			if(!allocator_.available(allocator_.context, available) ||
			    available.free_bytes < options_.min_free_bytes ||
			    bytes > available.free_bytes - options_.min_free_bytes || bytes > available.largest_block_bytes)
				return nullptr;
		}
		auto *storage = static_cast<std::uint8_t *>(options_.min_free_bytes && allocator_.allocate_guarded ?
		    allocator_.allocate_guarded(allocator_.context, bytes, options_.min_free_bytes) :
		    allocator_.allocate(allocator_.context, bytes));
		if(storage && reinterpret_cast<std::uintptr_t>(storage) % alignof(std::max_align_t))
		{
			allocator_.release(allocator_.context, storage, bytes);
			return nullptr;
		}
		return storage;
	}

	QueuedTransport::~QueuedTransport()
	{
		if(started_.load() && !stop(1000)) std::abort();

		const auto static_begin = reinterpret_cast<std::uintptr_t>(options_.static_payload);
		for(auto &job : jobs_)
		{
			if(job.state == State::Filling) std::abort();

			const auto address = reinterpret_cast<std::uintptr_t>(job.bytes);
			if(job.bytes && !(options_.static_payload && address >= static_begin &&
			    address - static_begin < options_.static_payload_bytes))
				allocator_.release(allocator_.context, job.bytes, job.capacity);
		}
		for(auto &job : jobs_) job.~Job();
		for(auto &connection : connections_) connection.~Connection();
		if(metadata_) metadata_allocator_.release(metadata_allocator_.context, metadata_, metadata_bytes_);
	}

	bool QueuedTransport::start()
	{
		if(!valid_ || started_.load()) return false;

		if(options_.execution == TransmissionExecution::DedicatedWorker && !signal_.initialize()) return false;

		stopping_.store(false);
		started_.store(true);
		if(options_.execution == TransmissionExecution::Cooperative) return true;

		worker_ = startTcpWorker(entry, this, WorkerRole::Transmission);
		if(!worker_) started_.store(false);

		return worker_ != nullptr;
	}

	bool QueuedTransport::stop(std::uint32_t timeout)
	{
		if(!started_.load()) return true;

		stopping_.store(true);
		if(options_.execution == TransmissionExecution::Cooperative)
		{
			const auto before = tcpMilliseconds();
			while(pumping_.test_and_set(std::memory_order_acquire))
			{
				if(tcpMilliseconds() - before >= timeout) return false;

				sleepTcpWorker(1);
			}
			clearReady();
			started_.store(false);
			pumping_.clear(std::memory_order_release);
			return true;
		}
		signal_.notify();
		if(!joinTcpWorker(worker_, timeout)) return false;

		worker_ = nullptr;
		started_.store(false);
		return true;
	}

	std::size_t QueuedTransport::find(Handle handle) const
	{
		for(std::size_t index = 0; index < connections_.size(); ++index)
			if(handle.valid && connections_[index].handle.valid && connections_[index].handle.value == handle.value)
				return index;

		return connections_.size();
	}

	bool QueuedTransport::registerHandle(Handle handle)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!handle.valid || generation_ == UINT64_MAX || find(handle) != connections_.size()) return false;

		for(auto &connection : connections_)
			if(!connection.handle.valid)
			{
				connection = {};
				connection.handle = handle;
				connection.generation = ++generation_;
				return true;
			}
		return false;
	}

	ListenerResult QueuedTransport::openListener(const char *address, std::uint16_t port, int backlog)
	{
		auto result = native_.openListener(address, port, backlog);
		if(result.handle.valid && !registerHandle(result.handle))
		{
			const auto closed = native_.close(result.handle);
			result.status = Status::Fatal;
			result.cleanup = closed;
		}
		return result;
	}

	AcceptResult QueuedTransport::accept(Handle listener)
	{
		auto result = native_.accept(listener);
		if(result.handle.valid && !registerHandle(result.handle))
		{
			const auto closed = native_.close(result.handle);
			result.status = Status::Fatal;
			result.cleanup = closed;
		}
		return result;
	}

	WriteLease QueuedTransport::acquireWrite(Handle handle, std::size_t requested, WritePolicy policy)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		const auto connection = find(handle);
		if(!requested || connection == connections_.size() || stopping_.load() ||
		    connections_[connection].closing || connections_[connection].failed || serial_ == UINT64_MAX) return {};

		policy.priority = std::max(policy.priority, connections_[connection].policy.priority);
		std::size_t active = 0, own = 0, active_bytes = 0;
		for(const auto &job : jobs_)
			if(job.state != State::Free)
			{
				++active;
				if(!job.immutable) active_bytes += job.capacity;
				if(job.connection == connection) ++own;
			}
		if(own >= options_.maximum_blocks_per_connection ||
		    (policy.priority < options_.important_priority &&
		    active >= options_.maximum_blocks - options_.reserved_important_blocks))
		{
			++stats_.rejected_jobs;
			return {};
		}
		for(std::size_t index = 0; index < options_.maximum_blocks; ++index)
		{
			auto &job = jobs_[index];
			if(job.state != State::Free) continue;

			auto bytes = std::min(requested, options_.block_bytes);
			if(policy.priority < options_.important_priority)
			{
				const auto ceiling = options_.maximum_bytes - options_.reserved_important_bytes;
				if(active_bytes >= ceiling || (job.bytes && job.capacity > ceiling - active_bytes)) continue;

				bytes = std::min(bytes, ceiling - active_bytes);
			}
			if(!job.bytes)
			{
				if(bytes > options_.maximum_bytes - stats_.allocated_bytes) continue;

				job.bytes = allocatePayload(bytes);
				if(!job.bytes) continue;

				job.capacity = bytes;
				stats_.allocated_bytes += bytes;
				stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.allocated_bytes);
			}
			job.state = State::Filling;
			job.connection = connection;
			job.generation = connections_[connection].generation;
			job.serial = ++serial_;
			job.policy = policy;
			++stats_.filling_jobs;
			return {this, index, job.serial, job.bytes, std::min(job.capacity, bytes), commitLease, cancelLease, policy};
		}
		++stats_.rejected_jobs;
		return {};
	}

	bool QueuedTransport::commitLease(void *context, std::size_t index, std::uint64_t serial,
	    std::size_t size, WritePolicy policy)
	{
		auto &self = *static_cast<QueuedTransport *>(context);
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(self.mutex_);
		auto &job = self.jobs_[index];
		if(job.state != State::Filling || job.serial != serial || !size || size > job.capacity ||
		    self.connections_[job.connection].generation != job.generation ||
		    !self.connections_[job.connection].handle.valid || self.connections_[job.connection].aborted ||
		    self.stopping_.load()) return false;

		job.size = size; job.offset = 0;
		policy.priority = std::max(policy.priority, job.policy.priority);
		if(policy.replacement_key)
			for(auto &other : self.jobs_)
				if(&other != &job && other.state == State::Ready && !other.offset &&
				    other.connection == job.connection && other.policy.replacement_key == policy.replacement_key)
				{
					++self.stats_.discarded_jobs;
					self.stats_.discarded_bytes += other.size;
					self.releaseJob(other);
				}
		job.policy = policy;
		job.queued_at = job.last_progress = tcpMilliseconds();
		job.retry_at = 0;
		job.head_started = false;
		job.state = State::Ready;
		--self.stats_.filling_jobs;
		++self.stats_.queued_jobs;
		self.stats_.accepted_bytes += size;
		self.signal_.notify();
		return true;
	}

	void QueuedTransport::releaseJob(Job &job)
	{
		if(job.state == State::Filling) --stats_.filling_jobs;
		else if(job.state != State::Free) --stats_.queued_jobs;

		if(!job.fixed && job.bytes)
		{
			allocator_.release(allocator_.context, job.bytes, job.capacity);
			stats_.allocated_bytes -= job.capacity;
			job.bytes = nullptr; job.capacity = 0;
		}
		job.state = State::Free;
		job.immutable = nullptr;
	}

	void QueuedTransport::cancelLease(void *context, std::size_t index, std::uint64_t serial)
	{
		auto &self = *static_cast<QueuedTransport *>(context);
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(self.mutex_);
		auto &job = self.jobs_[index];
		if(job.state == State::Filling && job.serial == serial)
		{
			self.releaseJob(job);
			// A canceled producer may have held the ordering barrier for later jobs.
			self.signal_.notify();
		}
	}

	IoResult QueuedTransport::readSome(Handle handle, std::uint8_t *bytes, std::size_t size)
	{
		return native_.readSome(handle, bytes, size);
	}

	IoResult QueuedTransport::writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size)
	{
		if(!bytes || !size) return {IoStatus::InvalidArgument, 0, 0};

		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			const auto index = find(handle);
			if(index == connections_.size()) return {IoStatus::InvalidState, 0, 0};

			if(connections_[index].failed) return {IoStatus::Fatal, 0, connections_[index].native_error};
		}
		auto block = acquireWrite(handle, size);
		if(!block) return {IoStatus::WouldBlock, 0, 0};

		const auto copied = block.capacity();
		std::memcpy(block.data(), bytes, copied);
		return block.commit(copied) ? IoResult{IoStatus::Progress, copied, 0} : IoResult{IoStatus::Fatal, 0, 0};
	}

	IoResult QueuedTransport::writeImmutableSome(Handle handle, const std::uint8_t *bytes, std::size_t size)
	{
		if(!bytes || !size) return {IoStatus::InvalidArgument, 0, 0};

		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		const auto connection = find(handle);
		if(connection == connections_.size() || stopping_.load() || connections_[connection].closing)
			return {IoStatus::InvalidState, 0, 0};

		if(connections_[connection].failed) return {IoStatus::Fatal, 0, connections_[connection].native_error};

		const auto policy = connections_[connection].policy;
		std::size_t active = 0, own = 0;
		for(const auto &job : jobs_)
			if(job.state != State::Free)
			{
				++active;
				if(job.connection == connection) ++own;
			}
		if(serial_ == UINT64_MAX || own >= options_.maximum_blocks_per_connection ||
		    (policy.priority < options_.important_priority &&
		    active >= options_.maximum_blocks - options_.reserved_important_blocks))
		{ ++stats_.rejected_jobs; return {IoStatus::WouldBlock, 0, 0}; }

		std::size_t selected = jobs_.size();
		for(std::size_t index = 0; index < options_.maximum_blocks; ++index)
			if(jobs_[index].state == State::Free &&
			    (selected == jobs_.size() || (jobs_[selected].bytes && !jobs_[index].bytes))) selected = index;

		if(selected == jobs_.size()) { ++stats_.rejected_jobs; return {IoStatus::WouldBlock, 0, 0}; }

		auto &job = jobs_[selected];
		job.state = State::Ready; job.connection = connection;
		job.generation = connections_[connection].generation; job.serial = ++serial_;
		job.immutable = bytes; job.size = std::min(size, options_.block_bytes); job.offset = 0;
		job.policy = policy; job.head_started = false; job.retry_at = 0;
		job.queued_at = job.last_progress = tcpMilliseconds();
		++stats_.queued_jobs; stats_.accepted_bytes += job.size;
		signal_.notify();
		return {IoStatus::Progress, job.size, 0};
	}

	bool QueuedTransport::pending(std::size_t connection) const
	{
		for(const auto &job : jobs_)
			if(job.state != State::Free && job.connection == connection) return true;

		return false;
	}

	bool QueuedTransport::cancelPending(std::size_t connection)
	{
		for(const auto &job : jobs_)
			if(job.connection == connection && (job.state == State::Writing || job.state == State::Filling)) return false;

		for(auto &job : jobs_)
			if(job.connection == connection && job.state != State::Free)
			{
				++stats_.failed_jobs;
				if(!connections_[connection].failed)
				{
					stats_.last_failure = TransmissionFailure::Canceled;
					stats_.last_native_error = 0;
				}
				stats_.failed_bytes += job.size - job.offset;
				releaseJob(job);
			}

		return true;
	}

	bool QueuedTransport::flushWrites(Handle handle, std::uint64_t deadline)
	{
		for(;;)
		{
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
				const auto index = find(handle);
				if(index == connections_.size() || connections_[index].failed) return false;

				if(!pending(index)) return true;
			}
			if(stopping_.load() || tcpMilliseconds() >= deadline) return false;

			if(options_.execution == TransmissionExecution::Cooperative && !pumpStep()) continue;

			sleepTcpWorker(1);
		}
	}

	WaitResult QueuedTransport::wait(const Interest *interests, Ready *ready, std::size_t count,
	    std::uint32_t timeout)
	{
		if(options_.execution == TransmissionExecution::Cooperative)
		{
			(void)pumpStep();
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			if(stats_.queued_jobs) timeout = std::min(timeout, std::uint32_t{1});
		}
		auto result = native_.wait(interests, ready, count, timeout);
		if(options_.execution == TransmissionExecution::Cooperative) (void)pumpStep();

		if(!interests || !ready || !count ||
		    (result.status != WaitStatus::Ready && result.status != WaitStatus::Timeout)) return result;

		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			for(std::size_t index = 0; index < count; ++index)
			{
				const auto connection = find(interests[index].handle);
				if(connection == connections_.size() || !connections_[connection].failed) continue;

				if(!ready[index].readable && !ready[index].writable && !ready[index].error) ++result.ready_count;

				ready[index].error = true;
				ready[index].native_error = connections_[connection].native_error;
				result.status = WaitStatus::Ready;
			}
		}
		// Native readiness is only a hint; queue pressure is reported by the next
		// bounded write. Yielding prevents a writable socket/full queue spin loop.
		if(result.status == WaitStatus::Ready)
		{
			bool writes = false;
			for(std::size_t index = 0; index < count; ++index) writes = writes || ready[index].writable;

			if(writes) sleepTcpWorker(1);
		}
		return result;
	}

	OperationResult QueuedTransport::shutdownWrite(Handle handle)
	{
		if(!flushWrites(handle, tcpMilliseconds() + options_.close_timeout_ms)) return {Status::WouldBlock, 0};

		return native_.shutdownWrite(handle);
	}

	CloseResult QueuedTransport::close(Handle &handle)
	{
		std::size_t index;
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			index = find(handle);
			if(index != connections_.size()) connections_[index].closing = true;
		}
		if(index == connections_.size()) return native_.close(handle);
		(void)flushWrites(handle, tcpMilliseconds() + options_.close_timeout_ms);
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			if(!cancelPending(index)) return {Status::WouldBlock, 0, false};
		}
		const auto closed = native_.close(handle);
		if(closed.released)
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			connections_[index] = {};
		}
		return closed;
	}

	CloseResult QueuedTransport::abort(Handle &handle)
	{
		std::size_t index;
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			index = find(handle);
			if(index != connections_.size())
			{
				auto &connection = connections_[index];
				connection.closing = connection.aborted = true;
				// Writing owns a native syscall and Filling owns a borrowed producer
				// buffer. The caller retries after those owners return their leases.
				if(!cancelPending(index))
				{
					signal_.notify();
					return {Status::WouldBlock, 0, false};
				}
			}
		}
		const auto result = native_.abort(handle);
		if(result.released && index != connections_.size())
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			connections_[index] = {};
		}
		return result;
	}

	TransmissionStats QueuedTransport::stats() const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		auto result = stats_;
		result.enabled = true;
		result.execution = options_.execution;
		result.maximum_jobs = options_.maximum_blocks;
		result.metadata_bytes = metadata_bytes_;
		result.metadata_connection_capacity = connections_.size();
		result.metadata_job_capacity = jobs_.size();
		result.maximum_bytes = options_.maximum_bytes;
		result.block_bytes = options_.block_bytes;
		result.step_bytes = options_.step_bytes;
		result.stall_timeout_ms = options_.stall_timeout_ms;
		result.close_timeout_ms = options_.close_timeout_ms;
		result.maximum_jobs_per_connection = options_.maximum_blocks_per_connection;
		result.reserved_important_jobs = options_.reserved_important_blocks;
		result.reserved_important_bytes = options_.reserved_important_bytes;
		for(const auto &job : jobs_)
			if(job.state != State::Free)
			{
				if(!job.immutable) result.used_bytes += job.capacity;
				else result.borrowed_bytes += job.size - job.offset;
				if(job.state != State::Filling) result.queued_bytes += job.size - job.offset;

				if(job.state == State::Writing) ++result.active_jobs;
			}
		return result;
	}

	WritePressure QueuedTransport::writePressure(Handle handle) const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		WritePressure result;
		const auto index = find(handle);
		result.failed = index == connections_.size() || connections_[index].failed;
		if(index != connections_.size()) result.sent_bytes = connections_[index].sent_bytes;
		for(const auto &job : jobs_)
			if(job.state != State::Free && job.connection == index)
			{
				++result.jobs;
				result.bytes += job.state == State::Filling ? job.capacity : job.size - job.offset;
			}
		return result;
	}

	void QueuedTransport::setWritePolicy(Handle handle, WritePolicy policy)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		const auto index = find(handle);
		if(index != connections_.size()) connections_[index].policy = policy;
	}

	void QueuedTransport::entry(void *context) { static_cast<QueuedTransport *>(context)->run(); }

	std::uint32_t QueuedTransport::pumpStep()
	{
		// Different HTTP/SSE/WS owners may enter wait or flush concurrently. The
		// nonblocking flag keeps native TX single-owner without holding the queue
		// mutex over a native syscall, and lets shutdown wait for that owner.
		if(pumping_.test_and_set(std::memory_order_acquire)) return 1;

		struct Release
		{
			std::atomic_flag &flag;
			~Release() { flag.clear(std::memory_order_release); }
		} release{pumping_};
		if(!started_.load() || stopping_.load()) return UINT32_MAX;

		std::size_t selected = jobs_.size();
		bool pending_ready = false;
		Handle handle;
		const auto now = tcpMilliseconds();
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			std::uint64_t score = 0;
			const bool fairness_turn = ((dispatches_ + 1) % 4) == 0;
			for(std::size_t count = 0; count < options_.maximum_blocks; ++count)
			{
				const auto index = (cursor_ + count) % options_.maximum_blocks;
				auto &job = jobs_[index];
				if(job.state != State::Ready) continue;

				pending_ready = true;
				if(now < job.retry_at) continue;

				auto &connection = connections_[job.connection];
				if(!connection.handle.valid || connection.generation != job.generation || connection.failed || connection.aborted)
				{
					++stats_.failed_jobs; stats_.failed_bytes += job.size - job.offset; releaseJob(job); continue;
				}
				if(job.policy.expires_at_ms && now >= job.policy.expires_at_ms)
				{
					if(!job.offset && job.policy.discard_expired && job.policy.expires_at_ms &&
					    now >= job.policy.expires_at_ms)
					{ ++stats_.discarded_jobs; stats_.discarded_bytes += job.size; }
					else
					{
						connection.failed = true; ++stats_.failed_jobs;
						stats_.failed_bytes += job.size - job.offset;
						stats_.last_failure = TransmissionFailure::Expired;
						stats_.last_native_error = 0;
					}

					releaseJob(job); continue;
				}
				bool earlier = false;
				for(const auto &other : jobs_)
					if(other.state != State::Free && other.connection == job.connection && other.serial < job.serial)
						earlier = true;

				if(earlier) continue;

				// Queue age is not a socket stall. Start the stall clock only
				// when this unit becomes the connection head, then reset on I/O.
				if(!job.head_started) { job.head_started = true; job.last_progress = now; }

				if(now - job.last_progress >= options_.stall_timeout_ms)
				{
					connection.failed = true;
					++stats_.failed_jobs; stats_.failed_bytes += job.size - job.offset;
					stats_.last_failure = TransmissionFailure::Stalled;
					stats_.last_native_error = 0;
					releaseJob(job); continue;
				}

				const auto candidate = fairness_turn ? UINT64_MAX - job.serial :
				    static_cast<std::uint64_t>(job.policy.priority) * 16 +
				    std::min<std::uint64_t>((now - job.queued_at) / 20, 255);
				if(selected == jobs_.size() || candidate > score) { selected = index; score = candidate; }
			}
			if(selected != jobs_.size())
			{
				// Count actual native attempts, not empty/blocked scans: otherwise
				// retry waits could consume the oldest job's fairness turn.
				++dispatches_;
				jobs_[selected].state = State::Writing;
				handle = connections_[jobs_[selected].connection].handle;
				cursor_ = (selected + 1) % options_.maximum_blocks;
			}
		}
		if(selected == jobs_.size())
		{
			return pending_ready ? 1 : UINT32_MAX;
		}

		auto &job = jobs_[selected];
		const auto wanted = std::min(job.size - job.offset, options_.step_bytes);
		const auto result = native_.writeSome(handle, (job.immutable ? job.immutable : job.bytes) + job.offset, wanted);
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			if(result.status == IoStatus::Progress && result.size && result.size <= wanted)
			{
				job.offset += result.size; job.last_progress = tcpMilliseconds();
				stats_.sent_bytes += result.size;
				auto &connection = connections_[job.connection];
				connection.sent_bytes += std::min<std::uint64_t>(result.size, UINT64_MAX - connection.sent_bytes);
				if(job.offset == job.size) { ++stats_.completed_jobs; releaseJob(job); }
				else job.state = State::Ready;
			}
			else if(!result.size && (result.status == IoStatus::WouldBlock || result.status == IoStatus::Interrupted))
			{
				job.state = State::Ready;
				job.retry_at = tcpMilliseconds() + 4;
				++stats_.would_block_count;
			}
			else
			{
				auto &connection = connections_[job.connection];
				connection.failed = true; connection.native_error = result.native_error;
				stats_.last_failure = TransmissionFailure::NativeWrite;
				stats_.last_native_error = result.native_error;
				++stats_.failed_jobs; stats_.failed_bytes += job.size - job.offset; releaseJob(job);
			}
		}
		return result.status == IoStatus::Progress ? 0 : 1;
	}

	void QueuedTransport::clearReady()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		for(auto &job : jobs_)
			if(job.state != State::Free && job.state != State::Filling) releaseJob(job);
	}

	void QueuedTransport::run()
	{
		while(!stopping_.load())
		{
			const auto delay = pumpStep();
			// Empty dedicated queues sleep until a latched commit/stop wakeup;
			// blocked writes retain the existing bounded retry cadence.
			if(delay) (void)signal_.wait(delay);
		}
		clearReady();
	}
}

namespace NaWServer::tcp
{
	std::size_t transmissionMetadataBytes(std::size_t connection_capacity, std::size_t job_capacity) noexcept
	{
		return detail::QueuedTransport::metadataBytes(connection_capacity, job_capacity);
	}
}
