// SPDX-License-Identifier: MIT
#include "flash.hpp"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "pico/time.h"
#include "boot/picobin.h"
#include <NaWServer/detail/synchronization.h>
#include <cstring>

namespace compact::rp
{
	namespace
	{
		NaWServer::detail::Mutex flash_mutex;
		FlashTiming timing;
		bool operation(unsigned kind, std::uint32_t offset, void *bytes, std::size_t size)
		{
			if(!size || offset >= 0x400000 || size > 0x400000 - offset) return false;
			if(kind != CFLASH_OP_VALUE_READ && (offset < slot_a || offset + size > fs_offset + fs_bytes)) return false;
			// USB key/settings persistence and HTTP OTA share this coordinator.
			// No filesystem callback or application lock is acquired from here.
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(flash_mutex);
			if(!bootrom_try_acquire_lock(BOOTROM_LOCK_FLASH_OP)) return false;
			cflash_flags_t flags{(kind << CFLASH_OP_LSB) |
			    (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
			    (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)};
			int result = -1;
			rom_helper_flash_op_params_t parameters{flags, XIP_BASE + offset,
			    static_cast<std::uint32_t>(size), static_cast<std::uint8_t *>(bytes), &result};
			const auto started = time_us_64();
			const auto safety = flash_safe_execute(rom_helper_flash_op, &parameters, 1000);
			const auto elapsed = static_cast<std::uint32_t>(time_us_64() - started);
			++timing.calls; timing.total_us += elapsed;
			auto &maximum = kind == CFLASH_OP_VALUE_ERASE ? timing.max_erase_us : kind == CFLASH_OP_VALUE_PROGRAM ? timing.max_program_us : timing.max_read_us;
			if(elapsed > maximum) maximum = elapsed;
			bootrom_release_lock(BOOTROM_LOCK_FLASH_OP);
			return safety == PICO_OK && result == 0;
		}
	}
	FlashTiming flashTiming()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(flash_mutex);
		return timing;
	}
	bool flashRead(std::uint32_t offset, void *bytes, std::size_t size)
	{
		return operation(CFLASH_OP_VALUE_READ, offset, bytes, size);
	}
	bool flashProgram(std::uint32_t offset, const void *bytes, std::size_t size)
	{
		if(offset % 256 || size % 256) return false;
		return operation(CFLASH_OP_VALUE_PROGRAM, offset, const_cast<void *>(bytes), size);
	}
	bool flashErase(std::uint32_t offset)
	{
		return !(offset % 4096) && operation(CFLASH_OP_VALUE_ERASE, offset, nullptr, 4096);
	}
	bool buyImage(std::uint8_t *scratch, std::size_t size)
	{
		// Explicit buy rewrites image metadata, so it joins the same coordinator.
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> guard(flash_mutex);
		return rom_explicit_buy(scratch, static_cast<std::uint32_t>(size)) == PICO_OK;
	}
	bool partitionLayout()
	{
		if(rom_get_b_partition(0) != 1) return false;
		constexpr std::uint32_t starts[]{slot_a, slot_b, fs_offset, 0x3c0000};
		constexpr std::uint32_t sizes[]{slot_bytes, slot_bytes, fs_bytes, 0x40000};
		for(unsigned index = 0; index < 4; ++index)
		{
			std::uint32_t info[3]{};
			const auto flags = PT_INFO_SINGLE_PARTITION | PT_INFO_PARTITION_LOCATION_AND_FLAGS;
			if(rom_get_partition_table_info(info, 3, flags | (index << 24)) != 3 || info[0] != flags) return false;
			const auto first = (info[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
			const auto last = (info[1] & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
			if(first * 4096 != starts[index] || (last - first + 1) * 4096 != sizes[index]) return false;
		}
		return true;
	}
}
