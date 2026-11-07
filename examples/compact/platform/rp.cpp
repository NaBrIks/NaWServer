// SPDX-License-Identifier: MIT
#include "rp.hpp"
#include "../rp/src/flash.hpp"
#include <FreeRTOS.h>
#include <task.h>
#include "pico/time.h"
#include "pico/rand.h"
#include "pico/cyw43_arch.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <malloc.h>
#include "hardware/regs/addressmap.h"

extern "C" char end, __StackLimit;

namespace compact
{
	namespace
	{
		int readBlock(const lfs_config *, lfs_block_t block, lfs_off_t offset, void *bytes, lfs_size_t size)
		{
			return block < rp::fs_bytes / 4096 && offset + size <= 4096 &&
			    rp::flashRead(rp::fs_offset + block * 4096 + offset, bytes, size) ? 0 : LFS_ERR_IO;
		}
		int programBlock(const lfs_config *, lfs_block_t block, lfs_off_t offset, const void *bytes, lfs_size_t size)
		{
			return block < rp::fs_bytes / 4096 && offset + size <= 4096 &&
			    rp::flashProgram(rp::fs_offset + block * 4096 + offset, bytes, size) ? 0 : LFS_ERR_IO;
		}
		int eraseBlock(const lfs_config *, lfs_block_t block)
		{
			return block < rp::fs_bytes / 4096 && rp::flashErase(rp::fs_offset + block * 4096) ? 0 : LFS_ERR_IO;
		}
		int sync(const lfs_config *) { return 0; }
		const char *keyPath(unsigned record) { return record ? "/web.compact-keys.1" : "/web.compact-keys.0"; }
		const char *settingsPath(unsigned record) { return record ? "/web.compact-settings.1" : "/web.compact-settings.0"; }
		struct Record
		{
			std::uint32_t magic = 0x434d5054, schema = 1;
			std::uint64_t generation = 0;
			Settings settings{};
			std::uint32_t checksum = 0;
		};
		std::uint32_t checksum(const Record &record)
		{
			std::uint32_t value = 2166136261;
			const auto *bytes = reinterpret_cast<const std::uint8_t *>(&record);
			for(std::size_t i = 0; i < offsetof(Record, checksum); ++i) value = (value ^ bytes[i]) * 16777619;
			return value;
		}
	}
	bool RpPlatform::Files::mount()
	{
		config_.read = readBlock; config_.prog = programBlock; config_.erase = eraseBlock; config_.sync = sync;
		config_.read_size = config_.prog_size = config_.cache_size = 256;
		config_.block_size = 4096; config_.block_count = rp::fs_bytes / 4096; config_.block_cycles = 500;
		config_.lookahead_size = sizeof(lookahead_); config_.read_buffer = read_cache_;
		config_.prog_buffer = write_cache_; config_.lookahead_buffer = lookahead_;
		// Existing owner data is never formatted, seeded or silently repaired.
		mounted_ = lfs_mount(&filesystem_, &config_) == 0;
		return mounted_;
	}
	NaWServer::keys::ReadStatus RpPlatform::Files::readFile(const char *path, void *bytes, std::size_t capacity, std::size_t &size)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(mutex_);
		size = 0;
		if(!mounted_) return NaWServer::keys::ReadStatus::Error;
		lfs_file_t file{};
		const auto result = lfs_file_open(&filesystem_, &file, path, LFS_O_RDONLY);
		if(result == LFS_ERR_NOENT) return NaWServer::keys::ReadStatus::Missing;
		if(result < 0) return NaWServer::keys::ReadStatus::Error;
		const auto length = lfs_file_size(&filesystem_, &file);
		const auto count = length >= 0 && static_cast<std::size_t>(length) <= capacity ?
		    lfs_file_read(&filesystem_, &file, bytes, length) : static_cast<lfs_ssize_t>(LFS_ERR_INVAL);
		const auto closed = lfs_file_close(&filesystem_, &file);
		if(count < 0 || count != length || closed) return NaWServer::keys::ReadStatus::Error;
		size = count;
		return NaWServer::keys::ReadStatus::Ok;
	}
	bool RpPlatform::Files::writeFile(const char *path, const void *bytes, std::size_t size)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(mutex_);
		if(!mounted_ || size > 512) return false;
		lfs_file_t file{};
		if(lfs_file_open(&filesystem_, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) return false;
		const auto written = lfs_file_write(&filesystem_, &file, bytes, size);
		const auto synced = lfs_file_sync(&filesystem_, &file);
		const auto closed = lfs_file_close(&filesystem_, &file);
		return written == static_cast<lfs_ssize_t>(size) && !synced && !closed;
	}
	NaWServer::keys::ReadStatus RpPlatform::Files::read(unsigned record, void *bytes, std::size_t capacity, std::size_t &size)
	{
		return record < 2 ? readFile(keyPath(record), bytes, capacity, size) : NaWServer::keys::ReadStatus::Error;
	}
	bool RpPlatform::Files::write(unsigned record, const void *bytes, std::size_t size)
	{
		return record < 2 && writeFile(keyPath(record), bytes, size);
	}
	bool RpPlatform::Random::fill(std::uint8_t *bytes, std::size_t size)
	{
		while(size)
		{
			const auto value = get_rand_32(); const auto count = std::min(size, sizeof(value));
			std::memcpy(bytes, &value, count); bytes += count; size -= count;
		}
		return true;
	}
	bool RpPlatform::initialize()
	{
		if(!rp::partitionLayout() || !files_.mount() || keys_.load() != NaWServer::keys::Status::Ok) return false;
		if(!keys_.metadata(0).active && !recoverKey()) return false;
		for(unsigned index = 0; index < 2; ++index)
		{
			Record record{}; std::size_t size = 0;
			if(files_.readFile(settingsPath(index), &record, sizeof(record), size) == NaWServer::keys::ReadStatus::Ok &&
			    size == sizeof(record) && record.magic == 0x434d5054 && record.schema == 1 &&
			    std::memchr(record.settings.name, 0, sizeof(record.settings.name)) &&
			    record.settings.interval_ms >= 1000 && record.settings.interval_ms <= 60000 &&
			    record.checksum == checksum(record) && record.generation > generation_)
			{ settings_ = record.settings; generation_ = record.generation; current_ = index; }
		}
		cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, settings_.led);
		sampleMetrics();
		return true;
	}
	bool RpPlatform::authorized(std::string_view secret)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(keys_mutex_);
		return keys_.authenticate(secret).has_value();
	}
	std::uint32_t RpPlatform::keyRevision()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(keys_mutex_);
		return keys_.metadata(0).revision;
	}
	bool RpPlatform::issueKey(NaWServer::keys::Secret &secret)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(keys_mutex_);
		const auto result = keys_.metadata(0).active ? keys_.rotate(0, secret) : keys_.create("compact", secret);
		return result == NaWServer::keys::Status::Ok;
	}
	bool RpPlatform::recoverKey()
	{
		NaWServer::keys::Secret secret;
		if(!issueKey(secret)) return false;
		std::printf("COMPACT_API_KEY=%s\n", secret.value.data()); secret.clear(); return true;
	}
	Settings RpPlatform::settings()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(settings_mutex_); return settings_;
	}
	bool RpPlatform::configure(const Settings &settings)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(settings_mutex_);
		if(!std::memchr(settings.name, 0, sizeof(settings.name)) || generation_ == UINT64_MAX) return false;
		Record record{}; record.generation = generation_ + 1; record.settings = settings; record.checksum = checksum(record);
		const auto next = current_ ^ 1; Record readback{}; std::size_t size = 0;
		if(!files_.writeFile(settingsPath(next), &record, sizeof(record)) ||
		    files_.readFile(settingsPath(next), &readback, sizeof(readback), size) != NaWServer::keys::ReadStatus::Ok ||
		    size != sizeof(record) || std::memcmp(&record, &readback, size)) return false;
		settings_ = settings; current_ = next; generation_ = record.generation;
		cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, settings.led);
		return true;
	}
	Status RpPlatform::status()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(metrics_mutex_);
		auto status = metrics_;
		status.platform = "RP2350 Pico 2 W"; status.running_slot = rp::Ota::runningSlot();
		status.uptime_ms = time_us_64() / 1000;
		return status;
	}

	void RpPlatform::sampleMetrics()
	{
		if(sampled_at_us_ && time_us_64() - sampled_at_us_ < 1000000) return;
		// A pair from the same scheduling instant; the hardware timer keeps
		// advancing during flash operations even if SysTick interrupts are lost.
		taskENTER_CRITICAL();
		const auto idle = ulTaskGetIdleRunTimeCounter();
		const auto now = time_us_64();
		taskEXIT_CRITICAL();
		Status sample;
		HeapStats_t heap{};
		vPortGetHeapStats(&heap);
		sample.free_heap = heap.xAvailableHeapSpaceInBytes;
		sample.min_free_heap = heap.xMinimumEverFreeBytesRemaining;
		sample.total_heap = configTOTAL_HEAP_SIZE;
		sample.largest_free_block = heap.xSizeOfLargestFreeBlockInBytes;
		sample.heap_scope = "FreeRTOS";
		// The linker reserves main RAM below `end` for code/data/BSS, including
		// FreeRTOS and lwIP arenas. Newlib grows above it, up to __StackLimit;
		// interrupt/core stacks live in the separate scratch RAM banks.
		const auto heap_start = reinterpret_cast<std::uintptr_t>(&end);
		const auto heap_limit = reinterpret_cast<std::uintptr_t>(&__StackLimit);
		const auto native = mallinfo();
		sample.secondary_heap_scope = "newlib/C++";
		sample.secondary_heap_total = heap_limit - heap_start;
		// Includes free allocator chunks plus RAM not requested by sbrk yet.
		sample.secondary_heap_free = sample.secondary_heap_total -
		    std::min(sample.secondary_heap_total, native.uordblks);
		sample.static_ram_bytes = heap_start - SRAM_BASE;
		sample.physical_ram_bytes = SRAM_END - SRAM_BASE;
		sample.cpu_cores = configNUMBER_OF_CORES;
		sample.cpu_available = sampled_at_us_ != 0 && now > sampled_at_us_;
		if(sample.cpu_available)
		{
			const auto elapsed = now - sampled_at_us_;
			const auto idle_elapsed = std::min<std::uint64_t>(idle - sampled_idle_us_, elapsed);
			sample.cpu_percent = 100.0 * static_cast<double>(elapsed - idle_elapsed) / static_cast<double>(elapsed);
			cpu_busy_us_ += elapsed - idle_elapsed;
			cpu_sample_us_ += elapsed;
		}
		sample.cpu_busy_us = cpu_busy_us_;
		sample.cpu_sample_us = cpu_sample_us_;
		sampled_at_us_ = now; sampled_idle_us_ = idle;
		std::int32_t rssi = 0;
		(void)cyw43_wifi_get_rssi(&cyw43_state, &rssi);
		sample.rssi_dbm = rssi;
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(metrics_mutex_);
		metrics_ = sample;
	}
	std::uint64_t RpPlatform::milliseconds()
	{
		return time_us_64() / 1000;
	}
	NaWServer::BodyReceiver RpPlatform::beginFirmware(std::size_t bytes, std::string_view hash)
	{
		return confirmed_.load() ? ota_.begin(bytes, hash) : NaWServer::BodyReceiver{};
	}
	void RpPlatform::scheduleRestart()
	{
		if(ota_.verified()) restart_at_ = time_us_64() / 1000 + 1500;
	}
	void RpPlatform::poll()
	{
		sampleMetrics();
		if(healthy_ && !confirmed_)
		{
			confirmed_ = ota_.confirmBoot();
			if(confirmed_) std::printf("COMPACT_BOOT_CONFIRMED uptime_ms=%llu\n", static_cast<unsigned long long>(milliseconds()));
		}
		const auto deadline = restart_at_.load();
		if(deadline && time_us_64() / 1000 >= deadline) ota_.reboot();
	}
}
