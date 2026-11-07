// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include "backends/tcp/queued_transport.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
	using namespace NaWServer;
	using namespace NaWServer::tcp;
	using namespace NaWServer::tcp::detail;
	void check(bool value, int line)
	{
		if(!value) { std::printf("DROP_LIFECYCLE_FAIL %d\n", line); std::fflush(stdout); std::_Exit(1); }
	}
#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	template <class F> void until(F predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while(!predicate())
		{
			CHECK(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	class Native final : public Transport
	{
	public:
		std::atomic<bool> hold{false}, writing{false};
		std::atomic<unsigned> closes{0}, aborts{0}, writes{0}, bytes{0};
		ListenerResult openListener(const char *, std::uint16_t, int) override
		{ return {Status::Ok, 0, {1, true}, 1234, {}}; }
		AcceptResult accept(Handle) override { return {Status::Ok, 0, {2, true}, {}}; }
		IoResult readSome(Handle, std::uint8_t *, std::size_t) override { return {IoStatus::WouldBlock, 0, 0}; }
		IoResult writeSome(Handle, const std::uint8_t *, std::size_t size) override
		{
			writing = true;
			while(hold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			++writes; bytes += static_cast<unsigned>(size); writing = false;
			return {IoStatus::Progress, size, 0};
		}
		WaitResult wait(const Interest *, Ready *, std::size_t, std::uint32_t) override
		{ return {WaitStatus::Timeout, 0, 0}; }
		OperationResult shutdownWrite(Handle) override { return {}; }
		CloseResult close(Handle &handle) override { ++closes; handle = {}; return {Status::Ok, 0, true}; }
		CloseResult abort(Handle &handle) override { ++aborts; handle = {}; return {Status::Ok, 0, true}; }
	};
	TransmissionOptions configuration()
	{
		TransmissionOptions result;
		result.maximum_blocks = 6; result.maximum_blocks_per_connection = 4;
		result.maximum_bytes = 96; result.block_bytes = 16; result.step_bytes = 4;
		result.reserved_important_blocks = 0;
		return result;
	}
	void fillingLease()
	{
		Native native; QueuedTransport queue(native, configuration()); CHECK(queue.valid());
		auto handle = queue.openListener(nullptr, 0, 1).handle;
		auto filling = queue.acquireWrite(handle, 8); CHECK(filling);
		auto ready = queue.acquireWrite(handle, 8); CHECK(ready);
		std::memcpy(ready.data(), "12345678", 8); CHECK(ready.commit(8));
		CHECK(queue.abort(handle).status == Status::WouldBlock && handle.valid);
		// Producer still owns this memory. Abort cannot recycle it or accept commit.
		std::memcpy(filling.data(), "abcdefgh", 8); CHECK(!filling.commit(8));
		filling.reset(); CHECK(queue.abort(handle).released && !handle.valid);
		CHECK(native.aborts == 1 && native.closes == 0 && native.writes == 0);
		CHECK(queue.stats().queued_jobs == 0 && queue.stats().filling_jobs == 0);
		// Native descriptor reuse must get a new queue generation.
		handle = queue.openListener(nullptr, 0, 1).handle;
		CHECK(queue.writeSome(handle, reinterpret_cast<const std::uint8_t *>("next"), 4).size == 4);
		CHECK(queue.start()); until([&] { return native.bytes == 4; });
		CHECK(queue.close(handle).released && queue.stop(1000));
	}
	void writingLease()
	{
		Native native; native.hold = true;
		QueuedTransport queue(native, configuration()); CHECK(queue.valid());
		auto handle = queue.openListener(nullptr, 0, 1).handle;
		CHECK(queue.writeSome(handle, reinterpret_cast<const std::uint8_t *>("abcdefgh"), 8).size == 8);
		CHECK(queue.writeSome(handle, reinterpret_cast<const std::uint8_t *>("pending"), 7).size == 7);
		CHECK(queue.start()); until([&] { return native.writing.load(); });
		CHECK(queue.abort(handle).status == Status::WouldBlock && native.aborts == 0);
		native.hold = false;
		until([&] { return queue.abort(handle).released; });
		CHECK(native.aborts == 1 && native.closes == 0 && native.writes == 1 && native.bytes == 4);
		CHECK(queue.stats().queued_jobs == 0 && queue.stats().filling_jobs == 0);
		CHECK(queue.stop(1000));
	}
}
int main()
{
	fillingLease(); writingLease();
	std::puts("DROP_LIFECYCLE_PASS noflush filling/writing/cancel/generation");
}
