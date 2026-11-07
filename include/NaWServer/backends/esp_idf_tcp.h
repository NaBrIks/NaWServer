// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp_transport.h>
#include <atomic>

namespace NaWServer::tcp
{

	// Concrete ESP-IDF/lwIP sockets. The application initializes SDK networking
	// before opening sockets; this class never initializes Wi-Fi or esp_netif.
	// Lifecycle calls are serialized outside operations. Socket copies are borrowed.
	class IdfTransport final : public Transport
	{
	public:
		// Caps lwIP's FIN-memory-pressure budget instead of its 20-second default.
		// SDK TCP polling (~500ms), mailbox/scheduler waits remain additional;
		// this is not a hard wall-clock bound or a delivery acknowledgement.
		static constexpr std::uint32_t sdk_send_timeout_ms = 50;
		IdfTransport() = default;
		~IdfTransport() override;
		IdfTransport(const IdfTransport &) = delete;
		IdfTransport &operator=(const IdfTransport &) = delete;

		OperationResult start();
		OperationResult stop();
		std::size_t ownedHandles() const;
		ListenerResult openListener(const char *address, std::uint16_t port, int backlog) override;
		AcceptResult accept(Handle listener) override;
		int listenerError(Handle listener) override;
		PeerAddress peerAddress(Handle handle) override;
		IoResult readSome(Handle handle, std::uint8_t *destination, std::size_t capacity) override;
		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override;
		WaitResult wait(const Interest *interests, Ready *ready,
		    std::size_t count, std::uint32_t timeout_ms) override;
		// Reasserts the finite SDK timeout before at most one native shutdown.
		OperationResult shutdownWrite(Handle handle) override;
		// A guard failure attempts no native close. Native success/EBADF releases;
		// other close errors retain ownership. No linger0, RAW TCP or blind retry.
		CloseResult close(Handle &handle) override;

	private:
		std::atomic<bool> started_{false};
		std::atomic<std::size_t> owned_count_{0};
	};

} // namespace NaWServer::tcp
