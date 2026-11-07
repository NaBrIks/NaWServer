// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/http_server.h>
#include <NaWServer/backends/memory.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/handler_middleware.h>
#include <cstdio>
#include <cstdlib>

using namespace NaWServer;
namespace
{
	unsigned checks = 0, endpoint_calls = 0;
	void check(bool condition, int line)
	{
		++checks;
		if(!condition) { std::fprintf(stderr, "HTTP_BACKEND_FAIL line=%d\n", line); std::abort(); }
	}

#define CHECK(value) check((value), __LINE__)
	std::string text(const MemoryResponse &response)
	{
		return {response.body.begin(), response.body.end()};
	}

	SendResult echo(Request &request, Response &response)
	{
		++endpoint_calls;
		return response.send(200, "application/octet-stream", request.body());
	}

	SendResult parameters(Request &request, Response &response)
	{
		++endpoint_calls;
		const auto value = request.parameter("value").value_or("");
		return response.send(200, "text/plain", {reinterpret_cast<const uint8_t *>(value.data()), value.size()});
	}

	SendResult empty(Response &)
	{
		return SendResult::Ok;
	}

	SendResult twice(Response &response)
	{
		(void)response.text(200, "first");
		return response.text(200, "second");
	}

}

int main()
{
	MemoryHttpBackend backend;
	FixedBasicAuthenticator credentials("user", "password", 1);
	AuthMiddleware auth(credentials);
	AccessMiddleware access;
	auto chain = makeHeaderChain(auth, access);
	Route echo_route("/echo", HttpMethod::Post, echo);
	Route parameters_route("/parameters", Route::any_method, parameters);
	Route private_route("/private", HttpMethod::Get, parameters, {}, AccessPolicy::require(1), {}, chain);
	Route forbidden_route("/forbidden", HttpMethod::Get, parameters, {}, AccessPolicy::require(2), {}, chain);
	Route empty_route("/empty", HttpMethod::Get, empty);
	Route twice_route("/twice", HttpMethod::Get, twice);
	HandlerStaticUriText index("/", "hello");
	HandlerBase *const routes[]{&index, &echo_route, &parameters_route, &private_route, &forbidden_route, &empty_route,
		                        &twice_route};
	HttpServer server(backend, routes);
	MemoryRequest request;
	CHECK(backend.perform(request).result == HttpError::InvalidState);
	CHECK(server.begin() == HttpError::Ok && server.running());
	CHECK(server.begin() == HttpError::Ok);
	auto response = backend.perform(request);
	CHECK(response.status == 200 && text(response) == "hello" && response.send_calls == 1);
	request.target = "/missing";
	CHECK(backend.perform(request).status == 404);
	request.target = "/echo";
	CHECK(backend.perform(request).status == 405);
	request.method = HttpMethod::Post;
	request.body = {0, 255, 'a', 0};
	request.content_length = request.body.size();
	request.read_chunk_size = 1;
	response = backend.perform(request);
	CHECK(response.status == 200 && response.body == request.body && response.read_calls == 4);
	request.content_length = 5;
	response = backend.perform(request);
	CHECK(response.status == 400 && response.disposition == ExchangeDisposition::CloseConnection);
	request.content_length = 4;
	request.fail_read_after = 2;
	response = backend.perform(request);
	CHECK(response.status == 408 && response.read_calls == 3);
	request.read_failure = BodyReadStatus::Disconnected;
	CHECK(backend.perform(request).status == 400);
	request.content_length = 4097;
	response = backend.perform(request);
	CHECK(response.status == 413 && response.read_calls == 0);
	request = {};
	request.target = "/parameters?value=query";
	CHECK(text(backend.perform(request)) == "query");
	request.method = HttpMethod::Post;
	request.headers.emplace_back("content-type", "application/x-www-form-urlencoded");
	const std::string form = "value=form%00tail";
	request.body.assign(form.begin(), form.end());
	request.content_length = form.size();
	CHECK(text(backend.perform(request)) == std::string("form\0tail", 9));
	const auto calls = endpoint_calls;
	request.body = {'%', 'G', 'G'};
	request.content_length = 3;
	CHECK(backend.perform(request).status == 400 && endpoint_calls == calls);
	request = {};
	request.target = "/private";
	CHECK(backend.perform(request).status == 401);
	request.headers.emplace_back("Authorization", "Basic dXNlcjpwYXNzd29yZA==");
	CHECK(backend.perform(request).status == 200);
	request.target = "/forbidden";
	CHECK(backend.perform(request).status == 403);
	request = {};
	request.target = "/empty";
	CHECK(backend.perform(request).status == 500);
	request.target = "/twice";
	response = backend.perform(request);
	CHECK(response.send_calls == 1 && text(response) == "first" &&
	    response.disposition == ExchangeDisposition::CloseConnection);
	request.target = "/";
	request.fail_send = true;
	response = backend.perform(request);
	CHECK(response.send_calls == 1 && response.status == 0 &&
	    response.disposition == ExchangeDisposition::CloseConnection);
	CHECK(server.end() == HttpError::Ok && !server.running());
	CHECK(backend.perform(request).result == HttpError::InvalidState);
	CHECK(server.end() == HttpError::Ok);
	CHECK(server.begin() == HttpError::Ok);
	CHECK(server.end() == HttpError::Ok);
	Route invalid("bad", HttpMethod::Get, empty);
	HandlerBase *const bad_routes[]{&invalid};
	HttpServer bad_server(backend, bad_routes);
	CHECK(bad_server.begin() == HttpError::InvalidArgument);
	std::printf("HTTP_BACKEND checks=%u PASS\n", checks);
}
