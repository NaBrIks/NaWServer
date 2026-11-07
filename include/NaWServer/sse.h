// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/sse_backend.h>
#include <NaWServer/detail/sse/response_bridge.hpp>

namespace NaWServer
{
	class Request;
	namespace sse
	{
		// Bounded SSE service. Backend and handlers outlive a successful stop.
		// Open only behind Route auth/policy. Stop producers and this service before HTTP.
		class EventSource final
		{
			Backend &backend_;
		public:
			explicit EventSource(Backend &backend)
			  : backend_(backend)
			{
			}

			EventSource(const EventSource &) = delete;
			EventSource &operator=(const EventSource &) = delete;
			bool start()
			{
				return backend_.start();
			}

			bool stop(std::uint32_t timeout_ms)
			{
				return backend_.stop(timeout_ms);
			}

			Publish publish(Token token, const Frame &frame)
			{
				return backend_.publish(token, frame);
			}

			bool close(Token token)
			{
				return backend_.close(token);
			}

			std::size_t active()
			{
				return backend_.active();
			}

			std::size_t completions()
			{
				return backend_.completions();
			}

			std::size_t close_errors() const
			{
				return backend_.close_errors();
			}

			AdapterStats stats()
			{
				return backend_.stats();
			}

			[[nodiscard]] OpenResult open(Request &request, Response &response);
		};
	}
}
