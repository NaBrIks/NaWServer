// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "websocket/wire/frame_codec.h"
#include "websocket/wire/handshake.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{

	using namespace NaWServer::websocket::wire;

	void require(bool value)
	{
		if(!value) std::abort();
	}

	bool terminal(DecodeStatus status)
	{
		return status == DecodeStatus::Close || status == DecodeStatus::Error || status == DecodeStatus::InvalidState;
	}

	struct Event
	{
		DecodeStatus status = DecodeStatus::NeedMore;
		std::size_t size = 0;
		std::uint64_t hash = 1469598103934665603ULL;
		bool binary = false;
		std::uint16_t code = 0;
	};

	struct Driver
	{
		std::uint64_t before = 0x185a3cfd74e0296bULL;
		std::array<std::uint8_t, 4096> bytes{};
		std::uint64_t after = 0x62ab8c71e3054fd9ULL;
		FrameDecoder decoder{bytes.data(), bytes.size(), bytes.size()};
		std::size_t offset = 0;
		bool done = false;

		bool next(const std::uint8_t *data, std::size_t size, std::size_t chunk, bool direct, Event &event)
		{
			if(done) return false;

			while(offset < size)
			{
				DecodeResult result;
				std::size_t amount = std::min(chunk, size - offset);
				if(direct)
				{
					const auto destination = decoder.writable();
					require(destination.data && destination.size);
					amount = std::min(amount, destination.size);
					std::memcpy(destination.data, data + offset, amount);
					result = decoder.commit(amount);
				}
				else
				{
					result = decoder.consume(data + offset, amount);
					require(result.consumed > 0 && result.consumed <= amount);
					amount = result.consumed;
				}

				offset += amount;
				require(before == 0x185a3cfd74e0296bULL && after == 0x62ab8c71e3054fd9ULL);
				require(result.status != DecodeStatus::InvalidState);
				if(result.status == DecodeStatus::NeedMore) continue;

				event = {result.status, result.payload.size(), 1469598103934665603ULL, result.binary,
					     result.close_code};
				require(event.size <= bytes.size() && (!event.size || result.payload.data()));
				for(std::size_t index = 0; index < event.size; ++index)
					event.hash = (event.hash ^ result.payload.data()[index]) * 1099511628211ULL;
				done = terminal(result.status);
				return true;
			}
			return false;
		}

	};

	void decodeInput(const std::uint8_t *data, std::size_t size)
	{
		Driver consume, direct;
		const auto chunk = size ? 1 + data[size - 1] % 127 : 1;
		for(std::size_t events = 0; events <= size; ++events)
		{
			Event first, second;
			const bool have_first = consume.next(data, size, chunk, false, first);
			const bool have_second = direct.next(data, size, 7, true, second);
			require(have_first == have_second);
			if(!have_first) return;

			require(first.status == second.status && first.size == second.size && first.hash == second.hash &&
			    first.binary == second.binary && first.code == second.code);
		}
		require(false);
	}

	void outputHeader(const std::uint8_t *data, std::size_t size)
	{
		std::size_t length = 0;
		for(std::size_t index = 0; index < std::min(size, sizeof(length)); ++index)
			length = (length << 8) | data[index];
		const auto opcode = size ? data[size - 1] : std::uint8_t{0};
		FrameHeader header;
		header.bytes.fill(0xa5);
		header.size = 123;
		if(!makeFrameHeader(opcode, length, header))
		{
			require(header.size == 123);
			for(auto byte : header.bytes)
				require(byte == 0xa5);
			return;
		}

		require(header.size == 2 || header.size == 4 || header.size == 10);
		require(header.bytes[0] == static_cast<std::uint8_t>(0x80 | opcode) && !(header.bytes[1] & 0x80));
		std::size_t decoded = header.bytes[1];
		if(header.size > 2)
		{
			decoded = 0;
			for(std::size_t index = 2; index < header.size; ++index)
				decoded = (decoded << 8) | header.bytes[index];
		}

		require(decoded == length);
	}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
	decodeInput(data, std::min(size, std::size_t{16384}));
	outputHeader(data, size);
	std::array<char, 31> accept;
	accept.fill(static_cast<char>(0xa5));
	const std::string_view key(size ? reinterpret_cast<const char *>(data) : "", std::min(size, std::size_t{256}));
	const bool valid = NaWServer::websocket::wire::makeWebSocketAccept(key, accept.data() + 1, 29);
	require(static_cast<unsigned char>(accept.front()) == 0xa5 && static_cast<unsigned char>(accept.back()) == 0xa5);
	if(valid) require(accept[29] == '\0');

	return 0;
}
