// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include "connection_registry.h"
#include "websocket_tx_pool.h"
#include "http/wire/request_parser.h"
#include "websocket/wire/frame_codec.h"
#include <NaWServer/websocket.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace NaWServer::tcp::detail
{

	// HTTP owns handshake/retirement; after handoff only the WS worker performs
	// native I/O until finishIo returns ownership to HTTP. Producers copy once
	// into the bounded TX pool; queued/active output retains its block leases.
	class WebSocketConnection final : public WebSocketSession
	{
		friend struct WebSocketConnectionTestAccess;
	public:
		// The parent validates the handshake before this admission allocation.
		static std::shared_ptr<WebSocketConnection> create(ConnectionRegistry &registry, WebSocketTxPool &pool,
		    Token token,
		    HandlerBase &original_handler, WebSocketEndpoint &endpoint,
		    const http::wire::RequestParser &request, const HttpLimits &limits, std::uint32_t send_timeout_ms,
		    std::uint32_t close_timeout_ms = 50, std::size_t close_drain_bytes = 8192);
		~WebSocketConnection() override;

		Token token() const;
		bool dispatchOpening(WebSocketReservation &reservation);
		// Allocate before 101; enable only after the HTTP owner consumes its prefix.
		bool prepareDeferredCallbacks();
		void enableDeferredCallbacks();
		void progressDeferredInput();
		void dispatchDeferredCallback();
		void finishIo();
		bool ioFinished() const;
		void consumePrefix(ByteView bytes);
		void readReady(std::uint64_t now = 0);
		void writeReady(std::uint64_t now);
		bool wantsRead() const;
		// Selects a bounded active frame and starts/checks its absolute deadline,
		// even if the socket never becomes writable. Call each owner-loop iteration.
		bool wantsWrite(std::uint64_t now);
		bool finished() const;
		void requestClose(std::uint16_t code = 1000);
		void abort();
		// Call exactly once after I/O quiesces. The threaded runtime releases
		// buffers here before native close permits QoS arena reclamation. This
		// method does not close a native handle; outcomes remain in the registry.
		void disconnect();
		WebSocketSendResult sendCopy(const std::uint8_t *bytes, std::size_t size, bool binary) override;
		WebSocketSendResult sendCopyPolicy(const std::uint8_t *, std::size_t, bool, WritePolicy) override;
		void invalidate() override;

	private:
		class Exchange;
		struct Pending
		{
			WebSocketTxPool::Lease block;
			std::size_t size = 0;
			bool binary = false;
		};
		struct Control
		{
			std::array<std::uint8_t, 125> bytes{};
			std::size_t size = 0;
			std::uint8_t opcode = 0;
		};
		enum class Output { None, Data, Control, Close };
		enum class CloseDrain { None, Reading };
		ConnectionRegistry &registry_;
		WebSocketTxPool &pool_;
		Token token_;
		HandlerBase &handler_;
		WebSocketEndpoint &endpoint_;
		WebSocketLimits limits_;
		std::uint32_t send_timeout_;
		std::uint32_t close_timeout_;
		std::size_t close_drain_bytes_;
		NaWServer::detail::BufferArray<std::uint8_t> receive_storage_;
		NaWServer::detail::BufferArray<std::uint8_t> callback_storage_;
		std::atomic<bool> callback_ready_{false};
		std::atomic<bool> io_finished_{false};
		std::atomic<bool> application_close_requested_{false};
		std::size_t callback_size_ = 0;
		bool callback_binary_ = false;
		bool deferred_callbacks_ = false;
		bool staged_input_ = false;
		ByteView staged_payload_;
		bool staged_binary_ = false;
		websocket::wire::FrameDecoder decoder_;
		NaWServer::detail::BufferArray<char> head_storage_;
		NaWServer::detail::BufferArray<http::wire::HeaderEntry> header_entries_;
		http::wire::RequestParser metadata_;
		NaWServer::detail::BufferArray<Pending> pending_;
		mutable NaWServer::detail::Mutex queue_mutex_;
		std::size_t queue_head_ = 0;
		std::size_t queued_ = 0;
		std::size_t pending_count_ = 0;
		std::size_t pending_bytes_ = 0;
		Pending active_data_;
		std::array<Control, 3> controls_;
		std::size_t control_head_ = 0;
		std::size_t control_count_ = 0;
		Control close_;
		Output output_ = Output::None;
		websocket::wire::FrameHeader output_header_;
		std::size_t header_offset_ = 0;
		std::size_t payload_offset_ = 0;
		std::uint64_t output_deadline_ = 0;
		std::uint64_t close_deadline_ = 0;
		std::size_t close_drained_ = 0;
		CloseDrain close_drain_ = CloseDrain::None;
		WebSocketClient *client_ = nullptr;
		std::atomic<bool> accepting_{true};
		std::atomic<bool> aborted_{false};
		std::atomic<bool> finished_{false};
		std::atomic<bool> overload_close_requested_{false};
		bool closing_ = false;
		bool opened_ = false;
		bool disconnected_ = false;
		bool valid_ = false;

		WebSocketConnection(ConnectionRegistry &registry, WebSocketTxPool &pool, Token token, HandlerBase &handler,
		    WebSocketEndpoint &endpoint, std::string_view raw_head, std::size_t header_count,
		    HttpLimits limits, std::uint32_t send_timeout_ms, std::uint32_t close_timeout_ms,
		    std::size_t close_drain_bytes);
		void decoded(const websocket::wire::DecodeResult &result);
		void requestClosePayload(ByteView payload);
		void queueControl(std::uint8_t opcode, ByteView payload);
		bool prepareOutput(std::uint64_t now);
		ByteView outputPayload() const;
		void completeOutput(std::uint64_t now);
		void clearPending();
		void clearQueued();
	};

} // namespace NaWServer::tcp::detail
