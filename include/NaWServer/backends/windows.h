// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/http_backend.h>
#include <NaWServer/sse_backend.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

namespace NaWServer
{
	struct WindowsWebSocketConnection;
	namespace detail
	{
		class WindowsSseExchange;
		class WindowsSseStream;
	}
	// Loopback-only HTTP/1.0/1.1, one request per connection, serialized callbacks.
	// Constructor is inert. Owner must stop before destroying borrowed dependencies.
	class WindowsHttpBackend final : public HttpBackend
	{
		std::uint16_t requested_port_;
		std::uint16_t bound_port_ = 0;
		std::uintptr_t listener_ = static_cast<std::uintptr_t>(-1);
		std::uintptr_t active_ = static_cast<std::uintptr_t>(-1);
		std::vector<std::shared_ptr<WindowsWebSocketConnection> > websocket_connections_;
		std::vector<std::shared_ptr<sse::Stream> > sse_streams_;
		HttpRequestHandler *handler_ = nullptr;
		HttpLimits limits_;
		std::atomic<bool> stopping_{false};
		std::mutex mutex_;
		std::thread worker_;
		friend class detail::WindowsSseExchange;
		friend class detail::WindowsSseStream;
		bool registerSseStream(const std::shared_ptr<sse::Stream> &stream);
		void releaseSseStream(const sse::Stream *stream);
		void run();
	public:
		explicit WindowsHttpBackend(std::uint16_t port = 8080)
		  : requested_port_(port)
		{
		}

		~WindowsHttpBackend() override;

		WindowsHttpBackend(const WindowsHttpBackend &) = delete;
		WindowsHttpBackend &operator=(const WindowsHttpBackend &) = delete;
		HttpError start(HttpRequestHandler &handler, const HttpLimits &limits) override;
		HttpError stop() override;
		std::uint16_t port() const
		{
			return bound_port_;
		}

	};
}
