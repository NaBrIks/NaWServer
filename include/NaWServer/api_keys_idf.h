// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <NaWServer/api_keys.h>
namespace NaWServer::keys
{
#if NAWSERVER_API_KEYS
	// Uses only its supplied namespace; initialization and partition ownership are application responsibilities.
	class IdfStorage final : public Storage
	{
		const char *namespace_;
	public:
		explicit IdfStorage(const char *name = "naw_keys") : namespace_(name) {}
		ReadStatus read(unsigned record, void *bytes, std::size_t capacity, std::size_t &size) override;
		bool write(unsigned record, const void *bytes, std::size_t size) override;
	};
	// The application attests that a true hardware entropy source is active.
	// Missing/false readiness fails closed; this adapter never starts RF itself.
	class IdfRandom final : public Random
	{
		bool (*entropy_ready_)();
	public:
		explicit IdfRandom(bool (*entropy_ready)() = nullptr) : entropy_ready_(entropy_ready) {}

		bool fill(std::uint8_t *bytes, std::size_t size) override;
	};
#endif
}
