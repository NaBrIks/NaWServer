// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <NaWServer/backends/tcp_transmission.h>

namespace NaWServer::tcp
{

	// Native value zero is valid. Copies are borrowed; exactly one owner closes.
	struct Handle
	{
		std::uintptr_t value = 0;
		bool valid = false;
	};
	struct PeerAddress
	{
		// Numeric IPv4 in host order; no DNS or identity/authentication claim.
		std::uint32_t ipv4 = 0;
		std::uint16_t port = 0;
		bool valid = false;
	};
	inline constexpr std::uint8_t peer_address_key = 0;

	enum class Status { Ok, WouldBlock, Interrupted, Fatal, InvalidArgument, InvalidState };
	enum class IoStatus { Progress, WouldBlock, Interrupted, PeerClosed, Fatal, InvalidArgument, InvalidState };
	enum class WaitStatus { Ready, Timeout, Interrupted, Fatal, InvalidArgument, InvalidState };

	struct OperationResult
	{
		Status status = Status::Ok;
		int native_error = 0;
	};

	struct CloseResult
	{
		Status status = Status::Fatal;
		int native_error = 0;
		// An error may still release a Linux descriptor; never retry that close.
		bool released = false;
	};

	struct ListenerResult
	{
		Status status = Status::Fatal;
		int native_error = 0;
		// A valid handle is owned by the caller even on setup/rollback failure.
		Handle handle;
		std::uint16_t port = 0;
		// Exact rollback outcome. nullopt means setup has not attempted a close.
		std::optional<CloseResult> cleanup{};
	};

	struct AcceptResult
	{
		Status status = Status::Fatal;
		int native_error = 0;
		// A failed accepted-socket setup may retain ownership after close failure.
		Handle handle;
		// nullopt means no close attempt; a present Fatal/retained outcome must not
		// trigger blind retry merely because the setup itself also failed.
		std::optional<CloseResult> cleanup{};
	};

	struct IoResult
	{
		IoStatus status = IoStatus::Fatal;
		std::size_t size = 0;
		int native_error = 0;
	};

	struct Interest
	{
		Handle handle;
		bool read = false;
		bool write = false;
	};

	struct Ready
	{
		bool readable = false;
		bool writable = false;
		bool error = false;
		// Socket readiness error, when known; never reuse an unrelated errno.
		int native_error = 0;
	};

	struct WaitResult
	{
		WaitStatus status = WaitStatus::Fatal;
		std::size_t ready_count = 0;
		int native_error = 0;
	};

	// One owner operates a given handle; different handles may have different owners.
	// Constructors are inert. Platform runtime startup/shutdown stays concrete.
	// Addresses are numeric IPv4; port zero asks the OS for an ephemeral port.
	class Transport
	{
	public:
		virtual ~Transport() = default;
		virtual ListenerResult openListener(const char *address, std::uint16_t port, int backlog) = 0;
		virtual AcceptResult accept(Handle listener) = 0;
		// Optional diagnostic for the owned listener after error readiness. Native
		// SO_ERROR queries can consume a pending error: never call for client I/O.
		// Zero means unavailable/no pending native code, not a healthy listener.
		virtual int listenerError(Handle)
		{
			return 0;
		}

		virtual PeerAddress peerAddress(Handle)
		{
			return {};
		}

		// One bounded syscall, nonempty buffers, no retained pointer or internal retry.
		// Only Progress carries bytes: 0 < size <= requested. recv EOF is PeerClosed;
		// send returning zero for nonempty input is Fatal, not successful progress.
		virtual IoResult readSome(Handle handle, std::uint8_t *destination, std::size_t capacity) = 0;
		virtual IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) = 0;
		// Explicit immutable storage must outlive the runtime; queued adapters may
		// borrow it instead of allocating/copying payload blocks.
		virtual IoResult writeImmutableSome(Handle handle, const std::uint8_t *bytes, std::size_t size)
		{
			return writeSome(handle, bytes, size);
		}
		virtual bool supportsWriteLease() const { return false; }
		virtual WriteLease acquireWrite(Handle, std::size_t, WritePolicy = {}) { return {}; }
		virtual bool flushWrites(Handle, std::uint64_t) { return true; }
		virtual WritePressure writePressure(Handle) const { return {}; }
		virtual void setWritePolicy(Handle, WritePolicy) {}
		virtual std::uint64_t transmissionMilliseconds() const { return 0; }
		// Caller owns both arrays of count entries. Clear outputs on every call,
		// including errors. Duplicate interests are allowed; ready_count counts
		// entries with any readiness, rather than native read/write set memberships.
		// Readiness is a hint, timeout is distinct from WouldBlock, count must be >0.
		virtual WaitResult wait(const Interest *interests, Ready *ready,
		    std::size_t count, std::uint32_t timeout_ms) = 0;
		virtual OperationResult shutdownWrite(Handle handle) = 0;
		// At most one native close attempt; a platform safety guard may refuse
		// before that attempt. Invalidate handle iff released. On retained
		// failure keep dependencies alive; do not blindly retry an ambiguous close.
		virtual CloseResult close(Handle &handle) = 0;
		// Terminal owner-only cancellation: no application TX flush or input drain.
		// Native transports already close directly; queued wrappers must first
		// cancel their pending writes and safely relinquish active write leases.
		virtual CloseResult abort(Handle &handle) { return close(handle); }
	};

} // namespace NaWServer::tcp
