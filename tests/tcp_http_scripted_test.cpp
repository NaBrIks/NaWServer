// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "backends/tcp/http_connection.h"
#include "support/tcp_scripted_transport.h"
#include <NaWServer/backends/memory.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace NaWServer;
using namespace NaWServer::tcp;
using namespace NaWServer::tcp::detail;
using namespace NaWServer::tcp::test;
using namespace std::string_literals;

namespace
{

	unsigned checks = 0;
	unsigned exchanges = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_HTTP_SCRIPTED_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	struct Observation
	{
		unsigned health_calls = 0;
		unsigned echo_calls = 0;
		unsigned parameter_calls = 0;
		unsigned twice_calls = 0;
		std::array<std::uint8_t, 4096> copied_body{};
		std::size_t copied_size = 0;
		const char *workspace = nullptr;
		std::size_t workspace_size = 0;
		const std::uint8_t *prepared_body = nullptr;
		bool body_storage_valid = false;
		SendResult first_send = SendResult::InvalidArgument;
		SendResult second_send = SendResult::InvalidArgument;
	};

	Observation *observed = nullptr;

	SendResult health(Response &response)
	{
		++observed->health_calls;
		return response.text(200, "ready");
	}

	SendResult echo(Request &request, Response &response)
	{
		++observed->echo_calls;
		const auto bytes = request.body();
		CHECK(request.copyBodyTo(observed->copied_body) == BodyCopyResult::Ok);
		observed->copied_size = bytes.size();
		const auto address = reinterpret_cast<std::uintptr_t>(bytes.data());
		const auto workspace = reinterpret_cast<std::uintptr_t>(observed->workspace);
		observed->body_storage_valid = observed->prepared_body ? bytes.data() == observed->prepared_body :
		    bytes.empty() || address < workspace || address - workspace >= observed->workspace_size;
		return response.bytes(200, bytes);
	}

	SendResult parameters(Request &request, Response &response)
	{
		++observed->parameter_calls;
		return response.text(200, request.parameter("value").value_or(""));
	}

	SendResult twice(Response &response)
	{
		++observed->twice_calls;
		observed->first_send = response.text(200, "first");
		observed->second_send = response.text(200, "second");
		return observed->second_send;
	}

	struct Application
	{
		MemoryHttpBackend backend;
		FixedBasicAuthenticator credentials{"user", "password", 1};
		AuthMiddleware auth{credentials};
		AccessMiddleware access;
		HeaderChain<AuthMiddleware, AccessMiddleware> chain{auth, access};
		Route health_route{"/health", HttpMethod::Get, health};
		Route echo_route{"/echo", HttpMethod::Post, echo};
		Route parameters_route{"/parameters", Route::any_method, parameters};
		Route private_route{"/private", HttpMethod::Get, parameters, {}, AccessPolicy::require(1), {}, chain};
		Route forbidden_route{"/forbidden", HttpMethod::Get, parameters, {}, AccessPolicy::require(2), {}, chain};
		Route twice_route{"/twice", HttpMethod::Get, twice};
		HandlerBase *const routes[6]{&health_route, &echo_route, &parameters_route,
			                         &private_route, &forbidden_route, &twice_route};
		HttpLimits limits;
		HttpServer server{backend, routes, limits};
		std::array<char, 8192> head{};
		std::array<http::wire::HeaderEntry, 32> entries{};
		std::array<std::uint8_t, 4096> body{};
		bool prepared_body;
		Observation observation;

		explicit Application(bool prepared)
		  : prepared_body(prepared)
		{
			observation.workspace = head.data();
			observation.workspace_size = head.size();
			observation.prepared_body = prepared ? body.data() : nullptr;
			CHECK(server.begin() == HttpError::Ok);
		}

		~Application()
		{
			CHECK(server.end() == HttpError::Ok);
		}

