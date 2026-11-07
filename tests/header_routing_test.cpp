// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <NaWServer/request.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/admission_http.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace NaWServer;
namespace
{
	unsigned checks = 0;
	void check(bool passed, int line)
	{
		++checks;
		if(!passed) { std::fprintf(stderr, "HEADER_ROUTING_FAIL line=%d\n", line); std::abort(); }
	}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	class Exchange final : public HttpExchange
	{
		static SendResult send(void *context, int status, const char *, const Header *, std::size_t, ByteView)
		{
			auto &self = *static_cast<Exchange *>(context); ++self.sends; self.status = status; return SendResult::Ok;
		}

	public:
		HttpRequestHead request{HttpMethod::Post, "/small", 4};
		HttpRequestState state;
		unsigned reads = 0, sends = 0;
		int status = 0;
		const char *key = "guest";
		HttpRequestHead head() const override
		{
			return request;
		}

		HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
		{
			if(std::strcmp(name, "X-Client-Key") != 0 || !key) return {HeaderReadStatus::NotFound, 0};

			const auto size = std::strlen(key);
			if(capacity <= size) return {HeaderReadStatus::BufferTooSmall, size};

			std::memcpy(destination, key, size + 1); return {HeaderReadStatus::Ok, size};
		}

		BodyReadResult readBody(uint8_t *out, std::size_t capacity) override
		{
			++reads; const auto count = capacity < 4 ? capacity : 4; std::memset(out, 1, count);
			return {BodyReadStatus::Data, count};
		}

		ResponseTransport responseTransport() override
		{
			return {this, send};
		}

		HttpRequestState *requestState() const override
		{
			return const_cast<HttpRequestState *>(&state);
		}

	};
	class Backend final : public HttpBackend
	{
	public:
		HttpError start(HttpRequestHandler &, const HttpLimits &) override
		{
			return HttpError::Ok;
		}

		HttpError stop() override
		{
			return HttpError::Ok;
		}

	};
	struct Provider
	{
		unsigned calls = 0;
		bool allowed = true;
		bool authenticate(HeaderRequest &, Principal &principal)
		{
			++calls;
			if(allowed) principal = {true, 7, 1};

			return allowed;
		}

