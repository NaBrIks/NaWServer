// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "websocket/wire/frame_codec.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace
{

	bool measure_allocations = false;
	std::size_t measured_allocations = 0;

} // namespace

void *operator new(std::size_t size)
{
	if(measure_allocations)
		++measured_allocations;

	if(void *storage = std::malloc(size ? size : 1))
		return storage;

	throw std::bad_alloc();
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void *storage) noexcept
{
	std::free(storage);
}

void operator delete[](void *storage) noexcept
{
	std::free(storage);
}

void operator delete(void *storage, std::size_t) noexcept
{
	std::free(storage);
}

void operator delete[](void *storage, std::size_t) noexcept
{
	std::free(storage);
}

namespace
{

	using namespace NaWServer::websocket::wire;
	unsigned checks = 0;
	unsigned split_runs = 0;

	void check(bool condition, int line)
	{
		++checks;
		if(!condition)
		{
			std::printf("WEBSOCKET_FRAME_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	struct Event
	{
		DecodeStatus status;
		std::vector<std::uint8_t> payload;
		bool binary;
		std::uint16_t code;
	};

	bool terminal(DecodeStatus status)
	{
		return status == DecodeStatus::Close || status == DecodeStatus::Error || status == DecodeStatus::InvalidState;
	}

	void capture(std::vector<Event> &events, const DecodeResult &result)
	{
		if(result.status != DecodeStatus::NeedMore)
		{
			Event event{result.status, {}, result.binary, result.close_code};
			if(!result.payload.empty())
				event.payload.assign(result.payload.data(), result.payload.data() + result.payload.size());

			events.push_back(std::move(event));
		}
	}

	void feed(FrameDecoder &decoder, const std::uint8_t *bytes, std::size_t size, std::vector<Event> &events)
	{
		std::size_t consumed = 0;
		while(consumed < size)
		{
			const DecodeResult result = decoder.consume(bytes + consumed, size - consumed);
			CHECK(result.consumed <= size - consumed);
			capture(events, result);
			if(terminal(result.status))
				break;

			CHECK(result.consumed > 0);
			consumed += result.consumed;
		}
	}

	std::vector<std::uint8_t> frame(std::uint8_t opcode, const std::vector<std::uint8_t> &payload,
	    bool final = true)
	{
		std::vector<std::uint8_t> result;
		result.push_back(static_cast<std::uint8_t>((final ? 0x80 : 0) | opcode));
		if(payload.size() <= 125)
			result.push_back(static_cast<std::uint8_t>(0x80 | payload.size()));
		else if(payload.size() <= 65535)
		{
			result.push_back(0xfe);
			result.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
			result.push_back(static_cast<std::uint8_t>(payload.size()));
		}
		else
		{
			result.push_back(0xff);
			for(unsigned index = 0; index < 8; ++index)
				result.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(payload.size()) >>
				    (56 - index * 8)));
		}

		constexpr std::array<std::uint8_t, 4> mask{{0x37, 0xfa, 0x21, 0x3d}};
		result.insert(result.end(), mask.begin(), mask.end());
		for(std::size_t index = 0; index < payload.size(); ++index)
			result.push_back(payload[index] ^ mask[index & 3]);

		return result;
	}

	void append(std::vector<std::uint8_t> &destination, const std::vector<std::uint8_t> &bytes)
	{
		destination.insert(destination.end(), bytes.begin(), bytes.end());
	}

	void expect(const std::vector<Event> &events, DecodeStatus expected, std::uint16_t code,
	    const std::vector<std::uint8_t> &payload, bool binary)
	{
		CHECK(events.size() == 1);
		CHECK(events[0].status == expected && events[0].code == code);
		if(expected != DecodeStatus::Error)
			CHECK(events[0].payload == payload);

		if(expected == DecodeStatus::Message)
			CHECK(events[0].binary == binary);
	}

	void everySplit(const std::vector<std::uint8_t> &wire, DecodeStatus expected,
	    std::uint16_t code = 0, const std::vector<std::uint8_t> &payload = {}, bool binary = false,
	    std::size_t limit = 4096)
	{
		std::array<std::uint8_t, 4098> storage;
		CHECK(limit <= 4096);
		for(std::size_t split = 0; split <= wire.size(); ++split)
		{
			storage.fill(0xa5);
			FrameDecoder decoder(storage.data() + 1, 4096, limit);
			std::vector<Event> events;
			feed(decoder, wire.data(), split, events);
			if(events.empty() || !terminal(events.back().status))
				feed(decoder, wire.data() + split, wire.size() - split, events);

			expect(events, expected, code, payload, binary);
			CHECK(storage.front() == 0xa5 && storage.back() == 0xa5);
			if(terminal(expected))
			{
				const auto sticky = decoder.consume(wire.data(), wire.size());
				CHECK(sticky.status == expected && sticky.consumed == 0 && sticky.close_code == code);
				CHECK(decoder.writable().size == 0);
			}

			++split_runs;
		}

		for(std::size_t chunk : {1u, 2u, 3u, 7u, 127u})
		{
			storage.fill(0xa5);
			FrameDecoder decoder(storage.data() + 1, 4096, limit);
			std::vector<Event> events;
			std::size_t consumed = 0;
			while(consumed < wire.size())
			{
				auto destination = decoder.writable();
				CHECK(destination.data && destination.size > 0);
				const auto size = std::min({chunk, destination.size, wire.size() - consumed});
				std::memcpy(destination.data, wire.data() + consumed, size);
				const auto result = decoder.commit(size);
				CHECK(result.consumed == size);
				capture(events, result);
				consumed += size;
				if(terminal(result.status))
					break;
			}
			expect(events, expected, code, payload, binary);
			CHECK(storage.front() == 0xa5 && storage.back() == 0xa5);
			++split_runs;
		}
	}

	void sizesAndDirectStorage()
	{
		for(std::size_t size : {0u, 1u, 125u, 126u, 127u, 4095u, 4096u})
		{
			std::vector<std::uint8_t> payload(size);
			for(std::size_t index = 0; index < size; ++index)
				payload[index] = static_cast<std::uint8_t>(index * 37);

			everySplit(frame(2, payload), DecodeStatus::Message, 0, payload, true);
		}
		everySplit(frame(2, std::vector<std::uint8_t>(4097)), DecodeStatus::Error, 1009);
		std::vector<std::uint8_t> payload(65536, 0x7b);
		const auto large = frame(2, payload);
		std::vector<std::uint8_t> storage(payload.size());
		FrameDecoder decoder(storage.data(), storage.size(), storage.size());
		std::size_t offset = 0;
		DecodeResult last;
		while(offset < large.size())
		{
			const auto destination = decoder.writable();
			CHECK(destination.size > 0);
			const auto size = std::min({std::size_t{127}, destination.size, large.size() - offset});
			if(offset >= 14)
				CHECK(destination.data == storage.data() + offset - 14);

			std::memcpy(destination.data, large.data() + offset, size);
			last = decoder.commit(size);
			CHECK(last.consumed == size);
			offset += size;
		}
		CHECK(last.status == DecodeStatus::Message && last.payload.data() == storage.data());
		CHECK(last.payload.size() == payload.size() && storage == payload);
	}

	void fragmentationAndControls()
	{
		std::vector<std::uint8_t> stream = frame(1, {0xe2}, false);
		append(stream, frame(9, {'p', 0}));
		append(stream, frame(0, {0x82}, false));
		append(stream, frame(10, {'q'}));
		append(stream, frame(0, {0xac}));
		append(stream, frame(2, {0, 0xff}));
		for(std::size_t split = 0; split <= stream.size(); ++split)
		{
			std::array<std::uint8_t, 4096> storage;
			FrameDecoder decoder(storage.data(), storage.size(), storage.size());
			std::vector<Event> events;
			feed(decoder, stream.data(), split, events);
			feed(decoder, stream.data() + split, stream.size() - split, events);
			CHECK(events.size() == 4);
			CHECK(events[0].status == DecodeStatus::Ping && events[0].payload == std::vector<std::uint8_t>({'p', 0}));
			CHECK(events[1].status == DecodeStatus::Pong && events[1].payload == std::vector<std::uint8_t>({'q'}));
			CHECK(events[2].status == DecodeStatus::Message && !events[2].binary &&
			    events[2].payload == std::vector<std::uint8_t>({0xe2, 0x82, 0xac}));
			CHECK(events[3].status == DecodeStatus::Message && events[3].binary &&
			    events[3].payload == std::vector<std::uint8_t>({0, 0xff}));
			++split_runs;
		}
		stream = frame(2, {1, 2}, false);
		append(stream, frame(0, {3, 4}));
		everySplit(stream, DecodeStatus::Message, 0, {1, 2, 3, 4}, true, 4);
		everySplit(stream, DecodeStatus::Error, 1009, {}, false, 3);
		stream = frame(1, {}, false);
		append(stream, frame(0, {}));
		everySplit(stream, DecodeStatus::Message);
		everySplit(frame(9, std::vector<std::uint8_t>(125, 0xff)), DecodeStatus::Ping, 0,
		    std::vector<std::uint8_t>(125, 0xff), false, 1);
		everySplit(frame(10, {}), DecodeStatus::Pong);
	}

	void protocolErrors()
	{
		for(std::uint8_t opcode : std::initializer_list<std::uint8_t>{3, 4, 5, 6, 7, 11, 12, 13, 14, 15})
			everySplit(frame(opcode, {}), DecodeStatus::Error, 1002);

		everySplit(frame(0, {}), DecodeStatus::Error, 1002);
		for(std::uint8_t reserved : std::initializer_list<std::uint8_t>{0x10, 0x20, 0x40})
		{
			auto wire = frame(2, {});
			wire[0] |= reserved;
			everySplit(wire, DecodeStatus::Error, 1002);
		}
		auto unmasked = frame(1, {'a'});
		unmasked[1] &= 0x7f;
		everySplit(unmasked, DecodeStatus::Error, 1002);
		everySplit(frame(9, {}, false), DecodeStatus::Error, 1002);
		everySplit(frame(8, std::vector<std::uint8_t>(126)), DecodeStatus::Error, 1002);
		everySplit({0x82, 0xfe, 0, 125, 0, 0, 0, 0}, DecodeStatus::Error, 1002);
		everySplit({0x82, 0xff, 0, 0, 0, 0, 0, 0, 0xff, 0xff}, DecodeStatus::Error, 1002);
		everySplit({0x82, 0xff, 0x80, 0, 0, 0, 0, 1, 0, 0}, DecodeStatus::Error, 1002);
		everySplit({0x82, 0xff, 0, 0, 0, 0, 0, 1, 0, 0}, DecodeStatus::Error, 1009);
		everySplit({0x82, 0xff, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, DecodeStatus::Error, 1009);
		auto nested = frame(1, {'a'}, false);
		append(nested, frame(2, {'b'}));
		everySplit(nested, DecodeStatus::Error, 1002);
		for(const auto &invalid : {std::vector<std::uint8_t>{0xc0, 0x80}, {0xed, 0xa0, 0x80},
								   {0xf4, 0x90, 0x80, 0x80}, {0xe2, 0x82}, {0x80}})
		{
			everySplit(frame(1, invalid), DecodeStatus::Error, 1007);
			everySplit(frame(2, invalid), DecodeStatus::Message, 0, invalid, true);
		}
	}

	void closingAndHeaders()
	{
		everySplit(frame(8, {}), DecodeStatus::Close, 1005);
		everySplit(frame(8, {1}), DecodeStatus::Error, 1002);
		for(std::uint16_t code : std::initializer_list<std::uint16_t>{1000, 1001, 1002, 1003, 1007, 1008, 1009,
		                                                              1010, 1011, 1012, 1013, 1014, 3000, 4999})
		{
			std::vector<std::uint8_t> payload{static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code),
				                              'x'};
			everySplit(frame(8, payload), DecodeStatus::Close, code, payload);
		}
		for(std::uint16_t code : std::initializer_list<std::uint16_t>{0, 999, 1004, 1005, 1006, 1015, 1016,
		                                                              2000, 2999, 5000, 65535})
			everySplit(frame(8, {static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code)}),
			    DecodeStatus::Error, 1002);

		everySplit(frame(8, {3, 0xe8, 0xc0, 0x80}), DecodeStatus::Error, 1007);
		FrameHeader output;
		for(std::size_t size : {0u, 1u, 125u, 126u, 127u, 65535u, 65536u})
		{
			CHECK(makeFrameHeader(2, size, output));
			CHECK(output.bytes[0] == 0x82 && !(output.bytes[1] & 0x80));
			CHECK(output.size == (size <= 125 ? 2u : size <= 65535 ? 4u : 10u));
			std::uint64_t length = output.bytes[1];
			if(output.size != 2)
			{
				length = 0;
				for(std::size_t index = 2; index < output.size; ++index)
					length = (length << 8) | output.bytes[index];
			}

			CHECK(length == size);
		}
		const auto saved = output;
		CHECK(!makeFrameHeader(0, 0, output));
		CHECK(!makeFrameHeader(9, 126, output));
		CHECK(!makeFrameHeader(8, 1, output));
		CHECK(output.bytes == saved.bytes && output.size == saved.size);
		if(std::numeric_limits<std::size_t>::max() > 0x7fffffffffffffffULL)
			CHECK(!makeFrameHeader(2, std::numeric_limits<std::size_t>::max(), output));
	}

