// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <NaWServer/admission.h>
#include <NaWServer/pipeline.h>
#include <NaWServer/header_request.h>
#include <charconv>
namespace NaWServer
{
#if NAWSERVER_CLIENT_QUOTAS
	// This adapter runs only in the header chain. The backend retains an opaque
	// lease; it never reads client credentials or implements quota policy.
	class AdmissionMiddleware
	{
		AdmissionRef policy_;
		const char *key_header_;
		std::uint64_t (*now_)();
	public:
		using IdentityResolver = HeaderReadResult (*)(void *, const HeaderContext &, char *, std::size_t);
	private:
		IdentityResolver identity_resolver_;
		void *identity_context_;
		bool resolve_all_keys_;
		static AdmissionTicket decode(const RequestResourceLease::Token &token)
		{
			return {static_cast<std::size_t>(token[0]), static_cast<std::uint32_t>(token[1]),
			        static_cast<AdmissionResource>(token[2]), static_cast<std::size_t>(token[3])};
		}

		static RequestResourceLease::Token encode(AdmissionTicket ticket)
		{
			return {ticket.client, ticket.generation, static_cast<std::uint64_t>(ticket.resource), ticket.permit};
		}

		static void release(void *context, const RequestResourceLease::Token &token)
		{
			const auto &self = *static_cast<const AdmissionMiddleware *>(context);
			(void)self.policy_.release(decode(token), self.now_());
		}

		static bool reclassify(void *context, RequestResourceLease::Token &token, RequestResource resource)
		{
			const auto &self = *static_cast<const AdmissionMiddleware *>(context);
			auto ticket = decode(token);
			if(!self.policy_.transfer(ticket, static_cast<AdmissionResource>(resource), self.now_())) return false;

			token = encode(ticket);
			return true;
		}

	public:
		AdmissionMiddleware(AdmissionRef policy, std::uint64_t(*now)(), const char *key_header = "X-Client-Key",
		    IdentityResolver resolver = nullptr, void *identity_context = nullptr, bool resolve_all_keys = false)
		  : policy_(policy), key_header_(key_header), now_(now), identity_resolver_(resolver),
		    identity_context_(identity_context), resolve_all_keys_(resolve_all_keys)
		{
		}

		Result operator()(HeaderContext &context, HeaderNext &next) const
		{
			if(!policy_ || !policy_.valid() || !now_ || !key_header_ ||
			    (resolve_all_keys_ && !identity_resolver_)) return Result::Error;

			auto *state = context.request.requestState();
			if(!state || state->resource_lease) return Result::Error;

			char key[NAWSERVER_ADMISSION_KEY_BYTES + 1];
			auto field = context.request.readHeader(key_header_, key, sizeof(key));
			if(field.status != HeaderReadStatus::Ok && field.status != HeaderReadStatus::NotFound)
				return responseResult(context.response.text(400, "Invalid client key"));

			std::string_view identity = field.status == HeaderReadStatus::Ok ? std::string_view(key,
			        field.size) : std::string_view{};
			if(identity_resolver_ && (resolve_all_keys_ || !policy_.protectedKey(identity)))
			{
				field = identity_resolver_(identity_context_, context, key, sizeof(key));
				if(field.status != HeaderReadStatus::Ok && field.status != HeaderReadStatus::NotFound)
					return responseResult(context.response.text(400, "Invalid client identity"));

				identity = field.status == HeaderReadStatus::Ok ? std::string_view(key,
				        field.size) : std::string_view{};
			}

			const auto admitted = policy_.acquire(identity, AdmissionResource::Http, now_());
			if(admitted.decision != AdmissionDecision::Accepted)
			{
				char retry[12];
				const auto number = std::to_chars(retry, retry + sizeof(retry),
				        std::max<std::uint32_t>(1,
				        admitted.retry_after_ms / 1000 + (admitted.retry_after_ms % 1000 != 0)));
				if(context.response.header("Retry-After",
				    {retry, static_cast<std::size_t>(number.ptr - retry)}) != SendResult::Ok)
					return Result::Error;

				return responseResult(context.response.text(admitted.decision == AdmissionDecision::Rate ? 429 : 503,
				           admitted.decision ==
				           AdmissionDecision::Rate ? "Request rate exceeded" : "No client capacity"));
			}

			state->resource_lease = RequestResourceLease(const_cast<AdmissionMiddleware *>(this),
			        encode(admitted.ticket),
			        release, reclassify, policy_.reserved(admitted.ticket));
			return next();
		}

	};
#endif
}
