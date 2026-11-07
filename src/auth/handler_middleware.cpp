// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/handler_middleware.h>
#include <NaWServer/request.h>
#include "auth/basic_auth.h"
namespace NaWServer
{
	bool FixedBasicAuthenticator::authenticate(HeaderRequest &request, Principal &principal) const
	{
		principal = {};
		if(!username_ || !password_ || !user_id_)
			return false;

		char authorization[NAWSERVER_AUTHORIZATION_MAX_BYTES + 1];
		const auto field = request.readHeader("Authorization", authorization, sizeof(authorization));
		if(field.status != HeaderReadStatus::Ok ||
		    !verifyBasicAuthorization({authorization, field.size}, username_, password_))
			return false;

		principal = {true, user_id_, permissions_};
		return true;
	}

} // namespace NaWServer
