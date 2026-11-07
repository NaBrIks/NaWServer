// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <NaWServer/detail/synchronization.h>
#include <set>
#include <NaWServer/handler.h>
#include <NaWServer/request.h>
#include <NaWServer/websocket_backend.h>

namespace NaWServer
{
	struct WebSocketBroadcastResult
	{
		size_t queued{};
		size_t rejected{};
		// Meaningful only when rejected != 0. No clients is {0, 0, Queued}.
		WebSocketSendResult first_error{WebSocketSendResult::Queued};
	};
	class WebSocketClient;
	class WebSocketEndpoint;

	// Backend-oriented admission. Endpoint/session outlive this move-only lease.
	// Prepare owns its client/container node but exposes no active broadcast client.
	// Rollback invalidates the token without socket I/O or application callbacks.
	class WebSocketReservation final
	{
		friend class WebSocketEndpoint;
		WebSocketEndpoint *owner_ = nullptr;
		WebSocketClient *client_ = nullptr;
		std::set<WebSocketClient *>::node_type node_;
		bool committed_ = false;

		explicit WebSocketReservation(WebSocketEndpoint *owner);

	public:
		WebSocketReservation() = default;
		~WebSocketReservation();
		WebSocketReservation(const WebSocketReservation &) = delete;
		WebSocketReservation &operator=(const WebSocketReservation &) = delete;
		WebSocketReservation(WebSocketReservation &&other) noexcept;
		WebSocketReservation &operator=(WebSocketReservation &&other) noexcept;

		bool valid() const;
		bool committed() const;
		WebSocketEndpoint *owner() const;
		// Commit only through the original handler's post-101 opening dispatch.
		// Library commit allocates nothing; application onConnect may allocate.
		bool commit(WebSocketExchange &exchange);
		// Rollback after a successful commit is a no-op: the transport owns the client.
		void rollback();
	};

	class WebSocketEndpoint : public HandlerStaticUrl
	{
		friend class WebSocketClient;
		friend class WebSocketReservation;
		mutable NaWServer::detail::Mutex mutex_;
		std::set<WebSocketClient *> sessions_;
		std::size_t reserved_clients_ = 0;
		std::function<void(WebSocketClient *)> connected_;
		std::function<void(WebSocketClient *, const uint8_t *, size_t, bool)> message_;
		WebSocketLimits limits_;
		bool commit(WebSocketReservation &reservation, WebSocketExchange &exchange);

	public:
		explicit WebSocketEndpoint(std::string url, WebSocketLimits limits = {})
		  : HandlerStaticUrl(std::move(url)), limits_(limits)
		{
		}

		WebSocketEndpoint(IHandlerContainer &container, std::string url, WebSocketLimits limits = {})
		  : HandlerStaticUrl(container, std::move(url)), limits_(limits)
		{
		}

		const char *websocket_uri() const override
		{
			return url_.c_str();
		}

		WebSocketEndpoint *websocketEndpoint() override
		{
			return this;
		}

		HttpError validate() const override;
		// Transport session construction is outside the endpoint lock. This step
		// reserves client/node before 101; active+reserved obey max_clients.
		// shared_ptr/container admission still needs sufficient heap; this is not
		// a recoverable global-OOM guarantee when C++ exceptions are disabled.
		WebSocketReservation prepare(std::shared_ptr<WebSocketSession> session);
		const WebSocketLimits &limits() const
		{
			return limits_;
		}

		// Backends invoke this exactly once when the transport closes a client.
		static void disconnect(void *client);
		void handleRequest(Request *r, Response &response) override;
		void onConnect(std::function<void(WebSocketClient *)> fn)
		{
			connected_ = std::move(fn);
		}

		void onMessage(std::function<void(WebSocketClient *, const uint8_t *, size_t, bool)> fn)
		{
			message_ = std::move(fn);
		}

		// Queued counts copied jobs, not peer delivery. No retries or extra queue.
		[[nodiscard]] WebSocketBroadcastResult send(const char *message) const;
		[[nodiscard]] WebSocketBroadcastResult send_binary(const uint8_t *message, size_t len) const;
		[[nodiscard]] WebSocketBroadcastResult send_binary(const uint8_t *message, size_t len, WritePolicy policy) const;
		size_t count() const
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			return sessions_.size();
		}

	};
	class WebSocketClient
	{
		friend class WebSocketEndpoint;
		WebSocketEndpoint *owner_;
		std::shared_ptr<WebSocketSession> session_;
		WebSocketClient(WebSocketEndpoint *owner, std::shared_ptr<WebSocketSession> session)
		  : owner_(owner), session_(std::move(session))
		{
		}

	public:
		// Borrowed only during callbacks. Never retain this pointer across requests.
		[[nodiscard]] WebSocketSendResult send(const char *message);
		[[nodiscard]] WebSocketSendResult send_binary(const uint8_t *message, size_t len);
		[[nodiscard]] WebSocketSendResult send_binary(const uint8_t *message, size_t len, WritePolicy policy);
		// Borrowed for backend receive state only; valid during the callback.
		WebSocketSession &transportSession() const
		{
			return *session_;
		}

	};
} // namespace NaWServer
