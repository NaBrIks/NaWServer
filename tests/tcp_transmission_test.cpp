// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include "backends/tcp/queued_transport.h"
#include "backends/tcp/http_connection_manager.h"
#include <NaWServer/backends/tcp.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace
{

	using namespace NaWServer;
	using namespace NaWServer::tcp;
	using namespace NaWServer::tcp::detail;
	unsigned checks = 0;
	void check(bool value, int line)
	{
		++checks;
		if(!value) { std::printf("TCP_TRANSMISSION_FAIL line=%d\n", line); std::fflush(stdout); std::_Exit(1); }
	}
#define CHECK(value) check(static_cast<bool>(value), __LINE__)

	template <class Predicate> void until(Predicate predicate)
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
		mutable std::mutex mutex_;
		std::array<std::string, 3> wire_;
	public:
		std::atomic<bool> block_first{true};
		std::atomic<int> first_error{0};
		std::atomic<unsigned> delay_ms{0};
		std::atomic<bool> hold_write{false};
		std::atomic<unsigned> writes_in_progress{0}, maximum_writers{0};
		std::atomic<std::uint32_t> last_wait_timeout{0};
		ListenerResult openListener(const char *, std::uint16_t, int) override
		{
			return {Status::Ok, 0, {1, true}, 1234, {}};
		}
		AcceptResult accept(Handle) override { return {Status::Ok, 0, {2, true}, {}}; }
		IoResult readSome(Handle, std::uint8_t *, std::size_t) override { return {IoStatus::WouldBlock, 0, 0}; }
		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override
		{
			const auto active = writes_in_progress.fetch_add(1) + 1;
			auto maximum = maximum_writers.load();
			while(maximum < active && !maximum_writers.compare_exchange_weak(maximum, active)) {}

			struct Release
			{
				std::atomic<unsigned> &active;
				~Release() { --active; }
			} release{writes_in_progress};
			if(handle.value == 1 && first_error.load()) return {IoStatus::Fatal, 0, first_error.load()};

			if(handle.value == 1 && block_first.load()) return {IoStatus::WouldBlock, 0, 0};

			while(hold_write.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

			if(delay_ms.load()) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));

			std::lock_guard<std::mutex> lock(mutex_);
			const auto accepted = std::min(size, std::size_t{3});
			wire_[handle.value].append(reinterpret_cast<const char *>(bytes), accepted);
			return {IoStatus::Progress, accepted, 0};
		}
		WaitResult wait(const Interest *, Ready *ready, std::size_t count, std::uint32_t timeout) override
		{
			last_wait_timeout.store(timeout);
			for(std::size_t index = 0; index < count; ++index) ready[index] = {};

			return {WaitStatus::Timeout, 0, 0};
		}
		OperationResult shutdownWrite(Handle) override { return {}; }
		CloseResult close(Handle &handle) override { handle = {}; return {Status::Ok, 0, true}; }
		std::string wire(std::size_t index) const { std::lock_guard<std::mutex> lock(mutex_); return wire_[index]; }
	};

	class CapacityNative final : public Transport
	{
		std::uint32_t next_ = 1;
		std::array<std::string, 128> incoming_;
	public:
		std::size_t closes = 0;
		bool retain_next_close = false;
		std::uintptr_t retained_value = 0;

		void feed(Handle handle, const char *bytes)
		{
			CHECK(handle.valid && handle.value < incoming_.size());
			incoming_[handle.value] = bytes;
		}

		std::size_t unreadBytes(Handle handle) const
		{
			CHECK(handle.valid && handle.value < incoming_.size());
			return incoming_[handle.value].size();
		}

		ListenerResult openListener(const char *, std::uint16_t, int) override
		{
			return {Status::Ok, 0, {next_++, true}, 1234, {}};
		}

		AcceptResult accept(Handle) override
		{
			return {Status::Ok, 0, {next_++, true}, {}};
		}

		IoResult readSome(Handle handle, std::uint8_t *bytes, std::size_t size) override
		{
			if(handle.value >= incoming_.size()) return {IoStatus::Fatal, 0, 0};

			auto &incoming = incoming_[handle.value];
			if(incoming.empty()) return {IoStatus::WouldBlock, 0, 0};

			const auto count = std::min(size, incoming.size());
			std::memcpy(bytes, incoming.data(), count);
			incoming.erase(0, count);
			return {IoStatus::Progress, count, 0};
		}
		IoResult writeSome(Handle, const std::uint8_t *, std::size_t size) override
		{
			return {IoStatus::Progress, size, 0};
		}

		WaitResult wait(const Interest *, Ready *ready, std::size_t count, std::uint32_t) override
		{
			for(std::size_t index = 0; index < count; ++index) ready[index] = {};

			return {WaitStatus::Timeout, 0, 0};
		}

		OperationResult shutdownWrite(Handle) override { return {}; }
		CloseResult close(Handle &handle) override
		{
			++closes;
			if(retain_next_close)
			{
				retain_next_close = false;
				retained_value = handle.value;
				return {Status::WouldBlock, 11, false};
			}

			handle = {};
			return {Status::Ok, 0, true};
		}
	};

	struct MetadataAllocations
	{
		std::size_t calls = 0, releases = 0, bytes = 0, released_bytes = 0;
		std::size_t fail_call = 0;
		std::size_t watch_bytes = 0, watched_calls = 0, watched_releases = 0;
		bool fail = false;
		bool misalign = false;
		void *misaligned_raw = nullptr;

		BufferAllocator allocator()
		{
			BufferAllocator result;
			result.context = this;
			result.allocate = [](void *context, std::size_t size) -> void *
			{
				auto &self = *static_cast<MetadataAllocations *>(context);
				++self.calls;
				self.bytes += size;
				if(size == self.watch_bytes) ++self.watched_calls;
				if(self.fail || self.calls == self.fail_call) return nullptr;

				if(self.misalign)
				{
					self.misaligned_raw = std::malloc(size + 1);
					return self.misaligned_raw ? static_cast<std::uint8_t *>(self.misaligned_raw) + 1 : nullptr;
				}

				return std::malloc(size);
			};
			result.release = [](void *context, void *storage, std::size_t size)
			{
				auto &self = *static_cast<MetadataAllocations *>(context);
				++self.releases;
				self.released_bytes += size;
				if(size == self.watch_bytes) ++self.watched_releases;
				std::free(self.misalign ? self.misaligned_raw : storage);
				self.misaligned_raw = nullptr;
			};
			return result;
		}
	};

	struct StaticMetadata
	{
		alignas(std::max_align_t) std::array<std::uint8_t, 4096> bytes{};
		bool used = false;
		std::size_t calls = 0, releases = 0, limit = bytes.size();

		BufferAllocator allocator()
		{
			BufferAllocator result;
			result.context = this;
			result.allocate = [](void *context, std::size_t size) -> void *
			{
				auto &self = *static_cast<StaticMetadata *>(context);
				++self.calls;
				if(self.used || size > self.limit || size > self.bytes.size()) return nullptr;

				self.used = true;
				return self.bytes.data();
			};
			result.release = [](void *context, void *storage, std::size_t)
			{
				auto &self = *static_cast<StaticMetadata *>(context);
				CHECK(self.used && storage == self.bytes.data());
				self.used = false;
				++self.releases;
			};
			return result;
		}
	};

	void staticQueue()
	{
		Native native;
		std::array<std::uint8_t, 96> payload{};
		TransmissionOptions options;
		options.maximum_blocks = 6; options.maximum_bytes = payload.size(); options.block_bytes = 16;
		options.maximum_blocks_per_connection = 3; options.step_bytes = 4;
		options.static_payload = payload.data(); options.static_payload_bytes = payload.size();
		options.close_timeout_ms = 5;
		QueuedTransport queue(native, options);
		CHECK(queue.valid());
		auto first = queue.openListener("127.0.0.1", 0, 1).handle;
		auto second = queue.accept(first).handle;
		auto filling = queue.acquireWrite(first, 6);
		CHECK(filling && filling.capacity() == 6);
		std::memcpy(filling.data(), "ABCDEF", 6);
		auto ready = queue.acquireWrite(second, 6);
		CHECK(ready);
		std::memcpy(ready.data(), "second", 6);
		CHECK(ready.commit(6));
		auto expired = queue.acquireWrite(second, 4, {3, tcpMilliseconds() - 1, true});
		CHECK(expired);
		std::memcpy(expired.data(), "drop", 4);
		CHECK(expired.commit(4));
		CHECK(queue.stats().filling_jobs == 1 && queue.stats().queued_jobs == 2);
		CHECK(queue.start());
		until([&] { return native.wire(2) == "second" && queue.stats().discarded_jobs == 1; });
		CHECK(native.wire(1).empty());
		CHECK(filling.commit(6));
		// A blocked native socket cannot monopolize scheduling for other handles.
		auto other = queue.acquireWrite(second, 4);
		CHECK(other);
		std::memcpy(other.data(), "next", 4);
		CHECK(other.commit(4));
		until([&] { return native.wire(2) == "secondnext"; });
		native.block_first.store(false);
		CHECK(queue.flushWrites(first, tcpMilliseconds() + 2000));
		CHECK(native.wire(1) == "ABCDEF");
		const auto stats = queue.stats();
		CHECK(stats.step_bytes == options.step_bytes && stats.stall_timeout_ms == options.stall_timeout_ms &&
		    stats.close_timeout_ms == options.close_timeout_ms);
		CHECK(stats.sent_bytes == 16 && stats.discarded_bytes == 4 && stats.accepted_bytes == 20);
		CHECK(stats.queued_jobs == 0 && stats.filling_jobs == 0 && stats.allocated_bytes == payload.size());
		auto owned = queue.acquireWrite(first, 8);
		CHECK(owned);
		CHECK(queue.close(first).status == Status::WouldBlock && first.valid);
		owned.reset();
		CHECK(queue.close(first).released && !first.valid);
		CHECK(queue.close(second).released && queue.stop(1000));
	}

	void dynamicQueue()
	{
		Native native;
		native.block_first.store(false);
		TransmissionOptions options;
		options.maximum_blocks = 4; options.block_bytes = 16; options.maximum_bytes = 32;
		options.reserved_important_blocks = 1; options.maximum_blocks_per_connection = 4;
		QueuedTransport queue(native, options);
		CHECK(queue.valid());
		auto handle = queue.openListener("127.0.0.1", 0, 1).handle;
		auto one = queue.acquireWrite(handle, 16);
		CHECK(one && !queue.acquireWrite(handle, 1));
		queue.setWritePolicy(handle, {2, 0, false});
		auto two = queue.acquireWrite(handle, 16);
		CHECK(one && two && !queue.acquireWrite(handle, 1));
		CHECK(queue.stats().allocated_bytes == 32);
		one.reset(); two.reset();
		queue.setWritePolicy(handle, {});
		CHECK(queue.stats().allocated_bytes == 0 && queue.stats().filling_jobs == 0);
		auto old_value = queue.acquireWrite(handle, 3, {0, 0, false, 77});
		CHECK(old_value);
		std::memcpy(old_value.data(), "old", 3);
		CHECK(old_value.commit(3));
		auto new_value = queue.acquireWrite(handle, 3, {0, 0, false, 77});
		CHECK(new_value);
		std::memcpy(new_value.data(), "new", 3);
		CHECK(new_value.commit(3));
		CHECK(queue.stats().discarded_jobs == 1 && queue.stats().discarded_bytes == 3);
		CHECK(queue.start());
		const std::uint8_t data[]{1, 2, 3, 4};
		CHECK(queue.writeSome(handle, data, sizeof(data)).size == sizeof(data));
		CHECK(queue.flushWrites(handle, tcpMilliseconds() + 1000));
		CHECK(queue.stats().allocated_bytes == 0 && queue.stats().completed_jobs == 2);
		CHECK(native.wire(1).substr(0, 3) == "new");
		static constexpr std::uint8_t immutable[32] = {'i'};
		native.block_first.store(true);
		CHECK(queue.writeImmutableSome(handle, immutable, sizeof(immutable)).size == 16);
		CHECK(queue.writeImmutableSome(handle, immutable + 16, 16).size == 16);
		CHECK(queue.stats().borrowed_bytes == 32 && queue.stats().allocated_bytes == 0);
		native.block_first.store(false);
		CHECK(queue.flushWrites(handle, tcpMilliseconds() + 1000));
		CHECK(queue.stats().borrowed_bytes == 0 && queue.stats().completed_jobs == 4);
		CHECK(queue.close(handle).released && queue.stop(1000));
	}

	void waitingBehindProgressIsNotStall()
	{
		Native native;
		native.block_first.store(false); native.delay_ms.store(5);
		TransmissionOptions options;
		options.block_bytes = 64; options.maximum_bytes = 192; options.maximum_blocks = 4;
		options.reserved_important_blocks = 0; options.stall_timeout_ms = 30; options.step_bytes = 3;
		QueuedTransport queue(native, options);
		CHECK(queue.valid());
		auto handle = queue.openListener("127.0.0.1", 0, 1).handle;
		auto first = queue.acquireWrite(handle, 48);
		auto second = queue.acquireWrite(handle, 12);
		CHECK(first && second);
		std::memset(first.data(), 'a', first.capacity());
		std::memset(second.data(), 'b', second.capacity());
		CHECK(first.commit(48) && second.commit(12));
		CHECK(queue.start() && queue.flushWrites(handle, tcpMilliseconds() + 2000));
		CHECK(native.wire(1) == std::string(48, 'a') + std::string(12, 'b'));
		CHECK(queue.stats().failed_jobs == 0);
		CHECK(queue.close(handle).released && queue.stop(1000));
	}

	void allocatorFloor()
	{
		struct Allocation { std::size_t free = 64; } allocation;
		TransmissionOptions options;
		options.maximum_blocks = 4; options.block_bytes = 16; options.maximum_bytes = 64;
		options.reserved_important_blocks = 0; options.min_free_bytes = 48;
		options.allocator.context = &allocation;
		options.allocator.allocate = [](void *context, std::size_t size) -> void *
		{
			auto &state = *static_cast<Allocation *>(context);
			state.free -= size;
			return std::malloc(size);
		};
		options.allocator.release = [](void *context, void *bytes, std::size_t size)
		{
			static_cast<Allocation *>(context)->free += size;
			std::free(bytes);
		};
		options.allocator.available = [](void *context, BufferAvailability &available)
		{
			available.free_bytes = available.largest_block_bytes = static_cast<Allocation *>(context)->free;
			return true;
		};
		Native native;
		QueuedTransport queue(native, options);
		CHECK(queue.valid());
		auto handle = queue.openListener("127.0.0.1", 0, 1).handle;
		auto block = queue.acquireWrite(handle, 16);
		CHECK(block && allocation.free == 48 && !queue.acquireWrite(handle, 1));
		block.reset();
		CHECK(allocation.free == 64 && queue.close(handle).released);
	}

	void metadataLimitsAndLifetime()
	{
		CapacityNative native;
		MetadataAllocations allocations;
		alignas(std::max_align_t) std::array<std::uint8_t, 32> payload{};
		TransmissionOptions options;
		options.maximum_blocks = 2;
		options.block_bytes = 16;
		options.maximum_bytes = payload.size();
		options.maximum_blocks_per_connection = 2;
		options.reserved_important_blocks = 0;
		options.static_payload = payload.data();
		options.static_payload_bytes = payload.size();
		options.close_timeout_ms = 5;
		options.metadata_allocator = allocations.allocator();
		{
			// One slot is needed for the listener, leaving exactly two clients.
			QueuedTransport queue(native, options, 3);
			CHECK(queue.valid());
			const auto started = queue.stats();
			CHECK(started.metadata_connection_capacity == 3 && started.metadata_job_capacity == 2);
			CHECK(started.metadata_bytes == allocations.bytes && allocations.calls == 1);
			CHECK(started.metadata_bytes == transmissionMetadataBytes(3, 2));
			auto listener = queue.openListener("127.0.0.1", 0, 1).handle;
			auto first = queue.accept(listener).handle;
			auto second = queue.accept(listener).handle;
			CHECK(listener.valid && first.valid && second.valid);
			const auto overflow = queue.accept(listener);
			CHECK(overflow.status == Status::Fatal && !overflow.handle.valid && overflow.cleanup &&
			    overflow.cleanup->released);
			CHECK(native.closes == 1);

			auto a = queue.acquireWrite(first, 8);
			auto b = queue.acquireWrite(second, 8);
			CHECK(a && b && !queue.acquireWrite(first, 1));
			CHECK(allocations.calls == 1);
			a.reset();
			auto replacement = queue.acquireWrite(first, 8);
			CHECK(replacement);
			CHECK(queue.close(first).status == Status::WouldBlock && first.valid);
			replacement.reset();
			CHECK(queue.close(first).released && !first.valid);
			b.reset();
			CHECK(queue.close(second).released);

			// Repeated slot reuse and payload leases must not grow metadata.
			for(unsigned cycle = 0; cycle < 64; ++cycle)
			{
				auto client = queue.accept(listener).handle;
				CHECK(client.valid);
				auto lease = queue.acquireWrite(client, 8);
				CHECK(lease);
				lease.reset();
				CHECK(queue.close(client).released);
			}
			CHECK(allocations.calls == 1 && allocations.releases == 0);
			CHECK(queue.close(listener).released);
		}
		CHECK(allocations.calls == 1 && allocations.releases == 1);
		CHECK(allocations.bytes == allocations.released_bytes);
	}

	void metadataFailureAndStaticStorage()
	{
		CapacityNative native;
		MetadataAllocations allocations;
		TransmissionOptions options;
		options.maximum_blocks = 2;
		options.block_bytes = 16;
		options.maximum_bytes = 32;
		options.reserved_important_blocks = 0;
		options.metadata_allocator = allocations.allocator();
		const auto required = transmissionMetadataBytes(3, 2);
		CHECK(required && required <= 4096);
		CHECK(transmissionMetadataBytes(0, 2) == 0 && transmissionMetadataBytes(3, 0) == 0);
		CHECK(transmissionMetadataBytes(3, 33) == 0);
		CHECK(transmissionMetadataBytes(std::numeric_limits<std::size_t>::max(), 2) == 0);
		{
			QueuedTransport invalid(native, options, 0);
			CHECK(!invalid.valid() && allocations.calls == 0);
		}
		options.metadata_max_bytes = 1;
		{
			QueuedTransport invalid(native, options, 3);
			CHECK(!invalid.valid() && allocations.calls == 0);
		}
		options.metadata_max_bytes = 64 * 1024;
		allocations.fail = true;
		{
			QueuedTransport invalid(native, options, 3);
			CHECK(!invalid.valid() && allocations.calls == 1 && allocations.releases == 0);
		}
		allocations.fail = false;
		allocations.misalign = true;
		{
			QueuedTransport invalid(native, options, 3);
			CHECK(!invalid.valid() && allocations.calls == 2 && allocations.releases == 1);
		}
		auto mismatched_options = options;
		mismatched_options.metadata_allocator.release = nullptr;
		{
			QueuedTransport invalid(native, mismatched_options, 3);
			CHECK(!invalid.valid() && allocations.calls == 2);
		}

		StaticMetadata first_storage, second_storage;
		first_storage.limit = required - 1;
		auto first_options = options, second_options = options;
		first_options.metadata_allocator = first_storage.allocator();
		second_options.metadata_allocator = second_storage.allocator();
		{
			QueuedTransport invalid(native, first_options, 3);
			CHECK(!invalid.valid() && first_storage.calls == 1 && !first_storage.used);
		}
		first_storage.limit = required;
		second_storage.limit = required;
		{
			QueuedTransport first(native, first_options, 3);
			QueuedTransport second(native, second_options, 3);
			CHECK(first.valid() && second.valid());
			CHECK(first_storage.used && second_storage.used);
			CHECK(first_storage.calls == 2 && second_storage.calls == 1);
			auto first_listener = first.openListener("127.0.0.1", 0, 1).handle;
			auto second_listener = second.openListener("127.0.0.1", 0, 1).handle;
			CHECK(first_listener.valid && second_listener.valid);
			auto a = first.acquireWrite(first_listener, 8);
			auto b = first.acquireWrite(first_listener, 8);
			auto c = second.acquireWrite(second_listener, 8);
			CHECK(a && b && c && !first.acquireWrite(first_listener, 1));
			a.reset();
			b.reset();
			c.reset();
			CHECK(first.close(first_listener).released && second.close(second_listener).released);
		}
		CHECK(!first_storage.used && !second_storage.used);
		CHECK(first_storage.releases == 1 && second_storage.releases == 1);
	}

	void metadataRetainedNativeClose()
	{
		CapacityNative native;
		TransmissionOptions options;
		options.maximum_blocks = 2;
		options.reserved_important_blocks = 0;
		QueuedTransport queue(native, options, 3);
		CHECK(queue.valid());
		auto listener = queue.openListener("127.0.0.1", 0, 1).handle;
		auto first = queue.accept(listener).handle;
		auto second = queue.accept(listener).handle;
		CHECK(listener.valid && first.valid && second.valid);
		native.retain_next_close = true;
		const auto retained = queue.close(first);
		CHECK(retained.status == Status::WouldBlock && !retained.released && first.valid);
		CHECK(!queue.acquireWrite(first, 1));
		// A retained native close must not make its metadata slot reusable.
		const auto rejected = queue.accept(listener);
		CHECK(rejected.status == Status::Fatal && rejected.cleanup && rejected.cleanup->released);
		CHECK(queue.close(first).released && !first.valid);
		auto replacement = queue.accept(listener).handle;
		CHECK(replacement.valid && queue.acquireWrite(replacement, 1));
		CHECK(queue.close(replacement).released && queue.close(second).released);
		CHECK(queue.close(listener).released && native.closes == 6);
	}

