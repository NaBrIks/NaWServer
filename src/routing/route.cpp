// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/route.h>
#include <NaWServer/request.h>
#include <cstring>
namespace NaWServer
{
	HttpError Route::validate() const
	{
		return path_ && path_[0] == '/' && !std::strchr(path_, '?') ? HttpError::Ok : HttpError::InvalidArgument;
	}

	void Route::handleRequest(Request *request, Response &response)
	{
		if(options_ && response.options(*options_) != SendResult::Ok)
			return;

		Context context{*request, response, access_};
#if NAWSERVER_AUTH
		if(const auto *state = request->requestState())
		{
			context.principal = state->principal;
			context.authenticated_provider = state->authenticated_provider;
		}

#endif

		auto terminal = [this](Context &ctx)
			{
				return ctx.response.started() ? Result::Error : endpoint_(ctx);
			};
		const auto result = pipeline_.run(context, EndpointRef(terminal), true);
		if(result == Result::Drop || response.dropped()) { response.drop(); return; }

		if(response.started())
		{
			if(result != Result::Complete)
				response.fail();

			return;
		}

		if(response.failed())
			return;

		if(result == Result::Unauthorized)
		{
#if NAWSERVER_AUTH
			if(context.authentication_challenge &&
			    response.header("WWW-Authenticate", context.authentication_challenge) != SendResult::Ok)
				return;

#endif

			(void)response.send(401, "text/plain", {reinterpret_cast<const uint8_t *>("Unauthorized"), 12});
		}
		else if(result == Result::Forbidden)
		{
			(void)response.send(403, "text/plain", {reinterpret_cast<const uint8_t *>("Forbidden"), 9});
		}
		else
		{
			(void)response.text(500, "Pipeline did not complete a response");
		}
	}

} // namespace NaWServer
