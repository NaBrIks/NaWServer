// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#ifndef NAWSERVER_TEST_TCP
#include <NaWServer/backends/windows.h>
#endif
#ifndef NAWSERVER_TEST_TCP
#include <NaWServer/backends/windows_sse.h>
#endif
#ifdef NAWSERVER_TEST_TCP
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#endif
#include <NaWServer/handler_static.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/sse.h>
#include <NaWServer/websocket.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sockets = NaWServer::test::host;

using namespace NaWServer;
namespace
{
	unsigned checked = 0;
	void check(bool value, int line)
	{
		++checked;
		if(!value)
		{
			std::printf("WINDOWS_SSE_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)
	void sendAll(sockets::Handle socket, const std::string &data)
	{
		std::size_t offset = 0;
		while(offset < data.size())
		{
			const int sent = sockets::sendSome(socket, data.data() + offset, static_cast<int>(data.size() - offset), 0);
			CHECK(sent > 0);
			offset += static_cast<std::size_t>(sent);
		}
	}

	sockets::Handle connectTo(std::uint16_t port)
	{
		const sockets::Handle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(socket != sockets::invalid);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		CHECK(connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
		CHECK(sockets::setTimeouts(socket, 3000));
		return socket;
	}

	std::string readToEnd(sockets::Handle socket)
	{
		std::string result;
		char bytes[1024];
		for(;;)
		{
			const int received = sockets::receiveSome(socket, bytes, sizeof(bytes), 0);
			CHECK(received >= 0);
			if(received == 0) break;

			result.append(bytes, static_cast<std::size_t>(received));
		}
		return result;
	}

	std::string readTo(sockets::Handle socket, std::string_view marker)
	{
		std::string result;
		char byte;
		while(result.find(marker) == result.npos)
		{
			const int received = sockets::receiveSome(socket, &byte, 1, 0);
			CHECK(received == 1);
			result.push_back(byte);
		}
		return result;
	}

	std::string performHttp(std::uint16_t port, const std::string &request)
	{
		const sockets::Handle socket = connectTo(port);
		sendAll(socket, request);
		const auto response = readToEnd(socket);
		sockets::closeSocket(socket);
		return response;
	}

	sockets::Handle connectWebSocket(std::uint16_t port, std::string &response)
	{
		const sockets::Handle socket = connectTo(port);
		sendAll(socket, "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
		    "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
		    "dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
		response = readTo(socket, "\r\n\r\n");
		return socket;
	}

	struct SseOpenEndpoint
	{
		sse::EventSource &events;
		std::mutex mutex;
		sse::Token token;
		std::atomic<unsigned> opens{0};

		explicit SseOpenEndpoint(sse::EventSource &events)
		  : events(events)
		{
		}

		SendResult operator()(Request &request, Response &response)
		{
			const auto result = events.open(request, response);
			if(result.active)
			{
				std::lock_guard<std::mutex> lock(mutex);
				token = result.token;
				++opens;
			}

			return result.result;
		}

		sse::Token currentToken()
		{
			std::lock_guard<std::mutex> lock(mutex);
			return token;
		}

	};

	struct ReserveRollbackEndpoint
	{
		sse::Backend &adapter;
		std::atomic<unsigned> reservations{0};

		SendResult operator()(Request &request, Response &response)
		{
			sse::Frame preface;
			if(sse::encode({"reserved", std::nullopt, std::nullopt, std::nullopt}, preface) !=
			    sse::EncodeResult::Ok)
				return SendResult::TransportError;

			{
				sse::Backend::Reservation reservation;
				if(adapter.reserve(request, preface, reservation) != sse::Admit::Owned)
					return SendResult::TransportError;

				++reservations;
			}

			return response.text(409, "ordinary response after rollback");
		}

	};

	struct DuplicateResponseEndpoint
	{
		sse::EventSource &events;
		std::atomic<unsigned> opens{0};
		std::atomic<int> duplicate_result{-1};

		SendResult operator()(Request &request, Response &response)
		{
			const auto result = events.open(request, response);
			if(result.active)
				++opens;

			duplicate_result = static_cast<int>(response.text(409, "must not be sent"));
			return result.result;
		}

	};

	bool waitFor(const std::atomic<unsigned> &value, unsigned expected)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while(value.load() < expected && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return value.load() >= expected;
	}

	bool waitForInactive(sse::EventSource &events)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while(events.active() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return events.active() == 0;
	}

	bool completed(const sse::AdapterStats &stats, std::size_t expected)
	{
		return stats.active == 0 && stats.queued_bytes == 0 && stats.completed >= expected;
	}

	// The clock and pause are injected so a stuck session cannot be hidden by a retry.
	template<class ReadStats, class Now, class Pause>
	bool waitForCompletion(ReadStats read_stats, Now now, Pause pause,
	    std::size_t expected, std::chrono::milliseconds timeout)
	{
		const auto deadline = now() + timeout;
		for(;;)
		{
			if(completed(read_stats(), expected)) return true;

			if(now() >= deadline) return false;

			pause();
		}
	}

	bool waitForCompletion(sse::EventSource &events, std::size_t expected)
	{
		const bool result = waitForCompletion(
			[&events] {
			return events.stats();
		},
			[] {
			return std::chrono::steady_clock::now();
		},
			[] {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		},
			expected, std::chrono::seconds(2));
		if(!result)
		{
			const auto stats = events.stats();
			std::printf("SSE_COMPLETION_TIMEOUT active=%zu queued=%zu begun=%zu completed=%zu expected=%zu\n",
			    stats.active, stats.queued_bytes, stats.begun, stats.completed, expected);
		}

		return result;
	}

	void completionWaitRegression()
	{
		using Milliseconds = std::chrono::milliseconds;
		Milliseconds time{0};
		sse::AdapterStats stats;
		stats.active = 1;
		stats.begun = 1;
		auto read_stats = [&stats] {
				return stats;
			};
		auto now = [&time] {
				return time;
			};
		auto pause = [&time] {
				time += Milliseconds(1);
			};

		// A genuine outstanding session reaches the deadline and fails the same waiter.
		CHECK(!waitForCompletion(read_stats, now, pause, 1, Milliseconds(3)));
		CHECK(time == Milliseconds(3));
		stats.active = 0;
		time = Milliseconds(0);
		CHECK(!waitForCompletion(read_stats, now, pause, 1, Milliseconds(3)));
		CHECK(time == Milliseconds(3));

		// Model peer EOF before local completion: success only follows the observed counter.
		time = Milliseconds(0);
		CHECK(waitForCompletion(read_stats, now, [&] {
			time += Milliseconds(1);
			if(time == Milliseconds(2)) stats.completed = 1;
		}, 1, Milliseconds(3)));
		CHECK(time == Milliseconds(2));
	}

}

int main()
{
	completionWaitRegression();
	CHECK(sockets::initialize() == 0);
#ifdef NAWSERVER_TEST_TCP
	sockets::TcpTransport transport;
	CHECK(transport.start().status == tcp::Status::Ok);
	TcpOptions options;
	options.port = 0;
	TcpHttpBackend backend(transport, options);
	sse::TcpAdapter adapter(backend);
#else
	WindowsHttpBackend backend(0);
	sse::WindowsAdapter adapter;
#endif
	sse::EventSource events(adapter);
	SseOpenEndpoint open_endpoint{events};
	ReserveRollbackEndpoint rollback_endpoint{adapter};
	DuplicateResponseEndpoint duplicate_endpoint{events};
	FixedBasicAuthenticator credentials("user", "password", 1);
	AuthMiddleware authentication(credentials);
	auto chain = makeHeaderChain(authentication);
	Route events_route("/events", HttpMethod::Get, EndpointRef(open_endpoint), {}, AccessPolicy::require(1), {}, chain);
	Route rollback_route("/events-reserve", HttpMethod::Get, EndpointRef(rollback_endpoint), {},
	    AccessPolicy::require(1), {}, chain);
	Route duplicate_route("/events-abort", HttpMethod::Get, EndpointRef(duplicate_endpoint), {},
	    AccessPolicy::require(1), {}, chain);
	WebSocketEndpoint websocket("/ws");
	HandlerStaticUriText health("/health", "http-alive");
	HandlerBase *const routes[]{&events_route, &rollback_route, &duplicate_route, &websocket, &health};
	HttpServer server(backend, routes);
	CHECK(events.start());
	CHECK(server.begin() == HttpError::Ok);
	const auto port = backend.port();
	CHECK(port != 0);

	const auto denied = performHttp(port, "GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n");
	CHECK(denied.find("HTTP/1.1 401") == 0);
	CHECK(events.active() == 0 && open_endpoint.opens == 0);

	const auto rollback_response = performHttp(port,
	        "GET /events-reserve HTTP/1.1\r\nHost: localhost\r\n"
	        "Authorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\n");
	CHECK(rollback_endpoint.reservations == 1);
	CHECK(rollback_response.find("HTTP/1.1 409") == 0);
	CHECK(rollback_response.find("ordinary response after rollback") != rollback_response.npos);
	CHECK(events.active() == 0 && events.stats().rolled_back == 1);

	const sockets::Handle aborted_socket = connectTo(port);
	sendAll(aborted_socket, "GET /events-abort HTTP/1.1\r\nHost: localhost\r\n"
	    "Authorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\n");
	CHECK(waitFor(duplicate_endpoint.opens, 1));
	CHECK(duplicate_endpoint.duplicate_result == static_cast<int>(SendResult::AlreadySent));
	const auto aborted_response = readToEnd(aborted_socket);
	CHECK(aborted_response.empty() || aborted_response.find("HTTP/1.1 200") == 0);
	CHECK(aborted_response.find("HTTP/1.1 409") == aborted_response.npos);
	CHECK(aborted_response.find("must not be sent") == aborted_response.npos);
	sockets::closeSocket(aborted_socket);
	CHECK(waitForInactive(events));

	const sockets::Handle sse_socket = connectTo(port);
	sendAll(sse_socket, "GET /events HTTP/1.1\r\nHost: localhost\r\n"
	    "Authorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\n");
	const auto sse_headers = readTo(sse_socket, "\r\n\r\n");
	CHECK(sse_headers.find("HTTP/1.1 200") == 0);
	CHECK(sse_headers.find("Content-Type: text/event-stream\r\n") != sse_headers.npos);
	CHECK(waitFor(open_endpoint.opens, 1) && events.active() == 1);

	CHECK(server.end() == HttpError::InvalidState);
	std::string websocket_response;
	const sockets::Handle websocket_socket = connectWebSocket(port, websocket_response);
	CHECK(websocket_response.find("HTTP/1.1 101 Switching Protocols\r\n") == 0);
	const auto http_response = performHttp(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
	CHECK(http_response.find("HTTP/1.1 200") == 0 && http_response.find("http-alive") != http_response.npos);

	sse::Frame frame;
	CHECK(sse::encode({"hello", std::nullopt, std::nullopt, std::nullopt}, frame) == sse::EncodeResult::Ok);
	CHECK(events.publish(open_endpoint.currentToken(), frame) == sse::Publish::Queued);
	const auto event = readTo(sse_socket, "data: hello\n\n");
	CHECK(event.find("data: hello\n\n") != event.npos);
	sockets::closeSocket(websocket_socket);

	const auto expected_completions = events.stats().begun;
	// Deliberately retain a real session: the completion gate must reject this leak state.
	std::chrono::milliseconds retained_time{0};
	CHECK(!waitForCompletion(
			[&events] {
		return events.stats();
	},
			[&retained_time] {
		return retained_time;
	},
			[&retained_time] {
		retained_time += std::chrono::milliseconds(1);
	},
			expected_completions, std::chrono::milliseconds(3)));
	CHECK(events.close(open_endpoint.currentToken()));
	CHECK(readToEnd(sse_socket).empty());
	sockets::closeSocket(sse_socket);
	// EOF is emitted inside Stream::finish, before the worker updates local completion.
	CHECK(waitForCompletion(events, expected_completions));

	const sockets::Handle draining_socket = connectTo(port);
	sendAll(draining_socket, "GET /events HTTP/1.1\r\nHost: localhost\r\n"
	    "Authorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\n");
	const auto draining_headers = readTo(draining_socket, "\r\n\r\n");
	CHECK(draining_headers.find("HTTP/1.1 200") == 0);
	CHECK(waitFor(open_endpoint.opens, 2) && events.active() == 1);
	CHECK(events.stop(2000));
	CHECK(readToEnd(draining_socket).empty());
	sockets::closeSocket(draining_socket);
	CHECK(events.active() == 0);
	CHECK(events.stats().completed == events.stats().begun);
	CHECK(server.end() == HttpError::Ok);
#ifdef NAWSERVER_TEST_TCP
	CHECK(adapter.close_errors() == 0);
	CHECK(transport.stop().status == tcp::Status::Ok);
#endif
	CHECK(sockets::cleanup() == 0);
	std::printf("WINDOWS_SSE checks=%u PASS\n", checked);
}
