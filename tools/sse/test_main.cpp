// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "checks.hpp"
#include <NaWServer/detail/sse/session.hpp>
#include <cstdio>
int main()
{
	static_assert(__cplusplus == 201703L, "SSE experiment requires C++17");
	auto n = NaWServer::sse::checks();
	std::printf(
		"SSE_CHECKS passed=%zu sizeof_frame=%zu sizeof_sessions=%zu\n",
		n,
		sizeof(NaWServer::sse::Frame),
		sizeof(NaWServer::sse::Sessions));
}
