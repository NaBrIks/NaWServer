// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp_transport.h>
#include <atomic>

namespace NaWServer::tcp
{

	// Explicit Winsock lifetime; start/stop run outside all transport operations.
	// The owner must successfully stop before destroying this object. Handle
	// copies are borrowed and may only be closed by their single designated owner.
	class WindowsTransport final : public Transport
	{
	public:
		WindowsTransport() = default;
		~WindowsTransport() override;

		WindowsTransport(const WindowsTransport &) = delete;
		WindowsTransport &operator=(const WindowsTransport &) = delete;

		OperationResult start();
		OperationResult stop();
		std::size_t ownedHandles() const;

		// A failed setup whose rollback could not close returns Fatal with a valid
		// handle. Its owner must retain this transport and resolve that cleanup.
		ListenerResult openListener(const char *address, std::uint16_t port, int backlog) override;
		AcceptResult accept(Handle listener) override;
		int listenerError(Handle listener) override;
		PeerAddress peerAddress(Handle handle) override;
		IoResult readSome(Handle handle, std::uint8_t *destination, std::size_t capacity) override;
		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override;
		WaitResult wait(const Interest *interests, Ready *ready,
		    std::size_t count, std::uint32_t timeout_ms) override;
		OperationResult shutdownWrite(Handle handle) override;
		CloseResult close(Handle &handle) override;

	private:
		std::atomic<bool> started_{false};
		std::atomic<std::size_t> owned_count_{0};
	};

} // namespace NaWServer::tcp
