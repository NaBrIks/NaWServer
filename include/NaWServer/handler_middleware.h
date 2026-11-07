// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <NaWServer/pipeline.h>
#include <NaWServer/header_request.h>
#ifndef NAWSERVER_AUTHORIZATION_MAX_BYTES
#define NAWSERVER_AUTHORIZATION_MAX_BYTES 512
#endif
namespace NaWServer
{
#if NAWSERVER_AUTH
	class AuthenticatorRef
	{
		void *object_;
		bool (*authenticate_)(void *, HeaderRequest &, Principal &);
		const char *(*challenge_)(void *);
	public:
		template <class Provider> explicit AuthenticatorRef(Provider &provider)
		  : object_(detail::borrowAddress(provider)),
		    authenticate_([](void *object, HeaderRequest &request, Principal &principal)
		{
			return static_cast<Provider *>(object)->authenticate(request, principal);
		}),
		    challenge_([](void *object) {
			return static_cast<Provider *>(object)->challenge();
		})
		{
		}

		bool authenticate(HeaderRequest &request, Principal &principal) const
		{
			return authenticate_(object_, request, principal);
		}

		const char *challenge() const
		{
			return challenge_(object_);
		}

		const void *identity() const
		{
			return object_;
		}

	};
	class FixedBasicAuthenticator
	{
		const char *username_;
		const char *password_;
		uint32_t user_id_;
		Permissions permissions_;
	public:
		constexpr FixedBasicAuthenticator(const char *username, const char *password,
		    Permissions permissions = 0, uint32_t user_id = 1)
		  : username_(username), password_(password), user_id_(user_id), permissions_(permissions)
		{
		}

		bool authenticate(HeaderRequest &request, Principal &principal) const;
		constexpr const char *challenge() const
		{
			return "Basic realm=\"nawserver\"";
		}

	};
	class AuthMiddleware
	{
		AuthenticatorRef provider_;
	public:
		template <class Provider> explicit AuthMiddleware(Provider &provider)
		  : provider_(provider)
		{
		}

		template <class C> Result operator()(C &context, BasicNext<C> &next) const
		{
			if(!context.access.requires_authentication && !context.access.required_permissions) return next();

			if(context.principal.authenticated && context.authenticated_provider == provider_.identity()) return next();

			Principal principal{};
			if(!provider_.authenticate(context.request, principal) || !principal.authenticated)
			{
				context.principal = {};
				context.authenticated_provider = nullptr;
				context.authentication_challenge = provider_.challenge();
				return Result::Unauthorized;
			}

			context.principal = principal;
			context.authenticated_provider = provider_.identity();
			return next();
		}

	};
#endif
	class AccessMiddleware
	{
	public:
		template <class C> Result operator()(C &context, BasicNext<C> &next) const
		{
			const auto result = checkAccess(context);
			return result == Result::Complete ? next() : result;
		}

	};
}
