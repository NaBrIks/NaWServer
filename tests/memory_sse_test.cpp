// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/memory.h>
#include <NaWServer/backends/memory_sse.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/sse.h>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace NaWServer;
namespace
{
	unsigned checks = 0;
	void check(bool condition, int line)
	{
		++checks;
		if(!condition)
		{
			std::printf("MEMORY_SSE_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	std::string asString(const std::vector<char> &bytes)
	{
		return {bytes.begin(), bytes.end()};
	}

	std::string asString(const std::vector<uint8_t> &bytes)
	{
		return {bytes.begin(), bytes.end()};
	}

	struct OpenEndpoint
	{
		sse::EventSource &source;
		sse::MemoryAdapter &adapter;
		sse::OpenResult result;
		bool reserve_then_rollback = false;
		bool abort_after_open = false;
		SendResult operator()(Request &request, Response &response)
		{
			if(reserve_then_rollback)
			{
				{
					sse::MemoryAdapter::Reservation reservation;
					result.admission = adapter.reserve(request, sse::default_header(), reservation);
				}
				return response.text(200, "ordinary");
			}

			result = source.open(request, response);
			if(abort_after_open && result.active) return response.text(200, "late");

			if(result.active) return SendResult::Ok;

			return response.text(result.admission == sse::Admit::Full ? 503 : 400, "rejected");
		}

	};

	struct Fixture
	{
		MemoryHttpBackend http;
		sse::MemoryAdapter adapter;
		sse::EventSource source{adapter};
		FixedBasicAuthenticator credentials{"user", "password", 1};
		AuthMiddleware auth{credentials};
		AccessMiddleware access;
		HeaderChain<AuthMiddleware, AccessMiddleware> chain{auth, access};
		OpenEndpoint endpoint{source, adapter};
		Route route{"/events", HttpMethod::Get, EndpointRef(endpoint), {}, AccessPolicy::require(1), {}, chain};
		HandlerBase *const routes[1]{&route};
		HttpServer server{http, routes};
		Fixture(std::size_t capture_capacity = 16 * 1024, sse::Limits limits = {})
		  : http(capture_capacity), adapter(limits)
		{
		}

		bool start()
		{
			return source.start() && server.begin() == HttpError::Ok;
		}

		bool stop()
		{
			const bool stopped = source.stop(100);
			return stopped && server.end() == HttpError::Ok;
		}

		MemoryResponse request(std::vector<std::pair<std::string, std::string> > headers = {})
		{
			MemoryRequest request;
			request.target = "/events";
			request.headers = std::move(headers);
			return http.perform(request);
		}

	};

	void authorize(std::vector<std::pair<std::string, std::string> > &headers)
	{
		headers.emplace_back("Authorization", "Basic dXNlcjpwYXNzd29yZA==");
	}

}

int main()
{
	{
		Fixture fixture;
		CHECK(fixture.start());
		auto denied = fixture.request();
		CHECK(denied.status == 401 && !denied.sse_stream && fixture.source.active() == 0);
		std::vector<std::pair<std::string, std::string> > headers;
		authorize(headers);
		headers.emplace_back("Last-Event-ID", "");
		auto response = fixture.request(headers);
		CHECK(response.sse_stream && fixture.endpoint.result.active);
		CHECK(fixture.endpoint.result.last_id.present && fixture.endpoint.result.last_id.size == 0);
		for(unsigned pass = 0; pass < 4; ++pass)
			fixture.adapter.advance(0);
		const auto header = asString(response.sse_stream->take());
		CHECK(header.find("HTTP/1.1 200 OK\r\n") == 0);
		CHECK(header.find("Content-Type: text/event-stream\r\n") != std::string::npos);
		CHECK(header.find("Cache-Control: no-cache\r\n") != std::string::npos);
		CHECK(header.find("Connection: close\r\n") != std::string::npos);
		CHECK(fixture.source.active() == 1);
		CHECK(fixture.source.stop(0));
		CHECK(response.sse_stream->finishCount() == 1 && !response.sse_stream->isOpen());
		CHECK(fixture.server.end() == HttpError::Ok);
	}
	{
		Fixture fixture;
		CHECK(fixture.start());
		std::vector<std::pair<std::string, std::string> > headers;
		authorize(headers);
		auto absent = fixture.request(headers);
		CHECK(fixture.endpoint.result.active && !fixture.endpoint.result.last_id.present);
		CHECK(fixture.source.close(fixture.endpoint.result.token));
		fixture.adapter.advance(0);
		auto invalid = headers;
		invalid.emplace_back("Last-Event-ID", std::string(129, 'i'));
		auto denied = fixture.request(invalid);
		CHECK(denied.status == 400 && !denied.sse_stream);
		invalid.back().second.assign(128, 'i');
		auto valid = fixture.request(invalid);
		CHECK(valid.sse_stream && fixture.endpoint.result.active);
		CHECK(fixture.endpoint.result.last_id.present && fixture.endpoint.result.last_id.size == 128);
		CHECK(fixture.stop());
		CHECK(valid.sse_stream->finishCount() == 1);
		CHECK(fixture.source.start() == false);
	}
	{
		Fixture fixture;
		CHECK(fixture.start());
		std::vector<std::pair<std::string, std::string> > headers;
		authorize(headers);
		auto response = fixture.request(headers);
		auto stream = response.sse_stream;
		CHECK(stream && fixture.endpoint.result.active);
		for(unsigned pass = 0; pass < 4; ++pass)
			fixture.adapter.advance(0);
		(void)stream->take();
		sse::Frame original;
		std::string data{"hello\nworld\r\nline\0end", 21};
		data.append("\n\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", 13);
		CHECK(sse::encode({data, "update", std::nullopt, 100}, original) == sse::EncodeResult::Ok);
		CHECK(fixture.source.publish(fixture.endpoint.result.token, original) == sse::Publish::Queued);
		original.bytes[0] = 'X';
		for(unsigned pass = 0; pass < 8; ++pass)
			fixture.adapter.advance(0);
		const auto event = asString(stream->take());
		CHECK(event.find("event: update\n") != std::string::npos);
		CHECK(event.find("data: hello\ndata: world\ndata: line\0end\n") != std::string::npos);
		CHECK(event.find("data: \xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\n") != std::string::npos);
		CHECK(event.find("retry: 100\n") != std::string::npos);
		CHECK(fixture.source.stats().sent_frames == 1);
		CHECK(fixture.stop());
	}
	{
		sse::Limits limits;
		limits.clients = 1;
		limits.pending = 1;
		Fixture fixture(16 * 1024, limits);
		CHECK(fixture.start());
		std::vector<std::pair<std::string, std::string> > headers;
		authorize(headers);
		auto first = fixture.request(headers);
		auto token = fixture.endpoint.result.token;
		CHECK(first.sse_stream && fixture.source.active() == 1);
		auto full = fixture.request(headers);
		CHECK(full.status == 503 && !full.sse_stream);
		sse::Frame frame;
		CHECK(sse::encode({"event", std::nullopt, std::nullopt, std::nullopt}, frame) == sse::EncodeResult::Ok);
		CHECK(fixture.source.publish(token, frame) == sse::Publish::Queued);
		CHECK(fixture.source.publish(token, frame) == sse::Publish::IndividualOverflow);
		fixture.adapter.advance(0);
		CHECK(first.sse_stream->finishCount() == 1 && fixture.source.active() == 0);
		CHECK(fixture.source.publish(token, frame) == sse::Publish::Closed);
		CHECK(fixture.stop());
	}
	{
		Fixture fixture(128);
		CHECK(fixture.start());
		std::vector<std::pair<std::string, std::string> > headers;
		authorize(headers);
		auto response = fixture.request(headers);
		for(unsigned pass = 0; pass < 4; ++pass)
			fixture.adapter.advance(0);
		response.sse_stream->take();
		sse::Frame frame;
		CHECK(sse::encode({std::string(200, 'x'), std::nullopt, std::nullopt, std::nullopt}, frame) ==
		    sse::EncodeResult::Ok);
		CHECK(fixture.source.publish(fixture.endpoint.result.token, frame) == sse::Publish::Queued);
		for(unsigned pass = 0; pass < 8; ++pass)
			fixture.adapter.advance(0);
		CHECK(response.sse_stream->finishCount() == 1);
		CHECK(fixture.source.stats().closed_by_reason[static_cast<std::size_t>(sse::CloseReason::Disconnected)] == 1);
		CHECK(fixture.stop());
	}
	{
		Fixture fixture;
		CHECK(fixture.start());
		std::vector<std::pair<std::string, std::string> > headers;
		authorize(headers);
		auto response = fixture.request(headers);
		response.sse_stream->failWrites(true);
		fixture.adapter.advance(0);
		CHECK(response.sse_stream->isOpen());
		fixture.adapter.advance(0);
		CHECK(!response.sse_stream->isOpen() && response.sse_stream->finishCount() == 1);
		CHECK(fixture.source.stats().closed_by_reason[static_cast<std::size_t>(sse::CloseReason::Disconnected)] == 1);
		CHECK(fixture.stop());
	}
	{
		Fixture fixture;
		CHECK(fixture.start());
		std::vector<std::pair<std::string, std::string> > headers;
		authorize(headers);
		auto response = fixture.request(headers);
		response.sse_stream->setMaxWrite(0);
		fixture.adapter.advance(0);
		CHECK(response.sse_stream->isOpen());
		fixture.adapter.advance(2000);
		CHECK(!response.sse_stream->isOpen() && response.sse_stream->finishCount() == 1);
		CHECK(fixture.source.stats().closed_by_reason[static_cast<std::size_t>(sse::CloseReason::Stall)] == 1);
		CHECK(fixture.stop());
	}
	{
		Fixture fixture;
		CHECK(fixture.start());
		fixture.endpoint.reserve_then_rollback = true;
		std::vector<std::pair<std::string, std::string> > headers;
		authorize(headers);
		auto response = fixture.request(headers);
		CHECK(response.status == 200 && asString(response.body) == "ordinary");
		CHECK(response.sse_stream && response.sse_stream->rollbackCount() == 1);
		CHECK(response.sse_stream->take().empty() && fixture.source.active() == 0);
		CHECK(fixture.source.stats().rolled_back == 1);
		CHECK(fixture.stop());
	}
	{
		Fixture fixture;
		CHECK(fixture.start());
		fixture.endpoint.abort_after_open = true;
		std::vector<std::pair<std::string, std::string> > headers;
		authorize(headers);
		auto response = fixture.request(headers);
		CHECK(response.sse_stream && fixture.endpoint.result.active);
		CHECK(fixture.source.active() == 1);
		fixture.adapter.advance(0);
		CHECK(!response.sse_stream->isOpen() && response.sse_stream->finishCount() == 1);
		CHECK(fixture.source.stats().closed_by_reason[static_cast<std::size_t>(sse::CloseReason::Requested)] == 1);
		CHECK(fixture.stop());
	}
	std::printf("MEMORY_SSE_PASS checks=%u\n", checks);
}
