// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/request.h>
#include <NaWServer/sse.h>
#include "support/tcp_scripted_transport.h"
#include <cstdio>
#include <cstdlib>

using namespace NaWServer;

namespace
{

	unsigned checks = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_SSE_WORKER_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	class SseExchange final : public sse::Exchange
	{
	public:
		const void *identity = nullptr;
		unsigned retains = 0;

		const void *owner() const override
		{
			return identity;
		}

		std::shared_ptr<sse::Stream> retain() override
		{
			++retains;
			return {};
		}

	};

	class HttpTestExchange final : public HttpExchange
	{
		SseExchange &sse_;
	public:
		explicit HttpTestExchange(SseExchange &exchange)
		  : sse_(exchange)
		{
		}

		HttpRequestHead head() const override
		{
			return {HttpMethod::Get, "/events", 0};
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
			return {};
		}

		void *extension(const void *key) const override
		{
			return key == &sse::exchange_key ? &sse_ : nullptr;
		}

	};

	void startBeforeHttpAndRestart()
	{
		tcp::test::ScriptedTransport transport;
		TcpOptions options;
		options.port = 0;
		TcpHttpBackend backend(transport, options);
		sse::TcpAdapter adapter(backend);
		sse::EventSource source(adapter);
		CHECK(backend.port() == 0 && source.stop(0));
		for(unsigned cycle = 0; cycle < 100; ++cycle)
		{
			CHECK(source.start() && !source.start());
			SseExchange wrong_owner;
			wrong_owner.identity = &transport;
			HttpTestExchange wrong_http(wrong_owner);
			Request wrong_request(wrong_http);
			Response wrong_response({});
			const auto wrong = source.open(wrong_request, wrong_response);
			CHECK(!wrong.active && wrong.admission == sse::Admit::InvalidRequest);
			CHECK(wrong.result == SendResult::InvalidArgument && wrong_owner.retains == 0);
			CHECK(wrong_response.state() == ResponseState::Preparing && source.active() == 0);

			SseExchange matching_owner;
			matching_owner.identity = &backend;
			HttpTestExchange matching_http(matching_owner);
			Request matching_request(matching_http);
			Response matching_response({});
			const auto missing = source.open(matching_request, matching_response);
			CHECK(!missing.active && missing.admission == sse::Admit::NoMemory && matching_owner.retains == 1);
			CHECK(source.active() == 0 && source.stats().begun == 0 && source.completions() == 0);
			CHECK(source.stop(2000) && source.close_errors() == 0);
			CHECK(backend.port() == 0 && transport.owned() == 0 && transport.failures() == 0);
		}
		CHECK(transport.calls(tcp::test::Operation::OpenListener) == 0);
		CHECK(transport.calls(tcp::test::Operation::Accept) == 0);
		CHECK(source.stop(0) && backend.stop() == HttpError::Ok);
	}

	void invalidLimitsStartNothing()
	{
		tcp::test::ScriptedTransport transport;
		TcpHttpBackend backend(transport);
		sse::Limits limits;
		limits.clients = 0;
		sse::TcpAdapter adapter(backend, limits);
		sse::EventSource source(adapter);
		CHECK(!source.start() && source.stop(0));
		CHECK(source.active() == 0 && source.close_errors() == 0);
		CHECK(transport.owned() == 0 && transport.failures() == 0);
	}

} // namespace

int main()
{
	startBeforeHttpAndRestart();
	invalidLimitsStartNothing();
	std::printf("TCP_SSE_WORKER checks=%u restart_cycles=100 adapter_bytes=%zu PASS\n",
	    checks, sizeof(sse::TcpAdapter));
}
