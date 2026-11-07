// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/handler_middleware.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include <type_traits>

#include <NaWServer/request.h>
using namespace NaWServer;

static unsigned allocations;
void *operator new(std::size_t size)
{
	++allocations;
	if(auto *ptr = std::malloc(size))
		return ptr;

	throw std::bad_alloc();
}

void operator delete(void *ptr) noexcept
{
	std::free(ptr);
}

void operator delete(void *ptr, std::size_t) noexcept
{
	std::free(ptr);
}

static unsigned checks;
#define CHECK(expression)                                                                                              \
		do                                                                                                                 \
		{                                                                                                                  \
			++checks;                                                                                                      \
			if(!(expression))                                                                                              \
			{                                                                                                              \
				std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression);                                         \
				std::exit(1);                                                                                              \
			}                                                                                                              \
		} while(0)

struct Trace
{
	char data[64]{};
	unsigned pos{};
	void add(char value)
	{
		data[pos++] = value;
	}

};
struct Around
{
	Trace &trace;
	char before, after;
	Result operator()(Context &, Next &next)
	{
		trace.add(before);
		auto result = next();
		trace.add(after);
		return result;
	}

};
struct Deny
{
	Result operator()(Context &, Next &)
	{
		return Result::Forbidden;
	}

};
struct Twice
{
	Result operator()(Context &, Next &next)
	{
		CHECK(next() == Result::Complete);
		next();
		return Result::Complete;
	}

};
struct Provider
{
	bool accepted{true};
	bool sets_identity{true};
	unsigned calls{};
	Permissions permissions{3};
	bool authenticate(HeaderRequest &, Principal &principal)
	{
		++calls;
		if(sets_identity)
			principal = {true, 7, permissions};

		return accepted;
	}

	const char *challenge() const
	{
		return "Fixture realm=\"tests\"";
	}

};
struct ConstProvider
{
	bool authenticate(HeaderRequest &, Principal &principal) const
	{
		principal = {true, 9, 3};
		return true;
	}

	const char *challenge() const
	{
		return "ConstFixture";
	}

};

