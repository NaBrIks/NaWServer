// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/write_buffer.h>
#include <NaWServer/request_body.h>
#include <memory>

namespace NaWServer
{
	enum class WebSocketSendResult : uint8_t { Queued, Closed, InvalidArgument, TooLarge, QueueFull, NoMemory,
		                                       QueueError, Busy, GlobalFull };
	struct WebSocketLimits
	{
		std::size_t max_message_size = 4096;
		unsigned max_pending_messages = 4;
		unsigned max_clients = 4;
		// Queued plus partially transmitted payload bytes for this connection.
		std::size_t max_pending_bytes = 16 * 1024;
	};
	class WebSocketClient;
	class WebSocketReservation;
	// Owned transport token. sendCopy consumes/copies bytes before returning.
	// Queued is acceptance by the backend, not delivery or peer acknowledgement.
	class WebSocketSession
	{
	public:
		virtual ~WebSocketSession() = default;
		virtual WebSocketSendResult sendCopy(const uint8_t *data, std::size_t size, bool binary) = 0;
		virtual WebSocketSendResult sendCopyPolicy(const uint8_t *data, std::size_t size, bool binary, WritePolicy policy)
		{
			if(policy.priority || policy.expires_at_ms || policy.discard_expired || policy.replacement_key)
				return WebSocketSendResult::InvalidArgument;

			return sendCopy(data, size, binary);
		}
		// Revoke/signal only; no socket I/O or close. The single transport I/O owner
		// releases its native connection. Also used by pre-101 admission rollback.
		virtual void invalidate() = 0;
	};
	enum class WebSocketReadStatus { Message, Ignored, Closed, Error };
	struct WebSocketReadResult
	{
		WebSocketReadStatus status;
		ByteView bytes;
		bool binary = false;
	};
	inline constexpr char websocket_exchange_key = 0;
	// Optional HttpExchange extension for an upgraded callback. It owns receive
	// storage until the callback returns. Protocol parsing belongs to the backend.
	class WebSocketExchange
	{
	public:
		virtual ~WebSocketExchange() = default;
		// True exactly once, after a successful upgrade and before data callbacks.
		virtual bool opening() const = 0;
		// Borrowed only for this opening callback. A prepared client/session/node
		// exists before 101; it must belong to this exchange's transport session.
		// The original handler decides whether to commit it.
		virtual WebSocketReservation *preparedAdmission()
		{
			return nullptr;
		}

		virtual std::shared_ptr<WebSocketSession> createSession(const WebSocketLimits &limits) = 0;
		virtual WebSocketClient *client() const = 0;
		// On the prepared path this is a non-fallible, allocation-free publication
		// after full 101; it performs no socket I/O and retains no Request/Response.
		virtual void attach(WebSocketClient *client) = 0;
		virtual WebSocketReadResult readMessage(const WebSocketLimits &limits) = 0;
		virtual void close() = 0;
	};
}
