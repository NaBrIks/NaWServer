// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "../examples/common/sse_test.hpp"
#include <NaWServer/http_server.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
	bool measuring = false;
	unsigned allocations = 0;
}

void *operator new(std::size_t size)
{
	if(measuring) ++allocations;

	if(auto *bytes = std::malloc(size ? size : 1)) return bytes;

	std::abort();
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void *bytes) noexcept
{
	std::free(bytes);
}

void operator delete[](void *bytes) noexcept
{
	std::free(bytes);
}

void operator delete(void *bytes, std::size_t) noexcept
{
	std::free(bytes);
}

void operator delete[](void *bytes, std::size_t) noexcept
{
	std::free(bytes);
}

namespace
{
	using namespace NaWServer;
	unsigned checks = 0;
#define CHECK(value) do { ++checks; \
						  if(!(value)) { std::printf("SSE_TEST_APP_FAIL line=%d\n", __LINE__); std::abort(); \
						  } } while(0)
	class Exchange final : public HttpExchange
	{
		static SendResult send(void *context, int status, const char *, const Header *, std::size_t, ByteView body)
		{
			auto &self = *static_cast<Exchange *>(context);
			CHECK(body.size() <= self.bytes.size());
			if(body.size()) std::memcpy(self.bytes.data(), body.data(), body.size());

			self.size = body.size(); self.status = status;
			return SendResult::Ok;
		}

	public:
		HttpRequestHead request;
		std::array<char, 1024> bytes{};
		std::size_t size = 0;
		int status = 0;
		explicit Exchange(std::string_view target, HttpMethod method = HttpMethod::Get)
		  : request{method, target, 0}
		{
		}

		HttpRequestHead head() const override
		{
			return request;
		}

		HeaderReadResult readHeader(const char *, char *, std::size_t) const override
		{
			return {HeaderReadStatus::NotFound, 0};
		}

		BodyReadResult readBody(std::uint8_t *, std::size_t) override
		{
			return {BodyReadStatus::End, 0};
		}

		ResponseTransport responseTransport() override
		{
			return {this, send};
		}

		std::string_view text() const
		{
			return {bytes.data(), size};
		}

	};
	class Backend final : public sse::Backend
	{
		static void abort(void *context)
		{
			++static_cast<Backend *>(context)->aborted;
		}

	public:
		sse::Token next_token{0, 1};
		sse::LastEventId last_id;
		std::array<sse::Publish, 8> results{};
		std::size_t result_count = 0, result_offset = 0, calls = 0, closed = 0, accepted = 0, aborted = 0;
		std::array<sse::Frame, 8> frames{};
		void (*hook)(void *) = nullptr;
		void *hook_context = nullptr;
		bool start() override
		{
			return true;
		}

		bool stop(std::uint32_t) override
		{
			return true;
		}

		sse::Admit reserve(Request &, const sse::Frame &, Reservation &reservation) override
		{
			bind(reservation, next_token, {this, abort}); reservation.last_id = last_id;
			return sse::Admit::Owned;
		}

		bool activate(Reservation &reservation) override
		{
			release(reservation); return true;
		}

		void rollback(Reservation &reservation) override
		{
			release(reservation);
		}

		sse::Publish publish(sse::Token token, const sse::Frame &frame) override
		{
			if(!token.generation) return sse::Publish::Closed;

			++calls;
			CHECK(result_offset < result_count);
			const auto result = results[result_offset++];
			if(auto callback = std::exchange(hook, nullptr)) callback(hook_context);

			if(result == sse::Publish::Queued) { CHECK(accepted < frames.size()); frames[accepted++] = frame; }

			return result;
		}

		bool close(sse::Token token) override
		{
			if(token.generation)
				++closed;

			return true;
		}

		sse::AdapterStats stats() override
		{
			return {};
		}

		std::size_t close_errors() const override
		{
			return 0;
		}

		void script(std::initializer_list<sse::Publish> values)
		{
			CHECK(values.size() <= results.size()); result_count = values.size(); result_offset = 0;
			std::copy(values.begin(), values.end(), results.begin());
		}

	};
	struct Fixture
	{
		Backend backend;
		sse::EventSource source{backend};
		examples::SseTest application{source, {}};
		HandlerBase *const routes[4]{&application.open_route, &application.publish_route,
			                         &application.close_route, &application.stats_route};
		ExchangeDisposition dispatch(Exchange &exchange)
		{
			return dispatchHttp(exchange, routes, 4, {}, {}, {}, {});
		}

		void open()
		{
			Exchange exchange("/events"); CHECK(dispatch(exchange) == ExchangeDisposition::Complete);
		}

	};
	void ownedDeferred()
	{
		Fixture fixture;
		fixture.backend.last_id.present = true; fixture.backend.last_id.size = 3;
		std::memcpy(fixture.backend.last_id.bytes.data(), "old", 3);
		fixture.backend.script({sse::Publish::Busy, sse::Publish::Busy, sse::Publish::Queued});
		fixture.open(); CHECK(fixture.backend.calls == 1 && fixture.backend.accepted == 0);
		fixture.backend.last_id.bytes.fill('x');
		measuring = true;
		fixture.application.poll(); CHECK(fixture.backend.calls == 2 && fixture.backend.accepted == 0);
		fixture.application.poll(); CHECK(fixture.backend.calls == 3 && fixture.backend.accepted == 1);
		fixture.application.poll(); CHECK(fixture.backend.calls == 3);
		measuring = false; CHECK(allocations == 0);
		const auto &frame = fixture.backend.frames[0];
		CHECK(std::string_view(frame.bytes.data(), frame.size).find("data: old") != std::string_view::npos);
		Exchange stats("/events/stats"); fixture.dispatch(stats);
		CHECK(stats.text().find("\"connected_busy\":1") != std::string_view::npos);
	}

	void generationAndClose()
	{
		Fixture fixture;
		fixture.backend.script({sse::Publish::Busy}); fixture.open();
		fixture.backend.hook_context = &fixture;
		fixture.backend.hook = [](void *context)
			{
				auto &value = *static_cast<Fixture *>(context);
				value.backend.next_token.generation = 2;
				value.open();
			};
		fixture.backend.script({sse::Publish::Queued, sse::Publish::Busy, sse::Publish::Queued});
		fixture.application.poll(); CHECK(fixture.backend.accepted == 1);
		fixture.application.poll(); CHECK(fixture.backend.accepted == 2);
		fixture.application.poll(); CHECK(fixture.backend.accepted == 2);
		fixture.backend.script({sse::Publish::Busy}); fixture.open();
		Exchange close("/events/close", HttpMethod::Post); fixture.dispatch(close);
		const auto calls = fixture.backend.calls; fixture.application.poll();
		CHECK(fixture.backend.calls == calls && fixture.backend.closed == 1);
	}

	void publishRefusals()
	{
		Fixture fixture;
		fixture.backend.script({sse::Publish::Queued}); fixture.open();
		fixture.backend.script({sse::Publish::Busy});
		Exchange busy("/events/publish", HttpMethod::Post); fixture.dispatch(busy);
		CHECK(busy.status == 409 && busy.text() == "busy" && fixture.backend.accepted == 1);
		fixture.backend.script({sse::Publish::GlobalFull});
		Exchange full("/events/publish", HttpMethod::Post); fixture.dispatch(full);
		CHECK(full.status == 409 && full.text() != "busy" && full.text().find("global_full=1") != full.text().npos);
		fixture.backend.next_token = {1, 2};
		fixture.backend.script({sse::Publish::Queued}); fixture.open();
		fixture.backend.script({sse::Publish::Queued, sse::Publish::Busy});
		Exchange partial("/events/publish", HttpMethod::Post); fixture.dispatch(partial);
		CHECK(partial.status == 200 && partial.text() == "queued" && fixture.backend.accepted == 3);
	}

	void deferredTermination()
	{
		for(const auto result : {sse::Publish::Closed, sse::Publish::GlobalFull,
		                         sse::Publish::TooLarge, sse::Publish::IndividualOverflow})
		{
			Fixture fixture;
			fixture.backend.script({sse::Publish::Busy, result}); fixture.open();
			fixture.application.poll(); const auto calls = fixture.backend.calls;
			fixture.application.poll(); CHECK(fixture.backend.calls == calls && fixture.backend.accepted == 0);
			CHECK(fixture.backend.closed == (result == sse::Publish::Closed ? 0u : 1u));
		}
	}

	void initialFailure()
	{
		Fixture fixture;
		fixture.backend.script({sse::Publish::GlobalFull});
		Exchange open("/events");
		CHECK(fixture.dispatch(open) == ExchangeDisposition::Complete && fixture.backend.aborted == 1);
		CHECK(fixture.backend.calls == 1 && fixture.backend.accepted == 0);
		fixture.application.poll(); CHECK(fixture.backend.calls == 1);
		Exchange stats("/events/stats"); fixture.dispatch(stats);
		CHECK(stats.text().find("\"connected_rejected\":1") != std::string_view::npos);
	}

}

int main()
{
	ownedDeferred(); generationAndClose(); publishRefusals(); deferredTermination();
	initialFailure();
	std::printf("SSE_TEST_APP_PASS checks=%u SseTest=%zu LastEventId=%zu\n", checks,
	    sizeof(NaWServer::examples::SseTest), sizeof(NaWServer::sse::LastEventId));
}
