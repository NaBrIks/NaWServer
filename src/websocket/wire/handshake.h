// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include "http/wire/request_parser.h"
#include <cstddef>
#include <string_view>

namespace NaWServer::websocket::wire
{

	bool validWebSocketKey(std::string_view key);
	// Protocol handshake only, not a password/signature primitive. Capacity must
	// include 28 Base64 characters and NUL. Failure leaves destination untouched.
	bool makeWebSocketAccept(std::string_view key, char *destination, std::size_t capacity);
	// Call only after RequestParser Complete. Zero means a valid upgrade.
	// Every unsupported/malformed handshake returns
	// 400, preserving the existing Windows subset. No reservation or I/O occurs.
	int validateHandshake(const http::wire::RequestParser &request);

} // namespace NaWServer::websocket::wire
