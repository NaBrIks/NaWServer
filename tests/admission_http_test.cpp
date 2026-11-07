// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/admission_http.h>
#include <NaWServer/backends/memory.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
	using namespace NaWServer;
	std::uint64_t clock_ms = 0;
	std::uint64_t now()
	{
		return clock_ms;
	}

	void check(bool condition, int line)
	{
		if(!condition) { std::printf("ADMISSION_HTTP_FAIL line=%d\n", line); std::abort(); }
	}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	SendResult endpoint(Response &response)
	{
		return response.text(200, "ok");
	}

}

int main()
{
	AdmissionOptions configuration;
	configuration.reserved_key = "primary";
	configuration.shared.requests_per_window = 1;
	configuration.shared.window_ms = 100;
	Admission<1> policy(configuration);
	AdmissionMiddleware middleware(AdmissionRef(policy), now);
	auto chain = makeHeaderChain(middleware);
	Route route("/", HttpMethod::Get, endpoint, {}, {}, {}, chain);
	HandlerBase *const routes[]{&route};
	MemoryHttpBackend backend;
	HttpServer server(backend, routes);
	CHECK(server.begin() == HttpError::Ok);
	MemoryRequest request;
	request.headers = {{"X-Client-Key", "guest"}};
	CHECK(backend.perform(request).status == 200);
	const auto released = policy.acquire("guest", AdmissionResource::Http, clock_ms, false);
	CHECK(released.ticket && policy.release(released.ticket, clock_ms));
	CHECK(backend.perform(request).status == 429);
	request.headers = {{"X-Client-Key", "other"}};
	CHECK(backend.perform(request).status == 503);
	request.headers = {{"X-Client-Key", "primary"}};
	CHECK(backend.perform(request).status == 200);
	clock_ms = 100;
	request.headers = {{"X-Client-Key", "guest"}};
	CHECK(backend.perform(request).status == 200);
	CHECK(server.end() == HttpError::Ok);
	// Public slot labels must never bypass an authoritative credential resolver.
	AdmissionOptions secure_configuration;
	secure_configuration.reserved_key = "public-slot";
	Admission<1> secure_policy(secure_configuration);
	auto resolver = [](void *, const HeaderContext &context, char *output, std::size_t capacity)
		{
			char supplied[32];
			const auto read = context.request.readHeader("X-Client-Key", supplied, sizeof(supplied));
			if(read.status != HeaderReadStatus::Ok || std::string_view(supplied, read.size) != "actual-secret")
				return HeaderReadResult{HeaderReadStatus::InvalidArgument, 0};
			if(capacity < 12) return HeaderReadResult{HeaderReadStatus::BufferTooSmall, 11};
			std::memcpy(output, "public-slot", 12);
			return HeaderReadResult{HeaderReadStatus::Ok, 11};
		};
	AdmissionMiddleware secure_middleware(AdmissionRef(secure_policy), now, "X-Client-Key", resolver, nullptr, true);
	auto secure_chain = makeHeaderChain(secure_middleware);
	Route secure_route("/", HttpMethod::Get, endpoint, {}, {}, {}, secure_chain);
	HandlerBase *const secure_routes[]{&secure_route};
	HttpServer secure_server(backend, secure_routes);
	CHECK(secure_server.begin() == HttpError::Ok);
	request.headers = {{"X-Client-Key", "public-slot"}};
	CHECK(backend.perform(request).status == 400);
	request.headers = {{"X-Client-Key", "actual-secret"}};
	CHECK(backend.perform(request).status == 200);
	CHECK(secure_server.end() == HttpError::Ok);
	std::puts("ADMISSION_HTTP_PASS");
}
