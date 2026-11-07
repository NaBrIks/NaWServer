// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#include <NaWServer/http_server.h>
#ifndef NAWSERVER_TEST_TCP
#include <NaWServer/backends/windows.h>
#endif
#ifdef NAWSERVER_TEST_TCP
#include <NaWServer/backends/tcp.h>
#endif
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/handler_middleware.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace sockets = NaWServer::test::host;

using namespace NaWServer;
namespace
{
	unsigned checked = 0;
	void check(bool value, int line)
	{
		++checked;
		if(!value) { std::printf("WINDOWS_HTTP_FAIL line=%d\n", line); std::fflush(stdout); std::abort(); }
	}

#define CHECK(value) check((value), __LINE__)
	sockets::Handle connectTo(std::uint16_t port)
	{
		const auto client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(client != sockets::invalid);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		CHECK(connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
		CHECK(sockets::setTimeouts(client, 3000));
		return client;
	}

	std::string performRequest(std::uint16_t port, const std::string &request, bool finish = false)
	{
		const auto client = connectTo(port);
		std::size_t offset = 0;
		while(offset < request.size())
		{
			const auto size = sockets::sendSome(client, request.data() + offset,
			        static_cast<int>(request.size() - offset), 0);
			CHECK(size > 0);
			offset += static_cast<std::size_t>(size);
		}
		if(finish) shutdown(client, sockets::shutdown_send);

		std::string response;
		char buffer[2048];
		for(;;)
		{
			const auto size = sockets::receiveSome(client, buffer, sizeof(buffer), 0);
			CHECK(size >= 0);
			if(size == 0) break;

			response.append(buffer, static_cast<std::size_t>(size));
		}
		sockets::closeSocket(client);
		return response;
	}

	bool status(const std::string &response, int code)
	{
		return response.find("HTTP/1.1 " + std::to_string(code) + " ") == 0;
	}

	SendResult echo(Request &request, Response &response)
	{
		return response.send(200, "application/octet-stream", request.body());
	}

	SendResult parameters(Request &request, Response &response)
	{
		const auto value = request.parameter("value").value_or("");
		return response.send(200, "text/plain", {reinterpret_cast<const uint8_t *>(value.data()), value.size()});
	}

}

int main()
{
	CHECK(sockets::initialize() == 0);
#ifdef NAWSERVER_TEST_TCP
	sockets::TcpTransport transport;
	CHECK(transport.start().status == tcp::Status::Ok);
	TcpOptions tcp_options;
	tcp_options.port = 0;
	TcpHttpBackend backend(transport, tcp_options);
#else
	WindowsHttpBackend backend(0);
#endif
	HandlerStaticUriText index("/", "hello");
	Route echo_route("/echo", HttpMethod::Post, echo);
	Route parameters_route("/parameters", Route::any_method, parameters);
	Route head_route("/head", HttpMethod::Head, parameters);
	FixedBasicAuthenticator credentials("user", "password", 1);
	AuthMiddleware auth(credentials);
	auto chain = makeHeaderChain(auth);
	Route private_route("/private", HttpMethod::Get, parameters, {}, AccessPolicy::require(1), {}, chain);
	HandlerBase *const routes[]{&index, &echo_route, &parameters_route, &head_route, &private_route};
	HttpLimits limits;
	limits.exchange_timeout_ms = 400;
	HttpServer server(backend, routes, limits);
	CHECK(server.begin() == HttpError::Ok);
	const auto port = backend.port();
	CHECK(port != 0);
	auto response = performRequest(port, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
	CHECK(status(response, 200) && response.find("\r\n\r\nhello") != response.npos);
	CHECK(response.find("Connection: close\r\n") != response.npos);
	CHECK(status(performRequest(port, "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n"), 404));
	const std::string binary("A\0B\xff", 4);
	response = performRequest(port, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n" + binary);
	CHECK(status(response, 200) && response.substr(response.find("\r\n\r\n") + 4) == binary);
	response = performRequest(port, "GET /parameters?value=a%00b HTTP/1.1\r\nHost: localhost\r\n\r\n");
	CHECK(status(response, 200) && response.substr(response.find("\r\n\r\n") + 4) == std::string("a\0b", 3));
	response = performRequest(port,
	        "POST /parameters?value=query HTTP/1.1\r\nHost: localhost\r\n"
	        "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 10\r\n\r\nvalue=form");
	CHECK(status(response, 200) && response.substr(response.find("\r\n\r\n") + 4) == "form");
	CHECK(status(performRequest(port, "GET /private HTTP/1.1\r\nHost: localhost\r\n\r\n"), 401));
	CHECK(status(performRequest(port,
	    "GET /private HTTP/1.1\r\nHost: localhost\r\nAuthorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\n"), 200));
	response = performRequest(port, "HEAD /head?value=body HTTP/1.1\r\nHost: localhost\r\n\r\n");
	CHECK(status(response,
	    200) && response.find("Content-Length: 4\r\n") != response.npos &&
	    response.substr(response.find("\r\n\r\n") + 4).empty());
	for(const auto &bad : {"Content-Length: 1\r\nContent-Length: 1", "Content-Length: -1", "Content-Length: +1",
	                       "Content-Length: 1, 1", "Content-Length: 184467440737095516160", "Host: duplicate",
	                       " Bad: folded", "Bad Name: value",
	                       "Content-Length: 1\r\nTransfer-Encoding: chunked"})
		CHECK(status(performRequest(port, std::string("POST /echo HTTP/1.1\r\nHost: localhost\r\n") + bad + "\r\n\r\n"),
		    400));
	CHECK(status(performRequest(port, "GET / HTTP/1.1\r\n\r\n"), 400));
	CHECK(status(performRequest(port, "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"),
	    501));
	CHECK(status(performRequest(port,
	    "POST /echo HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n"), 417));
	CHECK(status(performRequest(port, "GET /parameters?value=%GG HTTP/1.1\r\nHost: localhost\r\n\r\n"), 400));
	CHECK(status(performRequest(port, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4097\r\n\r\n"), 413));
	CHECK(status(performRequest(port, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\na", true),
	    400));
	response = performRequest(port,
	        "GET / HTTP/1.1\r\nHost: localhost\r\nX-Large: " + std::string(8192, 'a') + "\r\n\r\n");
	CHECK(status(response, 431) && response.substr(response.find("\r\n\r\n") + 4) == "HTTP request rejected");
	CHECK(status(performRequest(port, "GET /" + std::string(1024, 'a') + " HTTP/1.1\r\nHost: localhost\r\n\r\n"), 414));
	CHECK(status(performRequest(port, "GET / HTTP/1.0\r\n\r\n"), 200));
	const auto client = connectTo(port);
	CHECK(sockets::sendSome(client, "GET / HTTP/1.1\r\n", 16, 0) == 16);
	const auto before = std::chrono::steady_clock::now();
	CHECK(server.end() == HttpError::Ok);
	CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
	sockets::closeSocket(client);
	CHECK(server.begin() == HttpError::Ok);
	CHECK(status(performRequest(backend.port(), "GET / HTTP/1.0\r\n\r\n"), 200));
	CHECK(server.end() == HttpError::Ok);
#ifdef NAWSERVER_TEST_TCP
	CHECK(transport.stop().status == tcp::Status::Ok);
#endif
	CHECK(sockets::cleanup() == 0);
	std::printf("WINDOWS_HTTP checks=%u PASS\n", checked);
}
