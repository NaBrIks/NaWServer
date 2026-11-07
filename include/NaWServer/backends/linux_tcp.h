// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp_transport.h>
#include <atomic>

namespace NaWServer::tcp
{

	// Linux sockets only. Lifecycle calls are serialized outside socket operations;
	// stop refuses live owned handles. No process signal policy is changed.
	class LinuxTransport final : public Transport
	{
	public:
		static constexpr std::size_t maximum_wait_handles = 64;
		LinuxTransport() = default;
		~LinuxTransport() override;
		LinuxTransport(const LinuxTransport &) = delete;
		LinuxTransport &operator=(const LinuxTransport &) = delete;

		OperationResult start();
		OperationResult stop();
		std::size_t ownedHandles() const;
		ListenerResult openListener(const char *address, std::uint16_t port, int backlog) override;
		AcceptResult accept(Handle listener) override;
		int listenerError(Handle listener) override;
		PeerAddress peerAddress(Handle handle) override;
		IoResult readSome(Handle handle, std::uint8_t *destination, std::size_t capacity) override;
		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override;
		// Up to 64 unique descriptors; duplicate interests retain their own outputs.
		// One poll timeout is capped to INT_MAX milliseconds, never made infinite.
		WaitResult wait(const Interest *interests, Ready *ready,
		    std::size_t count, std::uint32_t timeout_ms) override;
		OperationResult shutdownWrite(Handle handle) override;
		// Linux releases the descriptor even on EINTR. Report the error, invalidate
		// the owner's handle and never retry; other POSIX systems can differ.
		CloseResult close(Handle &handle) override;

	private:
		std::atomic<bool> started_{false};
		std::atomic<std::size_t> owned_count_{0};
	};

} // namespace NaWServer::tcp