		static std::uint64_t milliseconds(void *context)
		{
			return static_cast<ScriptedTransport *>(context)->milliseconds();
		}

		void execute(ScriptedTransport &transport, Handle connection, HttpRequestHandler *handler = nullptr,
		    std::size_t body_capacity = 4096)
		{
			observed = &observation;
			std::atomic<bool> stopped{false};
			const Clock clock{&transport, milliseconds};
			SessionIo io(transport, connection, stopped, clock, transport.milliseconds() + 5000, 1000, 20);
			executeHttpConnection(io, {head.data(), head.size(), entries.data(), entries.size(),
			                           prepared_body ? body.data() : nullptr, prepared_body ? body_capacity : 0},
			    handler ? *handler : server, limits);
			++exchanges;
		}

	};

	struct Connection
	{
		ScriptedTransport transport;
		Handle listener;
		Handle handle;

		Connection()
		{
			Step open;
			open.operation = Operation::OpenListener;
			open.handle = {42, true};
			open.port = 31234;
			CHECK(transport.append(open));
			listener = transport.openListener("127.0.0.1", 0, 7).handle;
			Step accepted;
			accepted.operation = Operation::Accept;
			accepted.expected_handle = listener;
			accepted.handle = {0, true};
			CHECK(transport.append(accepted));
			handle = transport.accept(listener).handle;
			CHECK(listener.valid && handle.valid && handle.value == 0);
		}

		void input(std::string_view bytes, std::size_t chunk)
		{
			if(bytes.empty())
				return;

			Step step;
			step.operation = Operation::Read;
			step.expected_handle = handle;
			step.bytes = reinterpret_cast<const std::uint8_t *>(bytes.data());
			step.size = bytes.size();
			step.maximum_chunk = chunk;
			CHECK(transport.append(step));
		}

		void output(std::size_t size, std::size_t chunk)
		{
			Step step;
			step.operation = Operation::Write;
			step.expected_handle = handle;
			step.size = size;
			step.maximum_chunk = chunk;
			CHECK(transport.append(step));
		}

		void failure(Operation operation, IoStatus status)
		{
			Step step;
			step.operation = operation;
			step.expected_handle = handle;
			step.io_status = status;
			step.native_error = status == IoStatus::Fatal ? 123 : 0;
			CHECK(transport.append(step));
		}

		void wait(bool writing, WaitStatus status = WaitStatus::Ready)
		{
			Step step;
			step.operation = Operation::Wait;
			step.expected_handle = handle;
			step.wait_status = status;
			step.elapsed_ms = 1;
			if(status == WaitStatus::Ready)
				step.readiness[0] = {!writing, writing, false};

			CHECK(transport.append(step));
		}

		std::string captured() const
		{
			return {reinterpret_cast<const char *>(transport.capturedBytes()), transport.capturedSize()};
		}

		void close()
		{
			CHECK(transport.remaining() == 0 && transport.failures() == 0);
			Step step;
			step.operation = Operation::Close;
			step.expected_handle = handle;
			CHECK(transport.append(step));
			CHECK(transport.close(handle).released && !handle.valid);
			step.expected_handle = listener;
			CHECK(transport.append(step));
			CHECK(transport.close(listener).released && !listener.valid);
			CHECK(transport.owned() == 0 && transport.released() == 2 && transport.remaining() == 0);
		}

	};

	std::string expectedResponse(int status, std::string_view type, std::string_view body,
	    std::string_view extra_headers = {})
	{
		return "HTTP/1.1 " + std::to_string(status) + " Response\r\nConnection: close\r\nContent-Length: " +
		       std::to_string(body.size()) + "\r\nContent-Type: " + std::string(type) + "\r\n" +
		       std::string(extra_headers) + "\r\n" + std::string(body);
	}

	void complete(Application &application, std::string_view request, const std::string &expected,
	    std::size_t chunk = 127)
	{
		Connection connection;
		connection.input(request, chunk);
		connection.output(expected.size(), chunk);
		application.execute(connection.transport, connection.handle);
		CHECK(connection.captured() == expected);
		connection.close();
	}

