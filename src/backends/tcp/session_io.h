// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp_transport.h>
#include <NaWServer/http_backend.h>
#include <atomic>

namespace NaWServer::tcp::detail
{

	struct Clock
	{
		void *context = nullptr;
		std::uint64_t (*milliseconds)(void *) = nullptr;
	};

	enum class IoFailure { None, Timeout, Stopped, PeerClosed, Transport };

	// One synchronous owner; no handle ownership transfer or close occurs here.
	// The absolute exchange deadline never extends. Stall time resets on a phase
	// change and positive I/O progress, not on readiness or interrupted operations.
	class SessionIo
	{
		enum class Phase { None, Read, Write };
		Transport &transport_;
		Handle handle_;
		const std::atomic<bool> &stop_;
		Clock clock_;
		std::uint64_t deadline_;
		std::uint32_t no_progress_timeout_;
		std::uint32_t wait_ceiling_;
		std::uint32_t close_timeout_;
		std::size_t close_drain_bytes_;
		std::uint64_t last_progress_ = 0;
		Phase phase_ = Phase::None;
		IoFailure failure_ = IoFailure::None;
		int native_error_ = 0;

		bool begin(Phase phase);
		bool allowed(std::uint64_t deadline);
		bool wait(bool writing, std::uint64_t deadline);
		void fail(IoFailure failure, int native_error = 0);
		bool writeRange(const std::uint8_t *, std::size_t, bool immutable);

	public:
		SessionIo(Transport &transport, Handle handle, const std::atomic<bool> &stop,
		    Clock clock, std::uint64_t absolute_deadline_ms, std::uint32_t no_progress_timeout_ms,
		    std::uint32_t wait_ceiling_ms = 20, std::uint32_t close_timeout_ms = 50,
		    std::size_t close_drain_bytes = 4096);
		SessionIo(const SessionIo &) = delete;
		SessionIo &operator=(const SessionIo &) = delete;

		BodyReadResult read(std::uint8_t *destination, std::size_t capacity);
		// Additional absolute bound for reject/drain work; it cannot extend total.
		BodyReadResult readUntil(std::uint8_t *destination, std::size_t capacity,
		    std::uint64_t additional_deadline_ms);
		bool write(const std::uint8_t *bytes, std::size_t size);
		bool writeImmutable(const std::uint8_t *bytes, std::size_t size);
		bool writeSource(ResponseSource source);
		bool flush() { return transport_.flushWrites(handle_, deadline_); }
		WriteLease acquireWrite(std::size_t size, WritePolicy policy = {});
		// Configured byte/time discard before close, within the total budget. No
		// separate half-close; a peer waiting for EOF cannot prolong this work.
		// Stop skips this work. The caller remains responsible for the owned close.
		void finish();
		IoFailure failure() const;
		int nativeError() const;
		std::uint64_t now() const;
		std::uint64_t deadline() const;
	};

} // namespace NaWServer::tcp::detail
