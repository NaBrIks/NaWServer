// SPDX-License-Identifier: MIT
#include "uf2_validation.hpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
int main()
{
	uf2_block valid{};
	valid.magic_start0 = UF2_MAGIC_START0; valid.magic_start1 = UF2_MAGIC_START1; valid.magic_end = UF2_MAGIC_END;
	valid.flags = UF2_FLAG_FAMILY_ID_PRESENT; valid.file_size = RP2350_ARM_S_FAMILY_ID;
	valid.target_addr = 0x10000000; valid.payload_size = 256; valid.num_blocks = 16;
	auto check = [](bool result) { if(!result) std::abort(); };
	check(compact::rp::validUf2Block(valid, 0, 16, 0xff000));
	check(!compact::rp::validUf2Block(valid, 1, 16, 0xff000));
	check(!compact::rp::validUf2Block(valid, 0, 17, 0xff000));
	check(!compact::rp::validUf2Block(valid, 0, 16, 2048));
	for(unsigned field : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 127u})
	{
		auto changed = valid;
		reinterpret_cast<std::uint8_t *>(&changed)[field * 4] ^= 1;
		check(!compact::rp::validUf2Block(changed, 0, 16, 0xff000));
	}
	auto changed = valid; changed.target_addr = 0x10200000; check(!compact::rp::validUf2Block(changed, 0, 16, 0xff000));
	changed = valid; changed.file_size = RP2040_FAMILY_ID; check(!compact::rp::validUf2Block(changed, 0, 16, 0xff000));
	changed = valid; changed.flags |= UF2_FLAG_EXTENSION_FLAGS_PRESENT; check(!compact::rp::validUf2Block(changed, 0, 16, 0xff000));
	const compact::rp::Manifest manifest{{'N','a','W','S','e','r','v','e','r','-','c','o','m','p','a','c','t','-','P','i','c','o','2','W'}, 1, 0xff000, 0x200000, 0x1c0000};
	std::uint8_t sector[4096]{};
	std::memcpy(sector + 128, &manifest, sizeof(manifest));
	const std::uint32_t image[]{PICOBIN_BLOCK_MARKER_START, 0x90210142};
	std::memcpy(sector + 256, image, sizeof(image));
	check(compact::rp::validMetadata(sector, sizeof(sector), manifest));
	check(!compact::rp::validMetadata(sector, 132, manifest));
	sector[128] ^= 1; check(!compact::rp::validMetadata(sector, sizeof(sector), manifest)); sector[128] ^= 1;
	sector[128 + offsetof(compact::rp::Manifest, slot_size)] ^= 1;
	check(!compact::rp::validMetadata(sector, sizeof(sector), manifest));
	sector[128 + offsetof(compact::rp::Manifest, slot_size)] ^= 1;
	sector[263] ^= 0x80; check(!compact::rp::validMetadata(sector, sizeof(sector), manifest));
	std::puts("UF2_VALIDATION_PASS wrong-family/address/length/sequence/flags/capacity/magic");
}
