// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/detail/sse/session.hpp>
namespace NaWServer::sse
{
	struct LastEventId
	{
		bool present = false;
		std::array<char, 129> bytes{};
		std::size_t size = 0;
	};
	enum class Admit
	{
		Owned,
		Full,
		InvalidLastId,
		InvalidRequest,
		NoMemory,
		Stopping
	};
	struct AbortHandle
	{
		void *context = nullptr;
		void (*abort)(void *) = nullptr;
	};
} // namespace NaWServer::sse
