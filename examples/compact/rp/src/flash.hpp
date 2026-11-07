// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
namespace compact::rp
{
	inline constexpr std::uint32_t slot_a = 0x1000, slot_b = 0x100000, slot_bytes = 0xff000;
	inline constexpr std::uint32_t fs_offset = 0x200000, fs_bytes = 0x1c0000;
	bool flashRead(std::uint32_t offset, void *bytes, std::size_t size);
	bool flashProgram(std::uint32_t offset, const void *bytes, std::size_t size);
	bool flashErase(std::uint32_t offset);
	bool buyImage(std::uint8_t *scratch, std::size_t size);
	struct FlashTiming { std::uint32_t calls = 0, max_erase_us = 0, max_program_us = 0, max_read_us = 0; std::uint64_t total_us = 0; };
	FlashTiming flashTiming();
	bool partitionLayout();
}
