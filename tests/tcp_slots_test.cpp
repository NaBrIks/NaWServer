// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "backends/tcp/connection_slots.h"
#include <cstdio>
#include <cstdlib>

using namespace NaWServer::tcp;
using namespace NaWServer::tcp::detail;

namespace
{

	unsigned checks = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_SLOTS_FAIL line=%d\n", line);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	class CountingTransport final : public Transport
	{
	public:
		unsigned writes = 0;
		unsigned closes = 0;
		Handle last_written;
		CloseResult next_close{Status::Ok, 0, true};
		ConnectionSlots *observed_slots = nullptr;
		Token observed_token;

		ListenerResult openListener(const char *, std::uint16_t, int) override
		{
			return {};
		}

		AcceptResult accept(Handle) override
		{
			return {};
		}

		IoResult readSome(Handle, std::uint8_t *, std::size_t) override
		{
			return {};
		}

		IoResult writeSome(Handle handle, const std::uint8_t *, std::size_t size) override
		{
			++writes;
			last_written = handle;
			return {IoStatus::Progress, size, 0};
		}

		WaitResult wait(const Interest *, Ready *, std::size_t, std::uint32_t) override
		{
			return {};
		}

		OperationResult shutdownWrite(Handle) override
		{
			return {};
		}

		CloseResult close(Handle &handle) override
		{
			CHECK(handle.valid);
			if(observed_slots)
			{
				CHECK(observed_slots->get(observed_token, Owner::Closing).valid);
				CHECK(!observed_slots->get(observed_token, Owner::Http).valid);
				CHECK(!observed_slots->get(observed_token, Owner::WebSocket).valid);
				CHECK(!observed_slots->get(observed_token, Owner::Sse).valid);
				CHECK(!observed_slots->transfer(observed_token, Owner::Closing, Owner::Sse));
				CHECK(!observed_slots->prepareClose(observed_token, Owner::Closing).valid);
			}

			++closes;
			if(next_close.released)
				handle = {};

			return next_close;
		}

	};

	bool queuedWrite(ConnectionSlots &slots, CountingTransport &transport, Token token)
	{
		const auto handle = slots.get(token, Owner::WebSocket);
		if(!handle.valid)
			return false;

		const std::uint8_t payload = 42;
		return transport.writeSome(handle, &payload, 1).status == IoStatus::Progress;
	}

	CloseResult closeOwned(ConnectionSlots &slots, CountingTransport &transport, Token token, Owner owner)
	{
		auto handle = owner == Owner::Closing ? slots.get(token, owner) : slots.prepareClose(token, owner);
		if(!handle.valid)
			return {Status::InvalidState, 0, false};

		// A future runtime releases the table lock around this native operation.
		CHECK(!slots.get(token, Owner::WebSocket).valid);
		CHECK(!slots.get(token, Owner::Http).valid);
		transport.observed_slots = &slots;
		transport.observed_token = token;
		const auto result = transport.close(handle);
		transport.observed_slots = nullptr;
		CHECK(slots.completeClose(token, result));
		return result;
	}

}

