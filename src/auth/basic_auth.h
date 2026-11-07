// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <string_view>
namespace NaWServer
{
	// RFC 7617 Basic token validation; no allocation and no credential logging.
	bool verifyBasicAuthorization(std::string_view authorization, std::string_view username, std::string_view password);
} // namespace NaWServer