#if NAWSERVER_TCP_MANAGED
	void managedIngressBoundary()
	{
		class EmptyHandler final : public HttpRequestHandler
		{
		public:
			ExchangeDisposition handle(HttpExchange &) override { return ExchangeDisposition::Drop; }
		};

		for(const auto ingress : std::array<std::uint16_t, 2>{2, 3})
		{
			CapacityNative native;
			TcpOptions options;
			options.http_memory.enabled = true;
			options.http_memory.initial_requests = 0;
			options.http_memory.maximum_requests = 3;
			options.managed.ingress_slots = ingress;
			options.transmission.maximum_blocks = 2;
			options.transmission.reserved_important_blocks = 0;
			QueuedTransport queue(native, options.transmission, 8);
			CHECK(queue.valid());
			NaWServer::detail::BufferPool buffers;
			CHECK(buffers.initialize(options.buffer_memory, BufferGroup::Tcp));
			std::array<ConnectionSlot, 3> slots{};
			std::array<CloseState, 3> closes{};
			ConnectionRegistry registry(queue, &queue, slots.data(), closes.data(), slots.size());
			HttpLimits limits;
			limits.max_connections = 3;
			EmptyHandler handler;
			unsigned dispatched = 0;
			HttpConnectionManager manager(queue, registry, buffers, options, limits, handler, &dispatched,
			    [](void *context, Token, http::wire::RequestParser &parser, ByteView, ResponseTransport,
			        HttpRequestState &) -> ConnectionDisposition
			    {
				    CHECK(parser.head().target == "/third");
				    ++*static_cast<unsigned *>(context);
				    return ConnectionDisposition::Drop;
			    });
			CHECK(manager.valid());
			const auto now = tcpMilliseconds();
			auto listener = queue.openListener("127.0.0.1", 0, 1).handle;
			std::array<Handle, 2> incomplete{};
			for(auto &handle : incomplete)
			{
				handle = queue.accept(listener).handle;
				native.feed(handle, "GET /held HTTP/1.1\r\nHost: test\r\n");
				CHECK(manager.accept(handle, now));
			}
			CHECK(manager.pump(now));
			for(const auto handle : incomplete)
				CHECK(native.unreadBytes(handle) == 0 && !queue.writePressure(handle).failed);

			CHECK(dispatched == 0 && registry.active() == 0 && native.closes == 0);
			auto third = queue.accept(listener).handle;
			constexpr char request[] = "GET /third HTTP/1.1\r\nHost: test\r\n\r\n";
			native.feed(third, request);
			// true also means that an unadmitted socket was successfully closed.
			CHECK(manager.accept(third, now));
			if(ingress == 2)
			{
				CHECK(native.closes == 1 && queue.writePressure(third).failed);
				CHECK(native.unreadBytes(third) == sizeof(request) - 1);
				(void)manager.pump(now);
				CHECK(dispatched == 0);
			}
			else
			{
				CHECK(native.closes == 0 && !queue.writePressure(third).failed);
				until([&]
				{
					(void)manager.pump(now);
					return dispatched == 1;
				});
				CHECK(native.unreadBytes(third) == 0 && native.closes == 1);
			}
			// Neither incomplete header was timed out, dispatched or displaced.
			for(const auto handle : incomplete) CHECK(!queue.writePressure(handle).failed);

			CHECK(manager.valid() && registry.active() == 0 && manager.cleanup());
			CHECK(queue.close(listener).released && native.closes == 4);
			std::printf("MANAGED_INGRESS_BOUNDARY_PASS ingress=%u third_dispatched=%u closed_before_read=%u\n",
			    static_cast<unsigned>(ingress), dispatched, ingress == 2 ? 1u : 0u);
		}
	}

	void metadataManagedSaturation()
	{
		class EmptyHandler final : public HttpRequestHandler
		{
		public:
			ExchangeDisposition handle(HttpExchange &) override { return ExchangeDisposition::Drop; }
		};

		CapacityNative native;
		TcpOptions options;
		options.http_memory.enabled = true;
		options.http_memory.initial_requests = 0;
		options.http_memory.maximum_requests = 2;
		options.managed.ingress_slots = 2;
		options.managed.header_timeout_ms = 30000;
		options.keep_alive.enabled = true;
		options.keep_alive.maximum_idle = 2;
		options.keep_alive.idle_timeout_ms = 30000;
		options.transmission.enabled = true;
		options.transmission.maximum_blocks = 2;
		options.transmission.block_bytes = 256;
		options.transmission.maximum_bytes = 512;
		options.transmission.reserved_important_blocks = 0;
		HttpLimits limits;
		limits.max_connections = 2;
		EmptyHandler handler;
		constexpr std::size_t capacity = 8; // Registry + ingress + idle + listener + accepted.
		{
			TcpHttpBackend backend(native, options);
			CHECK(backend.start(handler, limits) == HttpError::Ok);
			CHECK(backend.transmissionStats().metadata_connection_capacity == capacity);
			CHECK(backend.stop() == HttpError::Ok);
		}

		QueuedTransport queue(native, options.transmission, capacity);
		CHECK(queue.valid() && queue.start());
		NaWServer::detail::BufferPool buffers;
		CHECK(buffers.initialize(options.buffer_memory, BufferGroup::Tcp));
		std::array<ConnectionSlot, 2> slots{};
		std::array<CloseState, 2> closes{};
		ConnectionRegistry registry(queue, &queue, slots.data(), closes.data(), slots.size());
		unsigned dispatched = 0;
		HttpConnectionManager manager(queue, registry, buffers, options, limits, handler, &dispatched,
		    [](void *context, Token, http::wire::RequestParser &, ByteView, ResponseTransport response,
		        HttpRequestState &) -> ConnectionDisposition
		    {
			    ++*static_cast<unsigned *>(context);
			    CHECK(Response(response).text(200, "idle") == SendResult::Ok);
			    return ConnectionDisposition::Http;
		    });
		CHECK(manager.valid());
		auto listener = queue.openListener("127.0.0.1", 0, 1).handle;
		std::array<Handle, 2> idle{};
		for(std::size_t index = 0; index < idle.size(); ++index)
		{
			idle[index] = queue.accept(listener).handle;
			CHECK(idle[index].valid);
			native.feed(idle[index], "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
			CHECK(manager.accept(idle[index], tcpMilliseconds()));
			until([&]
			{
				(void)manager.pump(tcpMilliseconds());
				return dispatched == index + 1 && registry.active() == 0 &&
				    manager.stats().headers.active_blocks == 0;
			});
			CHECK(!queue.writePressure(idle[index]).failed);
		}
		// Keep-alive now retains two native handles after releasing both workspaces.
		std::array<Token, 2> active{};
		for(auto &token : active)
		{
			auto handle = queue.accept(listener).handle;
			CHECK(handle.valid);
			token = registry.claim(handle, Owner::Http);
			CHECK(token.valid() && !handle.valid);
		}
		CHECK(registry.active() == slots.size());
		for(unsigned index = 0; index < options.managed.ingress_slots; ++index)
		{
			const auto handle = queue.accept(listener).handle;
			CHECK(handle.valid && manager.accept(handle, tcpMilliseconds()));
		}
		// With all owners saturated, one final accepted handle must still fit.
		auto orphan = queue.accept(listener).handle;
		CHECK(orphan.valid);
		native.retain_next_close = true;
		CHECK(!manager.accept(orphan, tcpMilliseconds()));
		CHECK(native.retained_value == orphan.value && !queue.writePressure(orphan).failed);
		const auto overflow = queue.accept(listener);
		CHECK(overflow.status == Status::Fatal && overflow.cleanup && overflow.cleanup->released);
		CHECK(manager.cleanup() && queue.writePressure(orphan).failed);
		for(const auto token : active) CHECK(registry.close(token, Owner::Http).released);

		CHECK(registry.active() == 0 && registry.cleanupAfterJoin());
		// Every owner, including the retained orphan, returned its metadata slot.
		std::array<Handle, capacity - 1> recovered{};
		for(auto &handle : recovered)
		{
			handle = queue.accept(listener).handle;
			CHECK(handle.valid);
		}
		for(auto &handle : recovered) CHECK(queue.close(handle).released);

		CHECK(queue.close(listener).released && queue.stop(1000));
	}
#endif

	void metadataAndPayloadPartialFailure()
	{
		CapacityNative native;
		MetadataAllocations metadata, payload;
		payload.fail_call = 2;
		TransmissionOptions options;
		options.maximum_blocks = 2;
		options.initial_blocks = 2;
		options.block_bytes = 16;
		options.maximum_bytes = 32;
		options.reserved_important_blocks = 0;
		options.metadata_allocator = metadata.allocator();
		options.allocator = payload.allocator();
		{
			QueuedTransport invalid(native, options, 3);
			CHECK(!invalid.valid());
			CHECK(metadata.calls == 1 && payload.calls == 2);
		}
		CHECK(metadata.releases == 1 && metadata.released_bytes == metadata.bytes);
		CHECK(payload.releases == 1 && payload.released_bytes == 16);
	}

	void backendMetadataAllocatorSelection()
	{
		class EmptyHandler final : public HttpRequestHandler
		{
		public:
			ExchangeDisposition handle(HttpExchange &) override { return ExchangeDisposition::Drop; }
		};

		// An unmanaged runtime tracks its listener, two sessions and one
		// in-flight/orphan slot, in addition to the two transmission jobs.
		const auto metadata_bytes = transmissionMetadataBytes(4, 2);
		CHECK(metadata_bytes);
		for(bool explicit_override : {false, true})
		{
			CapacityNative native;
			MetadataAllocations common, override_allocator;
			common.watch_bytes = override_allocator.watch_bytes = metadata_bytes;
			TcpOptions options;
			options.buffer_memory.allocator = common.allocator();
			options.transmission.enabled = true;
			options.transmission.maximum_blocks = 2;
			options.transmission.block_bytes = 16;
			options.transmission.maximum_bytes = 32;
			options.transmission.reserved_important_blocks = 0;
			if(explicit_override) options.transmission.metadata_allocator = override_allocator.allocator();

			HttpLimits limits;
			limits.max_connections = 2;
			EmptyHandler handler;
			{
				TcpHttpBackend backend(native, options);
				CHECK(backend.start(handler, limits) == HttpError::Ok);
				const auto stats = backend.transmissionStats();
				CHECK(stats.metadata_bytes == metadata_bytes && stats.metadata_connection_capacity == 4);
				CHECK((explicit_override ? override_allocator.watched_calls : common.watched_calls) == 1);
				if(explicit_override) CHECK(common.watched_calls == 0);

				CHECK(backend.stop() == HttpError::Ok);
			}
			CHECK((explicit_override ? override_allocator.watched_releases : common.watched_releases) == 1);
		}
	}

	TransmissionOptions cooperativeOptions()
	{
		TransmissionOptions options;
		options.execution = TransmissionExecution::Cooperative;
		options.maximum_blocks = 3; options.maximum_bytes = 48; options.block_bytes = 16;
		options.maximum_blocks_per_connection = 3; options.reserved_important_blocks = 0;
		options.step_bytes = 4; options.close_timeout_ms = 5;
		return options;
	}

	void cooperativeQueueOrdering()
	{
		{
			Native native;
			MetadataAllocations metadata;
			auto invalid_options = cooperativeOptions();
			invalid_options.execution = static_cast<TransmissionExecution>(255);
			invalid_options.metadata_allocator = metadata.allocator();
			QueuedTransport invalid(native, invalid_options, 2);
			CHECK(!invalid.valid() && !invalid.start() && metadata.calls == 0);
		}
		Native native;
		native.block_first.store(false);
		QueuedTransport queue(native, cooperativeOptions(), 2);
		CHECK(queue.valid() && queue.start() && !queue.start());
		CHECK(queue.stats().enabled && queue.stats().execution == TransmissionExecution::Cooperative);
		auto first = queue.openListener("127.0.0.1", 0, 1).handle;
		auto second = queue.accept(first).handle;
		auto filling = queue.acquireWrite(first, 3);
		CHECK(filling);
		std::memcpy(filling.data(), "ABC", 3);
		CHECK(queue.writeSome(first, reinterpret_cast<const std::uint8_t *>("DEF"), 3).size == 3);
		CHECK(queue.writeSome(second, reinterpret_cast<const std::uint8_t *>("healthy"), 7).size == 7);
		CHECK(!queue.acquireWrite(second, 1));
		// Cooperative start has no autonomous sender: ordinary sleeping cannot
		// advance either instance or bypass a producer's earlier filling lease.
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
		CHECK(native.wire(1).empty() && native.wire(2).empty());
		until([&]
		{
			(void)queue.wait(nullptr, nullptr, 0, 20);
			return native.wire(2) == "healthy";
		});
		CHECK(native.wire(1).empty() && native.last_wait_timeout.load() <= 1);
		CHECK(filling.commit(3));
		CHECK(queue.flushWrites(first, tcpMilliseconds() + 100));
		CHECK(native.wire(1) == "ABCDEF" && queue.stats().allocated_bytes == 0);
		(void)queue.wait(nullptr, nullptr, 0, 20);
		CHECK(native.last_wait_timeout.load() == 20);
		CHECK(queue.close(second).released && queue.close(first).released && queue.stop(100));
		CHECK(queue.stop(0) && queue.start() && queue.stop(100));
	}

	void cooperativeStallAndCancellation()
	{
		Native native;
		auto options = cooperativeOptions(); options.stall_timeout_ms = 20;
		QueuedTransport queue(native, options, 2);
		CHECK(queue.start());
		auto stalled = queue.openListener("127.0.0.1", 0, 1).handle;
		auto healthy = queue.accept(stalled).handle;
		CHECK(queue.writeSome(stalled, reinterpret_cast<const std::uint8_t *>("blocked"), 7).size == 7);
		CHECK(queue.writeSome(healthy, reinterpret_cast<const std::uint8_t *>("OK"), 2).size == 2);
		until([&]
		{
			(void)queue.wait(nullptr, nullptr, 0, 20);
			return queue.writePressure(stalled).failed;
		});
		CHECK(native.wire(2) == "OK" && queue.stats().last_failure == TransmissionFailure::Stalled);
		CHECK(!queue.flushWrites(stalled, tcpMilliseconds() + 5));
		CHECK(queue.close(stalled).released && queue.close(healthy).released && queue.stop(100));

		CHECK(queue.start());
		healthy = queue.openListener("127.0.0.1", 0, 1).handle;
		auto filling = queue.acquireWrite(healthy, 3);
		CHECK(filling);
		CHECK(queue.writeSome(healthy, reinterpret_cast<const std::uint8_t *>("later"), 5).size == 5);
		CHECK(!queue.flushWrites(healthy, tcpMilliseconds() + 5));
		CHECK(queue.abort(healthy).status == Status::WouldBlock && healthy.valid);
		CHECK(queue.stop(100) && queue.stats().queued_jobs == 0 && queue.stats().filling_jobs == 1);
		CHECK(!filling.commit(3));
		filling.reset();
		CHECK(queue.stats().allocated_bytes == 0 && queue.abort(healthy).released);
	}

	void cooperativeIndependentInstances()
	{
		Native first_native, second_native, dedicated_native;
		first_native.block_first.store(false); second_native.block_first.store(false);
		dedicated_native.block_first.store(false);
		QueuedTransport first(first_native, cooperativeOptions(), 1), second(second_native, cooperativeOptions(), 1);
		auto dedicated_options = cooperativeOptions();
		dedicated_options.execution = TransmissionExecution::DedicatedWorker;
		QueuedTransport dedicated(dedicated_native, dedicated_options, 1);
		CHECK(first.start() && second.start() && dedicated.start());
		CHECK(first.stats().execution == TransmissionExecution::Cooperative &&
		    second.stats().execution == TransmissionExecution::Cooperative &&
		    dedicated.stats().execution == TransmissionExecution::DedicatedWorker);
		auto dedicated_handle = dedicated.openListener("127.0.0.1", 0, 1).handle;
		CHECK(dedicated.writeImmutableSome(dedicated_handle,
		    reinterpret_cast<const std::uint8_t *>("dedicated"), 9).size == 9);
		until([&] { return dedicated_native.wire(1) == "dedicated"; });
		auto first_handle = first.openListener("127.0.0.1", 0, 1).handle;
		auto second_handle = second.openListener("127.0.0.1", 0, 1).handle;
		CHECK(first.writeImmutableSome(first_handle, reinterpret_cast<const std::uint8_t *>("first"), 5).size == 5);
		CHECK(second.writeImmutableSome(second_handle, reinterpret_cast<const std::uint8_t *>("second"), 6).size == 6);
		CHECK(first.flushWrites(first_handle, tcpMilliseconds() + 100));
		CHECK(first_native.wire(1) == "first" && second_native.wire(1).empty());
		CHECK(second.flushWrites(second_handle, tcpMilliseconds() + 100));
		CHECK(second_native.wire(1) == "second");
		CHECK(first.close(first_handle).released && second.close(second_handle).released);
		CHECK(dedicated.close(dedicated_handle).released);
		CHECK(first.stop(100) && second.stop(100) && dedicated.stop(100));
	}

	void cooperativeConcurrentOwners()
	{
		Native native;
		native.block_first.store(false); native.delay_ms.store(1);
		QueuedTransport queue(native, cooperativeOptions(), 2);
		CHECK(queue.start());
		auto first = queue.openListener("127.0.0.1", 0, 1).handle;
		auto second = queue.accept(first).handle;
		std::atomic<bool> failed{false};
		auto produce = [&](Handle handle, const char *text)
		{
			const auto deadline = tcpMilliseconds() + 2500;
			for(unsigned index = 0; index < 30;)
			{
				const auto result = queue.writeSome(handle, reinterpret_cast<const std::uint8_t *>(text), 6);
				if(result.status == IoStatus::Progress && result.size == 6) ++index;
				else if(result.status != IoStatus::WouldBlock) { failed.store(true); break; }

				(void)queue.wait(nullptr, nullptr, 0, 20);
				if(tcpMilliseconds() >= deadline) { failed.store(true); break; }
			}
			if(!queue.flushWrites(handle, deadline)) failed.store(true);
		};
		std::thread first_owner(produce, first, "abcdef"), second_owner(produce, second, "123456");
		first_owner.join(); second_owner.join();
		CHECK(!failed.load() && native.maximum_writers.load() == 1);
		std::string first_expected, second_expected;
		for(unsigned index = 0; index < 30; ++index) { first_expected += "abcdef"; second_expected += "123456"; }
		CHECK(native.wire(1) == first_expected && native.wire(2) == second_expected);
		CHECK(queue.close(first).released && queue.close(second).released && queue.stop(100));

		CHECK(queue.start());
		first = queue.openListener("127.0.0.1", 0, 1).handle;
		native.hold_write.store(true);
		CHECK(queue.writeSome(first, reinterpret_cast<const std::uint8_t *>("stop"), 4).size == 4);
		std::thread servicing([&] { (void)queue.wait(nullptr, nullptr, 0, 20); });
		until([&] { return native.writes_in_progress.load() != 0; });
		CHECK(!queue.stop(0));
		native.hold_write.store(false);
		servicing.join();
		CHECK(queue.stop(100) && queue.stats().queued_jobs == 0 && queue.stats().allocated_bytes == 0);
		CHECK(queue.close(first).released);
	}

	void coherentLastFailure()
	{
		for(const auto reason : {TransmissionFailure::Expired, TransmissionFailure::Stalled,
		    TransmissionFailure::Canceled})
		{
			Native native;
			native.first_error.store(104);
			TransmissionOptions options;
			options.maximum_blocks = 4; options.block_bytes = 16; options.maximum_bytes = 64;
			options.reserved_important_blocks = 0; options.close_timeout_ms = 5;
			options.stall_timeout_ms = reason == TransmissionFailure::Stalled ? 20 : 1000;
			QueuedTransport queue(native, options);
			auto handle = queue.openListener("127.0.0.1", 0, 1).handle;
			auto first = queue.acquireWrite(handle, 1);
			CHECK(first && first.commit(1) && queue.start());
			until([&] { return queue.stats().failed_jobs == 1; });
			CHECK(queue.stats().last_failure == TransmissionFailure::NativeWrite &&
			    queue.stats().last_native_error == 104);
			CHECK(queue.close(handle).released);

			// A later policy failure on another generation must not inherit errno
			// from the earlier socket's native error, even when its descriptor is reused.
			native.first_error.store(0);
			handle = queue.openListener("127.0.0.1", 0, 1).handle;
			WritePolicy policy;
			if(reason == TransmissionFailure::Expired) policy.expires_at_ms = tcpMilliseconds() - 1;

			auto next = queue.acquireWrite(handle, 1, policy);
			CHECK(next && next.commit(1));
			if(reason == TransmissionFailure::Canceled)
			{
				until([&] { return queue.stats().would_block_count != 0; });
				until([&]
				{
					const auto closed = queue.close(handle);
					CHECK(closed.released || closed.status == Status::WouldBlock);
					return closed.released;
				});
			}
			until([&] { return queue.stats().failed_jobs == 2; });
			CHECK(queue.stats().last_failure == reason && queue.stats().last_native_error == 0);
			if(handle.valid) CHECK(queue.close(handle).released);

			CHECK(queue.stop(1000));
		}
	}
}

int main()
{
	staticQueue(); dynamicQueue(); waitingBehindProgressIsNotStall(); allocatorFloor(); coherentLastFailure();
	metadataLimitsAndLifetime(); metadataFailureAndStaticStorage(); metadataAndPayloadPartialFailure();
	metadataRetainedNativeClose();
#if NAWSERVER_TCP_MANAGED
	managedIngressBoundary();
	metadataManagedSaturation();
#endif
	backendMetadataAllocatorSelection();
	cooperativeQueueOrdering(); cooperativeStallAndCancellation(); cooperativeIndependentInstances();
	cooperativeConcurrentOwners();
	std::printf("TCP_TRANSMISSION_PASS checks=%u\n", checks);
}