int main()
{
	static_assert(!std::is_copy_constructible_v<Next>);
	static_assert(!std::is_constructible_v<Chain<Deny>, Deny &&>);
	static_assert(!std::is_constructible_v<MiddlewareRef, Deny &&>);
	static_assert(!std::is_convertible_v<Chain<Deny> &&, Pipeline>);
	static_assert(!std::is_copy_constructible_v<Chain<Deny> >);
	static_assert(std::is_same_v<decltype(makeChain(std::declval<Deny &>())), Chain<Deny> >);
	class Exchange final : public HttpExchange
	{
	public:
		HttpRequestHead head() const override
		{
			return {HttpMethod::Get, "/", 0};
		}

		HeaderReadResult readHeader(const char *, char *, std::size_t) const override
		{
			return {HeaderReadStatus::NotFound, 0};
		}

		BodyReadResult readBody(uint8_t *, std::size_t) override
		{
			return {BodyReadStatus::End, 0};
		}

		ResponseTransport responseTransport() override
		{
			return {};
		}

	} exchange;
	Request request(exchange);
	Response response({});
	auto public_access = AccessPolicy::public_access();
	auto private_access = AccessPolicy::authenticated();
	auto permission_access = AccessPolicy::require(3);
	Context context{request, response, public_access};
	Trace trace;
	Around first{trace, 'A', 'a'}, second{trace, 'B', 'b'};
	unsigned endpoint_calls = 0;
	auto endpoint = [&](Context &)
		{
			++endpoint_calls;
			trace.add('E');
			return Result::Complete;
		};

	// All three facades must agree on before/after order and borrow instances.
	Chain<Around, Around> typed(first, second);
	auto factory = makeChain(first, second);
	FixedChain<2> fixed(middlewareRef(first), middlewareRef(second));
	const unsigned before_allocations = allocations;
	for(Pipeline pipeline : {typed.view(), factory.view(), fixed.view()})
	{
		trace = {};
		CHECK(pipeline.run(context, EndpointRef(endpoint)) == Result::Complete);
		CHECK(trace.pos == 5);
		CHECK(
			trace.data[0] == 'A' && trace.data[1] == 'B' && trace.data[2] == 'E' && trace.data[3] == 'b' &&
			trace.data[4] == 'a');
	}
	CHECK(allocations == before_allocations);
	auto empty = makeChain();
	CHECK(empty.run(context, EndpointRef(endpoint)) == Result::Complete);
	FixedChain<0> empty_fixed;
	CHECK(empty_fixed.run(context, EndpointRef(endpoint)) == Result::Complete);
	Deny deny;
	auto stopped = makeChain(first, deny, second);
	const auto previous_calls = endpoint_calls;
	CHECK(stopped.run(context, EndpointRef(endpoint)) == Result::Forbidden);
	CHECK(endpoint_calls == previous_calls);
	Twice twice;
	auto invalid = makeChain(first, twice);
	CHECK(invalid.run(context, EndpointRef(endpoint)) == Result::InvalidNext);
	CHECK(endpoint_calls == previous_calls + 1);

	Provider provider;
	AuthMiddleware auth(provider);
	AccessMiddleware access;
	auto authenticated = makeChain(auth, access);
	Context restricted{request, response, permission_access};
	CHECK(authenticated.run(restricted, EndpointRef(endpoint)) == Result::Complete);
	CHECK(restricted.principal.user_id == 7);
	provider.permissions = 1;
	CHECK(authenticated.run(restricted, EndpointRef(endpoint)) == Result::Forbidden);
	provider.accepted = false;
	CHECK(authenticated.run(restricted, EndpointRef(endpoint)) == Result::Unauthorized);
	CHECK(!restricted.principal.authenticated);
	CHECK(
		restricted.authentication_challenge &&
		std::strcmp(restricted.authentication_challenge, "Fixture realm=\"tests\"") == 0);
	provider.accepted = true;
	provider.sets_identity = false;
	CHECK(authenticated.run(restricted, EndpointRef(endpoint)) == Result::Unauthorized);
	const auto provider_calls = provider.calls;
	context.authentication_challenge = "stale challenge";
	CHECK(authenticated.run(context, EndpointRef(endpoint)) == Result::Complete);
	CHECK(provider.calls == provider_calls); // Public route does not require auth.
	CHECK(!context.authentication_challenge);
	Context private_context{request, response, private_access};
	private_context.principal = {true, 99, 0xffffffffu};
	CHECK(empty.run(private_context, EndpointRef(endpoint)) == Result::Unauthorized);
	CHECK(!private_context.principal.authenticated); // No leaked previous identity.
	provider.sets_identity = true;
	provider.permissions = 1;
	auto only_auth = makeChain(auth);
	CHECK(only_auth.run(restricted, EndpointRef(endpoint)) == Result::Forbidden);
	auto wrong_order = makeChain(access, auth);
	CHECK(wrong_order.run(restricted, EndpointRef(endpoint)) == Result::Unauthorized);
	CHECK(Pipeline(nullptr, 1).run(context, EndpointRef(endpoint)) == Result::Error);
	// A const provider/middleware remains const in its invocation thunk.
	const AccessMiddleware const_access;
	auto const_chain = makeChain(const_access);
	CHECK(const_chain.run(context, EndpointRef(endpoint)) == Result::Complete);
	const ConstProvider const_provider;
	const AuthMiddleware const_auth(const_provider);
	auto const_authenticated = makeChain(const_auth, const_access);
	CHECK(const_authenticated.run(restricted, EndpointRef(endpoint)) == Result::Complete);
	CHECK(restricted.principal.user_id == 9);
	// All endpoint signatures use the same executor, policy and result conversion.
	unsigned sends = 0;
	ResponseTransport transport{
		&sends,
		[](void *state, int, const char *, const Header *, std::size_t, ByteView)
		{
			++*static_cast<unsigned *>(state);
			return SendResult::Ok;
		}};
	auto pair_handler = [](Request &, Response &reply)
		{
			return reply.text(200, "pair");
		};
	auto response_handler = [](Response &reply)
		{
			return reply.text(200, "response");
		};
	const auto allocations_before_replies = allocations;
	for(unsigned i = 0; i < 100; ++i)
	{
		Response reply(transport);
		Context sample{request, reply, public_access};
		CHECK(empty.run(sample, EndpointRef(pair_handler)) == Result::Complete);
	}
	{
		Response reply(transport);
		Context sample{request, reply, public_access};
		CHECK(empty.run(sample, EndpointRef(+response_handler)) == Result::Complete);
	}
	{
		Response reply(transport);
		Context sample{request, reply, private_access};
		CHECK(empty.run(sample, EndpointRef(response_handler)) == Result::Unauthorized);
		CHECK(!reply.started());
	}
	CHECK(sends == 101 && allocations == allocations_before_replies);
	// Terminal results are latched before returning through outer middleware.
	{
		Response reply(transport);
		Context sample{request, reply, public_access};
		auto swallow = [](Context &, Next &next)
			{
				(void)next();
				return Result::Complete;
			};
		auto drop_endpoint = [](Context &)
			{
				return Result::Drop;
			};
		auto chain = makeChain(swallow);
		CHECK(chain.run(sample, EndpointRef(drop_endpoint)) == Result::Drop);
		CHECK(reply.dropped() && sends == 101);
	}
	// Failed authentication by another provider must not retain the old identity.
	{
		Response reply(transport);
		Context sample{request, reply, private_access};
		sample.principal = {true, 99, 3};
		sample.authenticated_provider = &const_provider;
		provider.accepted = false;
		CHECK(only_auth.view().run(sample, EndpointRef(endpoint), true) == Result::Unauthorized);
		CHECK(!sample.principal.authenticated && !sample.authenticated_provider);
	}
	std::printf("PASS: %u pipeline checks; no allocations during execution\n", checks);
}