	void partialAndBinary(Application &application)
	{
		std::string payload(4096, '\0');
		for(std::size_t index = 0; index < payload.size(); ++index)
			payload[index] = static_cast<char>(index * 37);

		const std::string head = "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 4096\r\n\r\n";
		const std::string request = head + payload;
		const auto expected = expectedResponse(200, "application/octet-stream", payload);
		for(const auto chunk : {1u, 2u, 3u, 7u, 127u})
		{
			complete(application, request, expected, chunk);
			CHECK(application.observation.body_storage_valid && application.observation.copied_size == payload.size());
			CHECK(std::memcmp(application.observation.copied_body.data(), payload.data(), payload.size()) == 0);
			application.head.fill(static_cast<char>(0xcc));
			CHECK(std::memcmp(application.observation.copied_body.data(), payload.data(), payload.size()) == 0);
		}

		const std::string small_head = "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\n";
		const std::string small = small_head + std::string("a\0b\xff", 4);
		const auto small_expected = expectedResponse(200, "application/octet-stream", std::string("a\0b\xff", 4));
		for(std::size_t split = 0; split <= small.size(); ++split)
		{
			Connection connection;
			connection.input(std::string_view(small).substr(0, split), 127);
			connection.input(std::string_view(small).substr(split), 127);
			connection.output(small_expected.size(), 3);
			application.execute(connection.transport, connection.handle);
			CHECK(connection.captured() == small_expected && application.observation.body_storage_valid);
			connection.close();
		}
	}

	void interruptedIo(Application &application)
	{
		const std::string request = "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\na\0bc"s;
		const auto expected = expectedResponse(200, "application/octet-stream", std::string("a\0bc", 4));
		Connection connection;
		connection.input(std::string_view(request).substr(0, 10), 2);
		connection.failure(Operation::Read, IoStatus::WouldBlock);
		connection.wait(false);
		connection.failure(Operation::Read, IoStatus::Interrupted);
		connection.wait(false, WaitStatus::Interrupted);
		connection.input(std::string_view(request).substr(10, request.size() - 12), 3);
		connection.failure(Operation::Read, IoStatus::WouldBlock);
		connection.wait(false);
		connection.input(std::string_view(request).substr(request.size() - 2), 1);
		connection.output(3, 1);
		connection.failure(Operation::Write, IoStatus::Interrupted);
		connection.wait(true, WaitStatus::Interrupted);
		connection.failure(Operation::Write, IoStatus::WouldBlock);
		connection.wait(true);
		connection.output(expected.size() - 3, 7);
		application.execute(connection.transport, connection.handle);
		CHECK(connection.captured() == expected && connection.transport.milliseconds() == 5);
		connection.close();
	}

