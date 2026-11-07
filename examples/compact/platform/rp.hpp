// SPDX-License-Identifier: MIT
#pragma once
#include "../common/platform.hpp"
#include "../rp/src/ota.hpp"
#include "../rp/src/flash.hpp"
#include <NaWServer/detail/synchronization.h>
#include "lfs.h"
#include <atomic>

namespace compact
{
	class RpPlatform final : public Platform
	{
		class Files final : public NaWServer::keys::Storage
		{
			NaWServer::detail::Mutex mutex_;
			lfs_t filesystem_{};
			lfs_config config_{};
			alignas(4) std::uint8_t read_cache_[256]{}, write_cache_[256]{}, lookahead_[64]{};
			bool mounted_ = false;
		public:
			bool mount();
			NaWServer::keys::ReadStatus readFile(const char *, void *, std::size_t, std::size_t &);
			bool writeFile(const char *, const void *, std::size_t);
			NaWServer::keys::ReadStatus read(unsigned, void *, std::size_t, std::size_t &) override;
			bool write(unsigned, const void *, std::size_t) override;
		} files_;
		class Random final : public NaWServer::keys::Random
		{
		public:
			bool fill(std::uint8_t *, std::size_t) override;
		} random_;
		NaWServer::keys::Store keys_{files_, random_};
		NaWServer::detail::Mutex keys_mutex_;
		NaWServer::detail::Mutex settings_mutex_;
		Settings settings_{};
		std::uint64_t generation_ = 0;
		unsigned current_ = 1;
		rp::Ota ota_;
		bool healthy_ = false;
		std::atomic<bool> confirmed_{false};
		std::atomic<std::uint64_t> restart_at_{0};
		NaWServer::detail::Mutex metrics_mutex_;
		Status metrics_{};
		std::uint64_t sampled_at_us_ = 0, sampled_idle_us_ = 0;
		std::uint64_t cpu_busy_us_ = 0, cpu_sample_us_ = 0;
		void sampleMetrics();
	public:
		bool initialize();
		void markHealthy(bool value) { healthy_ = value; }
#if NAWSERVER_DEMO_RP_STARTUP_MEASURE
		bool bootConfirmed() const { return confirmed_.load(); }
#endif
		NaWServer::keys::Store &keys() override { return keys_; }
		bool authorized(std::string_view) override;
		std::uint32_t keyRevision() override;
		bool recoverKey();
		bool issueKey(NaWServer::keys::Secret &) override;
		Settings settings() override;
		bool configure(const Settings &) override;
		Status status() override;
		std::uint64_t milliseconds() override;
		std::size_t maximumFirmwareBytes() override { return 2 * rp::slot_bytes; }
		const char *firmwareFormat() override { return "uf2"; }
		NaWServer::BodyReceiver beginFirmware(std::size_t, std::string_view) override;
		void scheduleRestart() override;
		void poll() override;
	};
}
