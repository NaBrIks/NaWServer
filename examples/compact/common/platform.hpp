// SPDX-License-Identifier: MIT
#pragma once
#include <NaWServer/api_keys.h>
#include <NaWServer/request_body_stream.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace compact
{

	struct Settings
	{
		char name[25] = "Device";
		std::uint32_t interval_ms = 2000;
		bool led = false;
	};

	struct Status
	{
		const char *platform = "unknown";
		const char *firmware = "compact-1";
		const char *running_slot = "";
		std::uint64_t uptime_ms = 0;
		std::size_t free_heap = 0, min_free_heap = 0;
		std::size_t total_heap = 0, largest_free_block = 0;
		const char *heap_scope = "unknown";
		std::size_t secondary_heap_total = 0, secondary_heap_free = 0;
		const char *secondary_heap_scope = "";
		std::size_t static_ram_bytes = 0, physical_ram_bytes = 0;
		double cpu_percent = 0;
		bool cpu_available = false;
		std::uint32_t cpu_cores = 0;
		std::uint64_t cpu_busy_us = 0, cpu_sample_us = 0;
		int rssi_dbm = 0;
	};

	// Application owns network, persistent storage and native OTA. No framework,
	// filesystem mount or board-specific concept leaks into the common server.
	class Platform
	{
	public:
		virtual ~Platform() = default;
		virtual NaWServer::keys::Store &keys() = 0;
		virtual bool authorized(std::string_view secret) { return keys().authenticate(secret).has_value(); }
		virtual std::uint32_t keyRevision() { return keys().metadata(0).revision; }
		// The unprotected demo management UI explicitly replaces the automation
		// credential. Native adapters serialize this with USB key management.
		virtual bool issueKey(NaWServer::keys::Secret &secret)
		{
			auto &store = keys();
			return (store.metadata(0).active ? store.rotate(0, secret) : store.create("API", secret)) ==
			       NaWServer::keys::Status::Ok;
		}
		virtual Settings settings() = 0;
		virtual bool configure(const Settings &settings) = 0;
		virtual Status status() = 0;
		virtual std::uint64_t milliseconds() { return status().uptime_ms; }
		virtual std::size_t maximumFirmwareBytes() = 0;
		virtual const char *firmwareFormat() = 0; // "bin" on ESP, "uf2" on RP.
		virtual NaWServer::BodyReceiver beginFirmware(std::size_t bytes, std::string_view sha256) = 0;
		// Called after a complete validated upload has produced its HTTP reply.
		virtual void scheduleRestart() = 0;
		// Short application tick: delayed reboot and boot health confirmation.
		virtual void poll() = 0;
	};

}