	void semanticCorpus(Application &application)
	{
		const auto ready = expectedResponse(200, "text/plain; charset=utf-8", "ready");
		complete(application, "GET /health HTTP/1.1\r\nHost: test\r\n\r\n", ready);
		const auto calls = application.observation.health_calls;
		complete(application, "GET /health HTTP/1.1\r\nHost: test\r\n\r\nGET /missing HTTP/1.1\r\nHost: test\r\n\r\n",
		    ready);
		CHECK(application.observation.health_calls == calls + 1);
		complete(application, "GET /missing HTTP/1.1\r\nHost: test\r\n\r\n",
		    expectedResponse(404, "text/plain; charset=utf-8", "Route not found"));
		complete(application, "GET /echo HTTP/1.1\r\nHost: test\r\n\r\n",
		    expectedResponse(405, "text/plain; charset=utf-8", "Method not allowed", "Allow: POST\r\n"));
		complete(application, "GET /parameters?value=query%00tail HTTP/1.1\r\nHost: test\r\n\r\n",
		    expectedResponse(200, "text/plain; charset=utf-8", std::string("query\0tail", 10)));
		const std::string form = "value=form%00tail";
		complete(application, "POST /parameters?value=query HTTP/1.1\r\nHost: test\r\n"
		    "Content-Type: Application/X-WWW-Form-Urlencoded; charset=UTF-8\r\nContent-Length: " +
		    std::to_string(form.size()) + "\r\n\r\n" + form,
		    expectedResponse(200, "text/plain; charset=utf-8", std::string("form\0tail", 9)));
		const auto parameter_calls = application.observation.parameter_calls;
		complete(application, "GET /parameters?value=%GG HTTP/1.1\r\nHost: test\r\n\r\n",
		    expectedResponse(400, "text/plain; charset=utf-8", "Invalid query/form encoding"));
		complete(application, "GET /private HTTP/1.1\r\nHost: test\r\n\r\n",
		    expectedResponse(401, "text/plain", "Unauthorized", "WWW-Authenticate: Basic realm=\"nawserver\"\r\n"));
		CHECK(application.observation.parameter_calls == parameter_calls);
		complete(application, "GET /private?value=authorized HTTP/1.1\r\nHost: test\r\n"
		    "Authorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\n",
		    expectedResponse(200, "text/plain; charset=utf-8", "authorized"));
		complete(application, "GET /forbidden HTTP/1.1\r\nHost: test\r\n"
		    "Authorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\n",
		    expectedResponse(403, "text/plain", "Forbidden"));
		complete(application, "GET /twice HTTP/1.1\r\nHost: test\r\n\r\n",
		    expectedResponse(200, "text/plain; charset=utf-8", "first"));
		CHECK(application.observation.first_send == SendResult::Ok &&
		    application.observation.second_send == SendResult::AlreadySent);
	}

	void eofAndSendFailure(Application &application)
	{
		{
			Connection connection;
			const std::string incomplete = "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\na\0"s;
			const auto expected = expectedResponse(400, "text/plain; charset=utf-8", "Incomplete request body");
			const auto calls = application.observation.echo_calls;
			connection.input(incomplete, 127);
			connection.failure(Operation::Read, IoStatus::PeerClosed);
			connection.output(expected.size(), 2);
			application.execute(connection.transport, connection.handle);
			CHECK(connection.captured() == expected && application.observation.echo_calls == calls);
			connection.close();
		}
		{
			Connection connection;
			connection.input("GET /health HTTP/1.1\r\nHost:", 3);
			connection.failure(Operation::Read, IoStatus::PeerClosed);
			const auto expected = expectedResponse(400, "text/plain; charset=utf-8", "HTTP request rejected");
			connection.output(expected.size(), 7);
			application.execute(connection.transport, connection.handle);
			CHECK(connection.captured() == expected);
			connection.close();
		}
		{
			Connection connection;
			connection.input("GET /twice HTTP/1.1\r\nHost: test\r\n\r\n", 7);
			connection.output(3, 3);
			connection.failure(Operation::Write, IoStatus::Fatal);
			const auto calls = application.observation.twice_calls;
			application.execute(connection.transport, connection.handle);
			CHECK(connection.captured() == "HTT" && application.observation.twice_calls == calls + 1);
			CHECK(application.observation.first_send == SendResult::TransportError &&
			    application.observation.second_send == SendResult::AlreadySent);
			CHECK(connection.transport.calls(Operation::Write) == 2);
			connection.close();
		}
	}

