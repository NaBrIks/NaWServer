// SPDX-License-Identifier: MIT
#pragma once
#include "boot/uf2.h"
#include "boot/picobin.h"
#include <cstdint>
#include <cstring>
#include <cstddef>
namespace compact::rp
{
	struct Manifest { char product[24]; std::uint32_t schema, slot_size, fs_start, fs_size; };
	inline bool validMetadata(const std::uint8_t *bytes, std::size_t size, const Manifest &manifest)
	{
		bool product = false, image = false;
		for(std::size_t offset = 0; offset + sizeof(Manifest) <= size; offset += 4)
		{
			if(!std::memcmp(bytes + offset, &manifest, sizeof(manifest))) product = true;
			std::uint32_t word; std::memcpy(&word, bytes + offset, 4);
			if(word != PICOBIN_BLOCK_MARKER_START) continue;
			std::uint32_t item; std::memcpy(&item, bytes + offset + 4, 4);
			const auto expected = PICOBIN_IMAGE_TYPE_IMAGE_TYPE_EXE |
			    PICOBIN_IMAGE_TYPE_EXE_SECURITY_AS_BITS(S) | PICOBIN_IMAGE_TYPE_EXE_CHIP_AS_BITS(RP2350) |
			    PICOBIN_IMAGE_TYPE_EXE_TBYB_BITS;
			if((item & 0xffff) == (PICOBIN_BLOCK_ITEM_1BS_IMAGE_TYPE | 0x100) && (item >> 16) == expected) image = true;
		}
		return product && image;
	}
	inline bool validUf2Block(const uf2_block &block, std::uint32_t next, std::uint32_t count, std::uint32_t capacity)
	{
		return count && count <= capacity / 256 && next < count &&
		    block.magic_start0 == UF2_MAGIC_START0 && block.magic_start1 == UF2_MAGIC_START1 &&
		    block.magic_end == UF2_MAGIC_END && block.flags == UF2_FLAG_FAMILY_ID_PRESENT &&
		    block.file_size == RP2350_ARM_S_FAMILY_ID && block.payload_size == 256 &&
		    block.num_blocks == count && block.block_no == next && block.target_addr == 0x10000000u + next * 256;
	}
}
