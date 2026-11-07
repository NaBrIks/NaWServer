// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <cstdint>
#ifndef NAWSERVER_AUTH
#define NAWSERVER_AUTH 1
#endif
#ifndef NAWSERVER_CLIENT_QUOTAS
#define NAWSERVER_CLIENT_QUOTAS 1
#endif
namespace NaWServer
{
	using Permissions = std::uint32_t;
	struct AccessPolicy
	{
		bool requires_authentication{};
		Permissions required_permissions{};
		static constexpr AccessPolicy public_access()
		{
			return {};
		}

		static constexpr AccessPolicy authenticated()
		{
			return {true, 0};
		}

		static constexpr AccessPolicy require(Permissions permissions)
		{
			return {true, permissions};
		}

	};
	struct Principal
	{
		bool authenticated{};
		std::uint32_t user_id{};
		Permissions permissions{};
	};
}
