// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <NaWServer/backends/tcp_transport.h>
#include <NaWServer/detail/synchronization.h>
#include <NaWServer/detail/worker_signal.h>
#include "platform/worker.h"
#include <atomic>

namespace NaWServer::tcp::detail
{

	// All protocol writers use one bounded job table. Only this owner calls the
	// native write operation; readers retain their existing socket ownership.
	class QueuedTransport final : public Transport
	{
		static constexpr std::size_t maximum_job_capacity = 32;
		enum class State { Free, Filling, Ready, Writing };
		struct Connection
		{
			Handle handle;
			std::uint64_t generation = 0;
			std::uint64_t sent_bytes = 0;
			bool closing = false, failed = false, aborted = false;
			int native_error = 0;
			WritePolicy policy;
		};
		struct Job
		{
			State state = State::Free;
			std::size_t connection = 0;
			std::uint64_t generation = 0, serial = 0;
			std::uint8_t *bytes = nullptr;
			const std::uint8_t *immutable = nullptr;
			std::size_t capacity = 0, size = 0, offset = 0;
			std::uint64_t queued_at = 0, last_progress = 0, retry_at = 0;
			WritePolicy policy;
			bool fixed = false;
			bool head_started = false;
		};
		template<class T> struct Table
		{
			T *data = nullptr;
			std::size_t count = 0;
			std::size_t size() const { return count; }
			T *begin() const { return data; }
			T *end() const { return data ? data + count : nullptr; }
			T &operator[](std::size_t index) const { return data[index]; }
		};
		Transport &native_;
		TransmissionOptions options_;
		BufferAllocator allocator_;
		BufferAllocator metadata_allocator_;
		mutable NaWServer::detail::Mutex mutex_;
		void *metadata_ = nullptr;
		std::size_t metadata_bytes_ = 0;
		Table<Connection> connections_;
		Table<Job> jobs_;
		std::atomic<bool> stopping_{false};
		std::atomic<bool> started_{false};
		std::atomic_flag pumping_ = ATOMIC_FLAG_INIT;
		WorkerSignal signal_;
		Worker *worker_ = nullptr;
		TransmissionStats stats_;
		std::uint64_t generation_ = 0, serial_ = 0;
		std::size_t cursor_ = 0;
		std::size_t dispatches_ = 0;
		bool valid_ = false;

		std::size_t find(Handle handle) const;
		bool registerHandle(Handle handle);
		std::uint8_t *allocatePayload(std::size_t bytes);
		void releaseJob(Job &job);
		bool pending(std::size_t connection) const;
		bool cancelPending(std::size_t connection);
		static bool commitLease(void *, std::size_t, std::uint64_t, std::size_t, WritePolicy);
		static void cancelLease(void *, std::size_t, std::uint64_t);
		static void entry(void *);
		// Performs at most one native write; returns the dedicated worker's
		// next retry delay, or UINT32_MAX when no committed work remains.
		std::uint32_t pumpStep();
		void clearReady();
		void run();
	public:
		static std::size_t metadataBytes(std::size_t connection_capacity, std::size_t job_capacity) noexcept;
		QueuedTransport(Transport &native, TransmissionOptions options, std::size_t connection_capacity = 64);
		~QueuedTransport() override;
		bool valid() const { return valid_; }
		bool start();
		bool stop(std::uint32_t timeout_ms);
		TransmissionStats stats() const;
		ListenerResult openListener(const char *, std::uint16_t, int) override;
		AcceptResult accept(Handle) override;
		int listenerError(Handle handle) override { return native_.listenerError(handle); }
		PeerAddress peerAddress(Handle handle) override { return native_.peerAddress(handle); }
		IoResult readSome(Handle handle, std::uint8_t *bytes, std::size_t size) override;
		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override;
		IoResult writeImmutableSome(Handle, const std::uint8_t *, std::size_t) override;
		bool supportsWriteLease() const override { return true; }
		WriteLease acquireWrite(Handle, std::size_t, WritePolicy = {}) override;
		bool flushWrites(Handle, std::uint64_t deadline) override;
		WritePressure writePressure(Handle) const override;
		void setWritePolicy(Handle, WritePolicy) override;
		std::uint64_t transmissionMilliseconds() const override { return tcpMilliseconds(); }
		WaitResult wait(const Interest *, Ready *, std::size_t, std::uint32_t) override;
		OperationResult shutdownWrite(Handle handle) override;
		CloseResult close(Handle &handle) override;
		CloseResult abort(Handle &handle) override;
	};
}