		const char *challenge() const
		{
			return "test";
		}

	};
	struct Dropper { Result operator()(HeaderContext &, HeaderNext &) const
					 {
						 return Result::Drop;
					 }

	};
	struct SwallowDrop
	{
		Result operator()(HeaderContext &, HeaderNext &next) const
		{
			(void)next(); return Result::Complete;
		}

	};
	std::uint64_t now()
	{
		return 10;
	}

}
int main()
{
	Backend backend;
	unsigned calls = 0;
	auto endpoint = [&](Request &request, Response &response)
		{
			++calls; CHECK(request.body().size() == 4); return response.empty(204);
		};
	Provider provider;
	AuthMiddleware auth(provider);
	auto early = makeHeaderChain(auth);
	Route route("/small", HttpMethod::Post, EndpointRef(endpoint), {}, AccessPolicy::require(1), {}, early);
	HandlerBase *const routes[]{&route};
	HttpServer server(backend, routes);
	CHECK(server.begin() == HttpError::Ok);
	{
		Exchange exchange;
		provider.allowed = false;
		CHECK(server.prepareHead(exchange, exchange.state) == HeaderDisposition::Reply);
		CHECK(exchange.status == 401 && exchange.reads == 0 && calls == 0);
		CHECK(server.handle(exchange) == ExchangeDisposition::CloseConnection);
		CHECK(exchange.sends == 1 && provider.calls == 1);
	}
	provider.allowed = true;
	{
		Exchange exchange;
		CHECK(server.prepareHead(exchange, exchange.state) == HeaderDisposition::Continue);
		CHECK(exchange.reads == 0 && exchange.sends == 0);
		CHECK(server.handle(exchange) == ExchangeDisposition::Complete);
		CHECK(provider.calls == 2 && exchange.reads == 1 && calls == 1);
	}
	CHECK(server.end() == HttpError::Ok);
	// Global authentication is permitted; access is enforced at the end of the
	// entire early chain, rather than before global middleware has run.
	Route global_route("/small", HttpMethod::Post, EndpointRef(endpoint), {}, AccessPolicy::require(1));
	HandlerBase *const global_routes[]{&global_route};
	HttpServer global_server(backend, global_routes, {}, {}, {}, early);
	{
		Exchange exchange; CHECK(global_server.handle(exchange) == ExchangeDisposition::Complete);
		CHECK(exchange.status == 204 && provider.calls == 3);
	}
	Dropper drop;
	SwallowDrop swallow;
	auto drop_chain = makeHeaderChain(swallow, drop);
	Route dropped("/small", HttpMethod::Post, EndpointRef(endpoint), {}, {}, {}, drop_chain);
	HandlerBase *const drop_routes[]{&dropped};
	HttpServer drop_server(backend, drop_routes);
	{
		Exchange exchange; const auto before = calls;
		CHECK(drop_server.handle(exchange) == ExchangeDisposition::Drop);
		CHECK(exchange.reads == 0 && exchange.sends == 0 && calls == before);
	}
	AdmissionOptions options;
	options.shared.http = 1;
	Admission<1> policy(options);
	AdmissionMiddleware quota(AdmissionRef(policy), now);
	auto quota_chain = makeHeaderChain(quota);
	Route public_route("/small", HttpMethod::Post, EndpointRef(endpoint));
	HandlerBase *const quota_routes[]{&public_route};
	HttpServer quota_server(backend, quota_routes, {}, {}, {}, quota_chain);
	{
		Exchange first, second;
		CHECK(quota_server.prepareHead(first, first.state) == HeaderDisposition::Continue);
		CHECK(quota_server.prepareHead(second, second.state) == HeaderDisposition::Reply);
		CHECK(second.status == 503 && second.reads == 0);
		first.state.releaseResources();
		Exchange third; CHECK(quota_server.prepareHead(third, third.state) == HeaderDisposition::Continue);
		CHECK(third.reads == 0);
	}
	CHECK(policy.stats().active[0] == 0);
	// A queued early rejection still owns its admitted permit until the backend
	// closes the request; middleware must not publish that capacity prematurely.
	{
		Admission<1> rejection_policy(options);
		AdmissionMiddleware rejection_quota(AdmissionRef(rejection_policy), now);
		unsigned rejections = 0;
		auto reject = [&](HeaderContext &context, HeaderNext &)
			{
				++rejections;
				return responseResult(context.response.empty(403));
			};
		auto route_chain = makeHeaderChain(reject);
		auto global_chain = makeHeaderChain(auth, rejection_quota);
		Route rejecting_route("/small", HttpMethod::Post, EndpointRef(endpoint), {},
		    AccessPolicy::authenticated(), {}, route_chain);
		HandlerBase *const rejecting_routes[]{&rejecting_route};
		HttpServer rejecting(backend, rejecting_routes, {}, {}, {}, global_chain);
		const auto before_auth = provider.calls;
		provider.allowed = false;
		{
			Exchange denied;
			CHECK(rejecting.prepareHead(denied, denied.state) == HeaderDisposition::Reply);
			CHECK(denied.status == 401 && rejections == 0 && rejection_policy.stats().active[0] == 0);
		}
		provider.allowed = true;
		Exchange exchange;
		CHECK(rejecting.prepareHead(exchange, exchange.state) == HeaderDisposition::Reply);
		CHECK(exchange.status == 403 && rejection_policy.stats().active[0] == 1);
		CHECK(rejections == 1 && provider.calls == before_auth + 2);
		exchange.state.releaseResources();
		CHECK(rejection_policy.stats().active[0] == 0);
	}
	{
		auto oversized_reply = [](HeaderContext &context, HeaderNext &)
			{
				const char body[NAWSERVER_HEADER_REPLY_MAX_BYTES + 1]{};
				return responseResult(context.response.text(503, {body, sizeof(body)}));
			};
		auto chain = makeHeaderChain(oversized_reply);
		HttpServer limited(backend, quota_routes, {}, {}, {}, chain);
		Exchange exchange;
		CHECK(limited.handle(exchange) == ExchangeDisposition::Drop);
		CHECK(exchange.sends == 0 && exchange.reads == 0);
	}
	class Prefix final : public HandlerBase
	{
	public:
		RouteDescriptors routes() const override
		{
			static const RouteDescriptor descriptors[]{{"/", HttpMethod::Any, RouteMatch::Prefix}};
			return {descriptors, 1};
		}

		void handleRequest(Request *, Response &response) override
		{
			(void)response.empty(202);
		}

	} prefix;
	HandlerBase *const mixed[]{&prefix, &public_route};
	HttpServer mixed_server(backend, mixed);
	{
		Exchange exchange; exchange.request.target = "/small?query=1";
		CHECK(mixed_server.handle(exchange) == ExchangeDisposition::Complete && exchange.status == 204);
	}
	{
		Exchange exchange; exchange.request.method = HttpMethod::Get;
		CHECK(mixed_server.handle(exchange) == ExchangeDisposition::CloseConnection && exchange.status == 405);
		CHECK(exchange.reads == 0);
	}
	std::printf("HEADER_ROUTING_PASS checks=%u\n", checks);
}