int main()
{
	ConnectionSlot storage[1];
	ConnectionSlots slots(storage, 1);
	CountingTransport transport;
	Handle first{0, true};
	CHECK(slots.active() == 0);
	const auto first_token = slots.claim(first, Owner::Http);
	CHECK(first_token.valid() && first_token.slot == 0 && first_token.generation == 1);
	CHECK(!first.valid && slots.active() == 1);
	CHECK(slots.get(first_token, Owner::Http).valid);
	CHECK(slots.get(first_token, Owner::Http).value == 0);
	CHECK(!slots.get(first_token, Owner::Sse).valid);
	CHECK(!slots.get({}, Owner::Http).valid);
	CHECK(!slots.get({1, 1}, Owner::Http).valid);
	CHECK(!slots.transfer(first_token, Owner::Http, Owner::None));
	CHECK(slots.transfer(first_token, Owner::Http, Owner::WebSocket));
	CHECK(!slots.get(first_token, Owner::Http).valid);
	CHECK(!closeOwned(slots, transport, first_token, Owner::Http).released);
	CHECK(transport.closes == 0);
	CHECK(queuedWrite(slots, transport, first_token));
	CHECK(transport.writes == 1 && transport.last_written.value == 0);
	Handle full_source{12, true};
	CHECK(!slots.claim(full_source, Owner::Http).valid() && full_source.valid);
	Handle duplicate{0, true};
	CHECK(!slots.claim(duplicate, Owner::Http).valid() && duplicate.valid);
	CHECK(closeOwned(slots, transport, first_token, Owner::WebSocket).released);
	CHECK(transport.closes == 1 && slots.active() == 0);
	CHECK(!closeOwned(slots, transport, first_token, Owner::WebSocket).released);
	CHECK(transport.closes == 1);

	// Reuse the exact native value; stale jobs cannot touch the new lifetime.
	Handle reused{0, true};
	const auto second_token = slots.claim(reused, Owner::WebSocket);
	CHECK(second_token.valid() && second_token.generation == 2);
	CHECK(!slots.completeClose(first_token, {Status::Ok, 0, true}));
	CHECK(!slots.completeClose(second_token, {Status::Ok, 0, true}));
	CHECK(!queuedWrite(slots, transport, first_token));
	CHECK(!closeOwned(slots, transport, first_token, Owner::WebSocket).released);
	CHECK(transport.writes == 1 && transport.closes == 1);
	CHECK(queuedWrite(slots, transport, second_token));
	CHECK(transport.writes == 2);

	// Failed cleanup retains ownership; release-with-error never retries the fd.
	transport.next_close = {Status::WouldBlock, 123, false};
	const auto retained = closeOwned(slots, transport, second_token, Owner::WebSocket);
	CHECK(retained.native_error == 123 && !retained.released);
	CHECK(slots.active() == 1 && slots.get(second_token, Owner::Closing).valid);
	CHECK(!slots.get(second_token, Owner::WebSocket).valid);
	CHECK(!slots.transfer(second_token, Owner::Closing, Owner::Sse));
	CHECK(!slots.prepareClose(second_token, Owner::Closing).valid);
	CHECK(!slots.claim(full_source, Owner::Http).valid() && full_source.valid);
	transport.next_close = {Status::Fatal, 456, true};
	const auto released = closeOwned(slots, transport, second_token, Owner::Closing);
	CHECK(released.native_error == 456 && released.released);
	CHECK(slots.active() == 0 && transport.closes == 3);
	CHECK(!closeOwned(slots, transport, second_token, Owner::WebSocket).released);
	CHECK(transport.closes == 3);

	storage[0].generation = std::numeric_limits<std::uint64_t>::max() - 1;
	const auto final_token = slots.claim(full_source, Owner::Http);
	CHECK(final_token.valid() && final_token.generation == std::numeric_limits<std::uint64_t>::max());
	CHECK(!full_source.valid);
	transport.next_close = {Status::Ok, 0, true};
	CHECK(closeOwned(slots, transport, final_token, Owner::Http).released);
	CHECK(storage[0].retired && slots.active() == 0);
	Handle after_exhaustion{12, true};
	CHECK(!slots.claim(after_exhaustion, Owner::Http).valid() && after_exhaustion.valid);
	CHECK(transport.closes == 4);
	ConnectionSlots absent(nullptr, 1);
	CHECK(!absent.claim(after_exhaustion, Owner::Http).valid());
	CHECK(!slots.claim(after_exhaustion, Owner::None).valid());
	CHECK(!slots.claim(reused, Owner::Http).valid());
	CHECK(!slots.completeClose(final_token, {Status::Ok, 0, true}));

	// A duplicate must fail even when a different slot is free.
	ConnectionSlot two_storage[2];
	ConnectionSlots two_slots(two_storage, 2);
	Handle original{42, true};
	const auto original_token = two_slots.claim(original, Owner::Sse);
	Handle same_native{42, true};
	CHECK(!two_slots.claim(same_native, Owner::Http).valid() && same_native.valid);
	Handle distinct{43, true};
	const auto distinct_token = two_slots.claim(distinct, Owner::Http);
	CHECK(distinct_token.valid() && distinct_token.slot != original_token.slot);
	CHECK(closeOwned(two_slots, transport, original_token, Owner::Sse).released);
	CHECK(closeOwned(two_slots, transport, distinct_token, Owner::Http).released);

	ConnectionSlot exhausted_storage[1];
	exhausted_storage[0].generation = std::numeric_limits<std::uint64_t>::max();
	ConnectionSlots exhausted(exhausted_storage, 1);
	CHECK(!exhausted.claim(after_exhaustion, Owner::Http).valid());
	CHECK(after_exhaustion.valid && exhausted_storage[0].retired);
	std::printf("TCP_SLOTS checks=%u slot_bytes=%zu seven_slots_bytes=%zu table_bytes=%zu PASS\n",
	    checks, sizeof(ConnectionSlot), sizeof(ConnectionSlot) * 7, sizeof(ConnectionSlots));
}