	void invalidApiAndAllocation()
	{
		std::array<std::uint8_t, 8> storage;
		FrameDecoder absent(nullptr, 1, 1);
		CHECK(absent.writable().size == 0);
		CHECK(absent.consume(nullptr, 0).status == DecodeStatus::InvalidState);
		FrameDecoder invalid(storage.data(), storage.size(), storage.size() + 1);
		CHECK(invalid.commit(1).status == DecodeStatus::InvalidState);
		FrameDecoder decoder(storage.data(), storage.size(), storage.size());
		CHECK(decoder.consume(nullptr, 0).status == DecodeStatus::NeedMore);
		CHECK(decoder.commit(0).status == DecodeStatus::InvalidState);
		CHECK(decoder.consume(storage.data(), 1).status == DecodeStatus::InvalidState);
		decoder.reset();
		CHECK(decoder.commit(3).status == DecodeStatus::InvalidState);
		decoder.reset();
		CHECK(decoder.consume(nullptr, 1).status == DecodeStatus::InvalidState);

		const auto wire = frame(2, {1, 2, 3});
		measure_allocations = true;
		for(unsigned cycle = 0; cycle < 100; ++cycle)
		{
			decoder.reset();
			const auto result = decoder.consume(wire.data(), wire.size());
			CHECK(result.status == DecodeStatus::Message && result.payload.data() == storage.data());
			CHECK(result.payload.size() == 3 && result.payload.data()[2] == 3);
			decoder.reset();
			std::size_t offset = 0;
			while(offset < wire.size())
			{
				const auto destination = decoder.writable();
				const auto size = std::min(destination.size, wire.size() - offset);
				CHECK(size > 0);
				std::memcpy(destination.data, wire.data() + offset, size);
				const auto direct = decoder.commit(size);
				offset += size;
				CHECK(direct.status == (offset == wire.size() ? DecodeStatus::Message : DecodeStatus::NeedMore));
			}
		}
		measure_allocations = false;
		CHECK(measured_allocations == 0);
	}

} // namespace

int main()
{
	sizesAndDirectStorage();
	fragmentationAndControls();
	protocolErrors();
	closingAndHeaders();
	invalidApiAndAllocation();
	std::printf(
		"WEBSOCKET_FRAME_PASS checks=%u split_runs=%u decoder_bytes=%zu server_header_bytes=%zu allocations=%zu\n",
		checks, split_runs, sizeof(FrameDecoder), sizeof(FrameHeader), measured_allocations);
	return 0;
}
