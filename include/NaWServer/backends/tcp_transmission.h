// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once

#include <NaWServer/buffer_memory.h>
#include <NaWServer/write_buffer.h>
#include <cstddef>
#include <cstdint>
#include <utility>

#ifndef NAWSERVER_TCP_TRANSMISSION_BLOCK_BYTES
#define NAWSERVER_TCP_TRANSMISSION_BLOCK_BYTES (4u * 1460u)
#endif

namespace NaWServer::tcp
{

	using WritePolicy = NaWServer::WritePolicy;
	using WriteLease = NaWServer::WriteBuffer;

	// Returns the required metadata arena bytes, including internal padding.
	// Zero indicates an invalid capacity or overflow. The supplied arena must
	// return storage aligned to alignof(std::max_align_t).
	std::size_t transmissionMetadataBytes(std::size_t connection_capacity, std::size_t job_capacity) noexcept;

	enum class TransmissionExecution : std::uint8_t { DedicatedWorker, Cooperative };

	struct TransmissionOptions
	{
		bool enabled = false;
		// Experimental cooperative service uses the existing transport wait/flush
		// callers and creates no TX worker. Long application callbacks delay TX;
		// applications must preserve the short, serialized callback contract.
		TransmissionExecution execution = TransmissionExecution::DedicatedWorker;
		std::size_t initial_blocks = 0, maximum_blocks = 12;
		std::size_t block_bytes = NAWSERVER_TCP_TRANSMISSION_BLOCK_BYTES;
		std::size_t maximum_bytes = 32 * 1024;
		std::size_t min_free_bytes = 0;
		std::size_t maximum_blocks_per_connection = 2;
		std::size_t reserved_important_blocks = 1;
		// Zero derives the byte reserve from reserved blocks * block_bytes.
		std::size_t reserved_important_bytes = 0;
		std::uint8_t important_priority = 2;
		std::uint32_t stall_timeout_ms = 1000, close_timeout_ms = 1000;
		std::size_t step_bytes = 1460;
		// Caller-owned aligned payload storage supports pure static or mixed
		// operation. Full blocks are used first; growth uses this allocator.
		std::uint8_t *static_payload = nullptr;
		std::size_t static_payload_bytes = 0;
		BufferAllocator allocator;
		// Metadata is reserved once during backend startup and freed at teardown;
		// this allocator is never called in the transmission hot path.
		// Empty inherits TcpOptions.buffer_memory.allocator, or the platform TCP
		// allocator when that is also empty. A caller-owned arena may be exposed
		// through these callbacks and must outlive the stopped backend
		// and all its write leases. Its allocation must satisfy max_align_t.
		// This limit is independent of payload maximum_bytes and TCP buffer pool
		// limits. It counts requested metadata including table padding, but not
		// allocator bookkeeping. available/allocate_guarded are not used here.
		BufferAllocator metadata_allocator;
		std::size_t metadata_max_bytes = 64 * 1024;
	};

	enum class TransmissionFailure : std::uint8_t { None, Expired, Stalled, NativeWrite, Canceled };
	struct TransmissionStats
	{
		bool enabled = false;
		TransmissionExecution execution = TransmissionExecution::DedicatedWorker;
		std::size_t maximum_jobs = 0, maximum_bytes = 0, block_bytes = 0;
		std::size_t metadata_bytes = 0, metadata_connection_capacity = 0, metadata_job_capacity = 0;
		std::size_t step_bytes = 0;
		std::uint32_t stall_timeout_ms = 0, close_timeout_ms = 0;
		std::size_t maximum_jobs_per_connection = 0, reserved_important_jobs = 0, reserved_important_bytes = 0;
		std::size_t queued_jobs = 0, filling_jobs = 0, allocated_bytes = 0, peak_bytes = 0;
		std::size_t active_jobs = 0, used_bytes = 0, queued_bytes = 0, accepted_bytes = 0;
		std::size_t borrowed_bytes = 0;
		std::size_t sent_bytes = 0, discarded_bytes = 0, failed_bytes = 0, would_block_count = 0;
		std::size_t completed_jobs = 0, discarded_jobs = 0, rejected_jobs = 0, failed_jobs = 0;
		TransmissionFailure last_failure = TransmissionFailure::None;
		int last_native_error = 0;
	};
	struct WritePressure
	{
		std::size_t jobs = 0, bytes = 0;
		bool failed = false;
		// Cumulative bytes accepted by this connection's native sender. Queue
		// admission alone is not network progress; resets with the handle lifetime.
		std::uint64_t sent_bytes = 0;
	};

}
