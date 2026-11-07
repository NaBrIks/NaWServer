// SPDX-License-Identifier: MIT
#pragma once
#include <NaWServer/request_body_stream.h>
#include "pico/sha256.h"
#include "boot/uf2.h"
#include <array>
#include <atomic>
#include <string_view>
namespace compact::rp
{
	class Ota
	{
		std::atomic<bool> busy_{false};
		std::atomic<bool> verified_{false};
		bool hashing_ = false;
		std::size_t expected_ = 0, received_ = 0, used_ = 0;
		std::uint32_t blocks_ = 0, next_ = 0, target_ = 0, erased_ = 0;
		alignas(4) uf2_block block_{};
		alignas(4) std::array<std::uint8_t, 4096> first_{};
		alignas(4) std::array<std::uint8_t, 4096> work_{};
		std::array<std::uint8_t, 32> expected_hash_{};
		pico_sha256_state_t hash_{};
		bool acceptBlock();
		NaWServer::BodyConsumeResult consume(NaWServer::ByteView);
		bool finish();
		void cancel();
	public:
		NaWServer::BodyReceiver begin(std::size_t bytes, std::string_view hash);
		bool verified() const { return verified_; }
		void reboot();
		bool confirmBoot();
		static const char *runningSlot();
	};
}
