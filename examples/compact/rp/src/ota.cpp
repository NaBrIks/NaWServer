// SPDX-License-Identifier: MIT
#include "ota.hpp"
#include "flash.hpp"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "boot/picoboot.h"
#include "uf2_validation.hpp"
#include <algorithm>
#include <cstring>
#include <FreeRTOS.h>
#include <task.h>

namespace compact::rp
{
	namespace
	{
		// Kept beside the SDK boot block, in the first sector. This identifies the
		// application/layout in addition to UF2's architecture-wide family ID.
		__attribute__((section(".embedded_block"), used, aligned(4)))
		const Manifest manifest{{'N','a','W','S','e','r','v','e','r','-','c','o','m','p','a','c','t','-','P','i','c','o','2','W'},
		    1, slot_bytes, fs_offset, fs_bytes};
		int hex(char c)
		{
			if(c >= '0' && c <= '9') return c - '0';
			if(c >= 'a' && c <= 'f') return c - 'a' + 10;
			if(c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		}
		bool programVerified(std::uint32_t offset, const std::uint8_t *page)
		{
			alignas(4) std::uint8_t verify[256];
			return flashProgram(offset, page, 256) && flashRead(offset, verify, sizeof(verify)) &&
			    std::memcmp(page, verify, sizeof(verify)) == 0;
		}
	}

	NaWServer::BodyReceiver Ota::begin(std::size_t bytes, std::string_view hash)
	{
		if(verified_.load() || hash.size() != 64 || bytes < 8192 || bytes % 512 || bytes > 2 * slot_bytes || busy_.exchange(true)) return {};
		verified_ = false;
		for(std::size_t index = 0; index < expected_hash_.size(); ++index)
		{
			const auto high = hex(hash[index * 2]), low = hex(hash[index * 2 + 1]);
			if(high < 0 || low < 0) { busy_ = false; return {}; }
			expected_hash_[index] = static_cast<std::uint8_t>((high << 4) | low);
		}
		boot_info_t boot{};
		const bool layout = partitionLayout(), boot_valid = rom_get_boot_info(&boot);
		if(!layout || !boot_valid || boot.partition < 0 || boot.partition > 1 ||
		    (boot.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING)) { busy_ = false; return {}; }
		resident_partition_t target{};
		const auto selected = rom_get_uf2_target_partition(work_.data(), work_.size(), RP2350_ARM_S_FAMILY_ID, &target);
		if(selected < 0)
		{ busy_ = false; return {}; }
		target_ = boot.partition == 0 ? slot_b : slot_a;
		const auto first_sector = target.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS;
		const auto last_sector = (target.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
		if(first_sector * 4096 != target_ || (last_sector - first_sector + 1) * 4096 != slot_bytes ||
		    pico_sha256_try_start(&hash_, SHA256_BIG_ENDIAN, false) != PICO_OK) { busy_ = false; return {}; }
		hashing_ = true; expected_ = bytes; received_ = used_ = 0; blocks_ = bytes / 512; next_ = 0; erased_ = 0;
		first_.fill(0xff);
		// The old candidate is made unbootable before any upload data is accepted.
		if(!flashErase(target_)) { cancel(); return {}; }
		erased_ = 4096;
		return {this, [](void *p, NaWServer::ByteView data) { return static_cast<Ota *>(p)->consume(data); },
		    [](void *p) { return static_cast<Ota *>(p)->finish(); },
		    [](void *p) { static_cast<Ota *>(p)->cancel(); },
		    [](void *p) { static_cast<Ota *>(p)->busy_ = false; }};
	}
	bool Ota::acceptBlock()
	{
		if(!validUf2Block(block_, next_, blocks_, slot_bytes)) return false;
		const auto relative = next_ * 256;
		if(relative < first_.size()) std::memcpy(first_.data() + relative, block_.data, 256);
		else
		{
			if(relative >= erased_)
			{
				if(!flashErase(target_ + relative)) return false;
				erased_ += 4096;
				// Let pending USB/radio service run after the long erase pause.
				vTaskDelay(1);
			}
			if(!programVerified(target_ + relative, block_.data)) return false;
		}
		++next_;
		return true;
	}
	NaWServer::BodyConsumeResult Ota::consume(NaWServer::ByteView input)
	{
		if(!hashing_ || input.size() > expected_ - received_ || input.size() > 1024) return NaWServer::BodyConsumeResult::Error;
		pico_sha256_update_blocking(&hash_, input.data(), input.size());
		received_ += input.size();
		while(!input.empty())
		{
			const auto size = std::min(input.size(), sizeof(block_) - used_);
			std::memcpy(reinterpret_cast<std::uint8_t *>(&block_) + used_, input.data(), size);
			used_ += size; input = {input.data() + size, input.size() - size};
			if(used_ == sizeof(block_)) { if(!acceptBlock()) return NaWServer::BodyConsumeResult::Error; used_ = 0; }
		}
		vTaskDelay(1);
		return NaWServer::BodyConsumeResult::Accepted;
	}
	bool Ota::finish()
	{
		if(!hashing_ || received_ != expected_ || used_ || next_ != blocks_ || !validMetadata(first_.data(), first_.size(), manifest))
			return false;
		sha256_result_t digest; pico_sha256_finish(&hash_, &digest); hashing_ = false;
		if(std::memcmp(digest.bytes, expected_hash_.data(), expected_hash_.size())) return false;
		// Publish the first page last. This is not an atomic sector write: the
		// ROM image hash still rejects a reset during any incomplete commit.
		for(unsigned offset = 256; offset < first_.size(); offset += 256)
			if(!programVerified(target_ + offset, first_.data() + offset)) { (void)flashErase(target_); return false; }
		if(!programVerified(target_, first_.data())) { (void)flashErase(target_); return false; }
		verified_ = true;
		return true;
	}
	void Ota::cancel()
	{
		if(hashing_) pico_sha256_cleanup(&hash_);
		hashing_ = false; verified_ = false; busy_ = false;
	}
	void Ota::reboot()
	{
		if(verified_) (void)rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 10, XIP_BASE + target_, 0);
	}
	bool Ota::confirmBoot()
	{
		boot_info_t boot{};
		if(!partitionLayout() || !rom_get_boot_info(&boot)) return false;
		return !(boot.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) ||
		    buyImage(work_.data(), work_.size());
	}
	const char *Ota::runningSlot()
	{
		boot_info_t boot{};
		return rom_get_boot_info(&boot) ? boot.partition == 0 ? "A" : boot.partition == 1 ? "B" : "unpartitioned" : "unknown";
	}
}
