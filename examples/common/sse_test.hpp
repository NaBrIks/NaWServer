// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/sse.h>
#include <NaWServer/route.h>
#include <NaWServer/request.h>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace NaWServer::examples
{
	// Shared Windows/C3 diagnostic endpoints. No product state or network setup.
	class SseTest
	{
		sse::EventSource &events_;
		struct PendingConnected
		{
			sse::Token token;
			sse::LastEventId last_id;
			bool pending = false;
		};
		std::array<PendingConnected, sse::client_capacity> connected_{};
		std::mutex mutex_;
		std::size_t connected_busy_ = 0;
		std::size_t connected_rejected_ = 0;

		std::array<sse::Token, sse::client_capacity> tokens(bool cancel_pending = false)
		{
			std::array<sse::Token, sse::client_capacity> result;
			std::lock_guard<std::mutex> lock(mutex_);
			for(std::size_t index = 0; index < result.size(); ++index)
			{
				result[index] = connected_[index].token;
				if(cancel_pending) connected_[index].pending = false;
			}

			return result;
		}

		static sse::EncodeResult encodeConnected(const sse::LastEventId &last, sse::Frame &frame)
		{
			const std::string_view data = last.present ? std::string_view(last.bytes.data(), last.size) : "ready";
			return sse::encode({data, "connected", "boot:1", 1000}, frame);
		}

		struct Open
		{
			SseTest &owner;
			SendResult operator()(Request &request, Response &response)
			{
				if(response.header("X-Sse-Test", "shared") != SendResult::Ok) return response.error();

				const auto opened = owner.events_.open(request, response);
				if(!opened.active)
					return response.state() == ResponseState::External ? opened.result :
					       response.text(opened.admission == sse::Admit::Full ||
					           opened.admission == sse::Admit::Stopping ? 503 : 400,
					           "SSE admission rejected");

				{
					std::lock_guard<std::mutex> lock(owner.mutex_);
					owner.connected_[opened.token.slot] = {opened.token, {}, false};
				}
				sse::Frame connected;
				if(encodeConnected(opened.last_id, connected) != sse::EncodeResult::Ok)
					return SendResult::InvalidArgument;

				const auto published = owner.events_.publish(opened.token, connected);
				if(published != sse::Publish::Queued)
				{
					std::lock_guard<std::mutex> lock(owner.mutex_);
					if(published == sse::Publish::Busy)
					{
						++owner.connected_busy_;
						owner.connected_[opened.token.slot] = {opened.token, opened.last_id, true};
						return SendResult::Ok; // The application owner polls after this callback.
					}

					++owner.connected_rejected_;

					return SendResult::TransportError;
				}

				return SendResult::Ok;
			}

		} open_{*this};
		struct Publish
		{
			SseTest &owner;
			SendResult operator()(Request &request, Response &response)
			{
				const auto body = request.body();
				const auto identifier = request.parameter("id");
				const std::optional<std::string_view> id =
				    identifier ? std::optional<std::string_view>(*identifier) : std::nullopt;
				sse::Frame frame;
				const auto bytes =
				    body.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char *>(body.data()),
				    body.size());
				if(sse::encode({bytes, "reading", id, 1000}, frame) != sse::EncodeResult::Ok)
					return response.text(400, "Invalid SSE event");

				std::size_t queued = 0;
				bool busy = false;
				std::array<std::size_t, 6> refusals{};
				for(const auto token : owner.tokens())
				{
					const auto result = owner.events_.publish(token, frame);
					if(result == sse::Publish::Queued) ++queued;
					else
					{
						++refusals[static_cast<std::size_t>(result)];
						busy = busy || result == sse::Publish::Busy;
					}
				}

				if(queued) return response.text(200, "queued");

				if(busy) return response.text(409, "busy");

				TextBuffer<192> refusal;
				refusal.printf("rejected: closed=%zu too_large=%zu individual_overflow=%zu global_full=%zu",
				    refusals[static_cast<std::size_t>(sse::Publish::Closed)],
				    refusals[static_cast<std::size_t>(sse::Publish::TooLarge)],
				    refusals[static_cast<std::size_t>(sse::Publish::IndividualOverflow)],
				    refusals[static_cast<std::size_t>(sse::Publish::GlobalFull)]);
				return response.text(409, refusal.view());
			}

		} publish_{*this};
		struct Close
		{
			SseTest &owner;
			SendResult operator()(Response &response)
			{
				const auto tokens = owner.tokens(true);
				for(const auto token : tokens)
					(void)owner.events_.close(token);
				return response.text(200, "closing");
			}

		} close_{*this};
		struct Stats
		{
			SseTest &owner;
			SendResult operator()(Response &response)
			{
				const auto value = owner.events_.stats();
				std::size_t busy, rejected;
				{
					std::lock_guard<std::mutex> lock(owner.mutex_);
					busy = owner.connected_busy_;
					rejected = owner.connected_rejected_;
				}
				TextBuffer<320> text;
				text.printf("{\"active\":%zu,\"begun\":%zu,\"completed\":%zu,\"queued_bytes\":%zu,\"rolled_back\":%zu,"
				    "\"connected_busy\":%zu,\"connected_rejected\":%zu}", value.active, value.begun, value.completed,
				    value.queued_bytes, value.rolled_back, busy, rejected);
				return response.send(200, "application/json",
				           {reinterpret_cast<const uint8_t *>(text.view().data()), text.view().size()});
			}

		} stats_{*this};
		struct Stop
		{
			SseTest &owner;
			SendResult operator()(Response &response)
			{
				owner.stop_requested = true; // Owner task performs stop, never this callback.
				return response.text(202, "stop requested");
			}

		} stop_{*this};
	public:
		// Application owner only, outside HTTP callbacks; stop polling before drain.
		// Keep only owned Last-Event-ID metadata per pending client, never a Frame,
		// Request, Response or callback. One stack Frame is encoded per poll step.
		void poll()
		{
			for(std::size_t index = 0; index < connected_.size(); ++index)
			{
				PendingConnected pending;
				{
					std::lock_guard<std::mutex> lock(mutex_);
					if(!connected_[index].pending) continue;

					pending = connected_[index];
				}
				sse::Frame frame;
				const auto encoded = encodeConnected(pending.last_id, frame);
				const auto result = encoded == sse::EncodeResult::Ok ? events_.publish(pending.token, frame) :
				    sse::Publish::TooLarge;
				if(result == sse::Publish::Busy) continue;

				{
					std::lock_guard<std::mutex> lock(mutex_);
					if(connected_[index].pending && connected_[index].token == pending.token)
					{
						connected_[index].pending = false;
						if(result != sse::Publish::Queued && result != sse::Publish::Closed) ++connected_rejected_;
					}
				}
				if(result != sse::Publish::Queued && result != sse::Publish::Closed) (void)events_.close(pending.token);
			}
		}

		std::atomic<bool> stop_requested{false};
		Route open_route, private_route, publish_route, close_route, stats_route, stop_route;
		SseTest(sse::EventSource &events, HeaderPipeline protected_chain)
		  : events_(events),
		    open_route("/events", HttpMethod::Get, EndpointRef(open_)),
		    private_route("/events/private", HttpMethod::Get, EndpointRef(open_), {}, AccessPolicy::authenticated(), {}, protected_chain),
		    publish_route("/events/publish", HttpMethod::Post, EndpointRef(publish_)),
		    close_route("/events/close", HttpMethod::Post, EndpointRef(close_)),
		    stats_route("/events/stats", HttpMethod::Get, EndpointRef(stats_)),
		    stop_route("/events/stop", HttpMethod::Post, EndpointRef(stop_))
		{
		}

	};
}
