// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/client_identity.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

using namespace NaWServer;

namespace
{
	template <class T, class = void> struct HasAuthIdentity : std::false_type {};
	template <class T> struct HasAuthIdentity<T, std::void_t<decltype(std::declval<T>().principal)> >
		: std::true_type {};
	template <class T, class = void> struct HasAuthProvider : std::false_type {};
	template <class T> struct HasAuthProvider<T, std::void_t<decltype(std::declval<T>().authenticated_provider)> >
		: std::true_type {};
	template <class T, class = void> struct HasAuthChallenge : std::false_type {};
	template <class T> struct HasAuthChallenge<T, std::void_t<decltype(std::declval<T>().authentication_challenge)> >
		: std::true_type {};
	static_assert(HasAuthIdentity<HttpRequestState>::value == static_cast<bool>(NAWSERVER_AUTH));
	static_assert(HasAuthProvider<HttpRequestState>::value == static_cast<bool>(NAWSERVER_AUTH));
	static_assert(HasAuthIdentity<HeaderContext>::value == static_cast<bool>(NAWSERVER_AUTH));
	static_assert(HasAuthProvider<HeaderContext>::value == static_cast<bool>(NAWSERVER_AUTH));
	static_assert(HasAuthChallenge<HeaderContext>::value == static_cast<bool>(NAWSERVER_AUTH));
	static_assert(HasAuthIdentity<Context>::value == static_cast<bool>(NAWSERVER_AUTH));
	static_assert(HasAuthProvider<Context>::value == static_cast<bool>(NAWSERVER_AUTH));
	static_assert(HasAuthChallenge<Context>::value == static_cast<bool>(NAWSERVER_AUTH));

	void check(bool value, int line)
	{
		if(!value) { std::printf("CLIENT_IDENTITY_FAIL line=%d\n", line); std::abort(); }
	}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	class Exchange : public HttpExchange
	{
	public:
		std::string target = "/api";
		std::optional<std::string> page;
		std::optional<std::uint32_t> peer = 0xc0a80101;
		HttpRequestHead head() const override
		{
			return {HttpMethod::Get, target, 0};
		}

		HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
		{
			if(std::string_view(name) != "X-Page-Id" || !page) return {HeaderReadStatus::NotFound, 0};

			if(!destination || capacity <= page->size()) return {HeaderReadStatus::BufferTooSmall, page->size()};

			std::memcpy(destination, page->data(), page->size());
			destination[page->size()] = '\0';
			return {HeaderReadStatus::Ok, page->size()};
		}

		BodyReadResult readBody(std::uint8_t *, std::size_t) override
		{
			std::abort();
		}

		ResponseTransport responseTransport() override
		{
			return {};
		}

		std::optional<std::uint32_t> peerAddress() const override
		{
			return peer;
		}

	};

}

int main()
{
	Exchange exchange;
	HeaderRequest request(exchange);
	Response response({});
	const AccessPolicy access{};
	HeaderContext context{request, response, access};
	ClientIdentity pages(ClientIdentitySource::Page);
	char key[64];
	CHECK(pages.resolve(context, key, sizeof(key)).status == HeaderReadStatus::Ok);
	CHECK(std::string_view(key) == "ic0a80101");
	exchange.page = "tab_1";
	CHECK(pages.resolve(context, key, sizeof(key)).status == HeaderReadStatus::Ok);
	CHECK(std::string_view(key) == "ic0a80101:ptab_1");
	exchange.target = "/ws?page_id=tab_1";
	CHECK(pages.resolve(context, key, sizeof(key)).status == HeaderReadStatus::Ok);
	exchange.page.reset();
	CHECK(pages.resolve(context, key, sizeof(key)).status == HeaderReadStatus::Ok);
	exchange.target = "/ws?page_id=tab_1&page_id=tab_2";
	CHECK(pages.resolve(context, key, sizeof(key)).status == HeaderReadStatus::InvalidArgument);
	exchange.target = "/ws?page_id=%31";
	CHECK(pages.resolve(context, key, sizeof(key)).status == HeaderReadStatus::InvalidArgument);
	exchange.target = "/ws?page_id=tab_2";
	exchange.page = "tab_1";
	CHECK(pages.resolve(context, key, sizeof(key)).status == HeaderReadStatus::InvalidArgument);
	exchange.page.reset();
	CHECK(pages.resolve(context, key, 3).status == HeaderReadStatus::BufferTooSmall);
	exchange.peer.reset();
	CHECK(pages.resolve(context, key, sizeof(key)).status == HeaderReadStatus::InvalidArgument);
	ClientIdentity users(ClientIdentitySource::AuthenticatedUser);
	CHECK(users.resolve(context, key, sizeof(key)).status == HeaderReadStatus::InvalidArgument);
#if NAWSERVER_AUTH
	context.principal = {true, 42, 0};
	CHECK(users.resolve(context, key, sizeof(key)).status == HeaderReadStatus::Ok);
	CHECK(std::string_view(key) == ":u2a");
#else
	exchange.peer = 0xc0a80101;
	ClientIdentity peer_user(ClientIdentitySource::PeerAndUser);
	CHECK(peer_user.resolve(context, key, sizeof(key)).status == HeaderReadStatus::InvalidArgument);
	const auto protected_access = AccessPolicy::authenticated();
	HeaderContext protected_context{request, response, protected_access};
	CHECK(checkAccess(protected_context) == Result::Unauthorized);
	CHECK(checkAccess(context) == Result::Complete);
#endif
	std::puts("CLIENT_IDENTITY_PASS");
}