	void bodyBoundaryAndTimeout(Application &application)
	{
		complete(application, "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n",
		    expectedResponse(200, "application/octet-stream", ""));
		CHECK(application.observation.body_storage_valid);
		const auto calls = application.observation.echo_calls;
		const std::string oversized(4097, 'x');
		complete(application, "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 4097\r\n\r\n" + oversized,
		    expectedResponse(413, "text/plain; charset=utf-8", "Request body exceeds route limit"), 8192);
		CHECK(application.observation.echo_calls == calls);
		Connection connection;
		connection.input("POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\na", 127);
		Step timeout;
		timeout.operation = Operation::Wait;
		timeout.expected_handle = connection.handle;
		timeout.wait_status = WaitStatus::Timeout;
		timeout.elapsed_ms = 20;
		for(unsigned index = 0; index < 50; ++index)
		{
			connection.failure(Operation::Read, IoStatus::WouldBlock);
			CHECK(connection.transport.append(timeout));
		}
		const auto expected = expectedResponse(408, "text/plain; charset=utf-8", "Incomplete request body");
		connection.output(expected.size(), 3);
		application.execute(connection.transport, connection.handle);
		CHECK(connection.captured() == expected && application.observation.echo_calls == calls);
		connection.close();
	}

	class PreparedReader final : public HttpRequestHandler
	{
		Application &application_;
	public:
		explicit PreparedReader(Application &application)
		  : application_(application)
		{
		}

		HeaderDisposition prepareHead(HttpExchange &exchange, HttpRequestState &state) override
		{
			return application_.server.prepareHead(exchange, state);
		}

		ExchangeDisposition handle(HttpExchange &exchange) override
		{
			const auto prepared = exchange.preparedBody();
			CHECK(prepared && prepared->data() == application_.body.data() && prepared->size() == 4);
			std::uint8_t bytes[2];
			CHECK(exchange.readBody(bytes, 2).size == 2 && bytes[0] == 'a' && bytes[1] == 0);
			CHECK(exchange.readBody(bytes, 2).size == 2 && bytes[0] == 'b' && bytes[1] == 0xff);
			CHECK(exchange.readBody(bytes, 2).status == BodyReadStatus::End);
			CHECK(exchange.preparedBody()->data() == prepared->data());
			return application_.server.handle(exchange);
		}

	};

	void preparedWorkspace(Application &application)
	{
		if(!application.prepared_body) return;

		const std::string request = "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\na\0b\xff"s;
		Connection connection;
		connection.input(request, 127);
		const auto expected = expectedResponse(200, "application/octet-stream", std::string("a\0b\xff", 4));
		connection.output(expected.size(), 1);
		PreparedReader reader(application);
		application.execute(connection.transport, connection.handle, &reader);
		CHECK(connection.captured() == expected);
		connection.close();

		Connection insufficient;
		insufficient.input(request, 127);
		const auto rejected = expectedResponse(400, "text/plain; charset=utf-8", "Incomplete request body");
		insufficient.output(rejected.size(), 3);
		const auto calls = application.observation.echo_calls;
		application.execute(insufficient.transport, insufficient.handle, nullptr, 3);
		CHECK(insufficient.captured() == rejected && application.observation.echo_calls == calls);
		insufficient.close();

		Connection invalid;
		std::atomic<bool> stopped{false};
		const Clock clock{&invalid.transport, Application::milliseconds};
		SessionIo io(invalid.transport, invalid.handle, stopped, clock, 5000, 1000, 20);
		CHECK(executeHttpConnection(io, {application.head.data(), application.head.size(), application.entries.data(),
		                                 application.entries.size(), nullptr, 1}, application.server,
		    application.limits) == ConnectionDisposition::Close);
		CHECK(invalid.transport.calls(Operation::Read) == 0 && invalid.transport.capturedSize() == 0);
		invalid.close();
	}

} // namespace

int main()
{
	for(const bool prepared : {false, true})
	{
		Application application(prepared);
		partialAndBinary(application);
		interruptedIo(application);
		semanticCorpus(application);
		eofAndSendFailure(application);
		bodyBoundaryAndTimeout(application);
		preparedWorkspace(application);
	}
	std::printf("TCP_HTTP_SCRIPTED checks=%u exchanges=%u PASS\n", checks, exchanges);
}
