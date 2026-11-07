// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <NaWServer/api_keys.h>
#include <string>
#include <utility>
namespace NaWServer::keys
{
#if NAWSERVER_API_KEYS
	// Application supplies a private existing directory. Two snapshot files plus
	// a temporary sibling are owned by this adapter; no other files are touched.
	class FileStorage final : public Storage
	{
		std::string base_path_;
	public:
		explicit FileStorage(std::string base_path) : base_path_(std::move(base_path)) {}
		ReadStatus read(unsigned record, void *bytes, std::size_t capacity, std::size_t &size) override;
		bool write(unsigned record, const void *bytes, std::size_t size) override;
	};
	class SystemRandom final : public Random
	{
	public:
		bool fill(std::uint8_t *bytes, std::size_t size) override;
	};
#endif
}
