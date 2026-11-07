// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "backends/tcp/websocket_connection.h"
#include "support/tcp_scripted_transport.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <string_view>
#include <vector>

namespace
{

	bool fail_nothrow = false;
	int fail_after = -1;
	bool measure_allocations = false;
	std::size_t allocations = 0;
	std::atomic<std::size_t> live_allocations{0};
	unsigned checks = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_WEBSOCKET_CONNECTION_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

} // namespace

void *operator new(std::size_t size)
{
	if(measure_allocations)
		++allocations;

	if(void *storage = std::malloc(size ? size : 1))
	{
		++live_allocations;
		return storage;
	}

	std::abort();
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	if(fail_nothrow || fail_after == 0)
	{
		fail_nothrow = false;
		fail_after = -1;
		return nullptr;
	}

	if(fail_after > 0)
		--fail_after;

	return ::operator new(size);
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept
{
	return ::operator new(size, tag);
}

void operator delete(void *storage) noexcept
{
	if(storage)
		--live_allocations;

	std::free(storage);
}

void operator delete[](void *storage) noexcept
{
	::operator delete(storage);
}

void operator delete(void *storage, std::size_t) noexcept
{
	::operator delete(storage);
}

void operator delete[](void *storage, std::size_t) noexcept
{
	::operator delete(storage);
}

void operator delete(void *storage, const std::nothrow_t &) noexcept
{
	::operator delete(storage);
}

void operator delete[](void *storage, const std::nothrow_t &) noexcept
{
	::operator delete(storage);
}

namespace NaWServer::tcp::detail
{
	struct WebSocketTxPoolTestAccess
	{
		static std::mutex &mutex(WebSocketTxPool &pool)
		{
			return pool.mutex_;
		}

	};
	struct WebSocketConnectionTestAccess
	{
		static std::mutex &mutex(WebSocketConnection &connection)
		{
			return connection.queue_mutex_;
		}

		static bool draining(const WebSocketConnection &connection)
		{
			return connection.close_drain_ == WebSocketConnection::CloseDrain::Reading;
		}

	};
}

namespace
{

	using namespace NaWServer;
	using namespace NaWServer::tcp;
	using namespace NaWServer::tcp::detail;
	using namespace NaWServer::tcp::test;
	static_assert(static_cast<unsigned>(WebSocketSendResult::QueueError) == 6);
	static_assert(static_cast<unsigned>(WebSocketSendResult::Busy) == 7);
	static_assert(static_cast<unsigned>(WebSocketSendResult::GlobalFull) == 8);

#define CHECK(value) check((value), __LINE__)

	Step control(Operation operation, Status status = Status::Ok)
	{
		Step step;
		step.operation = operation;
		step.status = status;
		return step;
	}

	Step input(const std::vector<std::uint8_t> &bytes, std::size_t maximum_chunk = 1)
	{
		Step step;
		step.operation = Operation::Read;
		step.bytes = bytes.data();
		step.size = bytes.size();
		step.maximum_chunk = maximum_chunk;
		return step;
	}

	Step output(const std::uint8_t *bytes, std::size_t size, std::size_t maximum_chunk = 1)
	{
		Step step;
		step.operation = Operation::Write;
		step.bytes = bytes;
		step.size = size;
		step.maximum_chunk = maximum_chunk;
		return step;
	}

	std::vector<std::uint8_t> frame(std::uint8_t opcode, std::initializer_list<std::uint8_t> payload,
	    bool final = true)
	{
		std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>((final ? 0x80 : 0) | opcode),
			                            static_cast<std::uint8_t>(0x80 | payload.size()), 1, 2, 3, 4};
		std::size_t index = 0;
		for(auto value : payload)
			bytes.push_back(static_cast<std::uint8_t>(value ^ (1 + ((index++) & 3))));

		return bytes;
	}

	class TracedEndpoint final : public WebSocketEndpoint
	{
	public:
		const char *target_pointer = nullptr;
		unsigned dispatches = 0;
		unsigned connected = 0;
		unsigned messages = 0;
		std::vector<std::uint8_t> received;
		bool binary = false;

		explicit TracedEndpoint(WebSocketLimits limits)
		  : WebSocketEndpoint("/ws", limits)
		{
			onConnect([this](WebSocketClient *) {
				++connected;
			});
			onMessage([this](WebSocketClient *, const std::uint8_t *bytes, std::size_t size, bool is_binary)
			{
				++messages;
				received.assign(bytes, bytes + size);
				binary = is_binary;
			});
		}

		void handleRequest(Request *request, Response &response) override
		{
			++dispatches;
			CHECK(request->path() == "/ws");
			if(!target_pointer)
				target_pointer = request->path().data();

			CHECK(target_pointer == request->path().data());
			char value[8];
			const auto header = request->readHeader("X-Test", value, sizeof(value));
			CHECK(header.status == HeaderReadStatus::Ok && header.size == 6 && std::strcmp(value, "stable") == 0);
			WebSocketEndpoint::handleRequest(request, response);
		}

	};

	struct Fixture
	{
		ScriptedTransport transport;
		ConnectionSlot slots[2];
		CloseState close_states[2];
		ConnectionRegistry registry{transport, this, slots, close_states, 2};
		WebSocketTxPool local_pool{4, 4096};
		WebSocketTxPool &pool;
		TracedEndpoint endpoint;
		std::array<char, 8192> head_storage;
		std::array<http::wire::HeaderEntry, 32> entries;
		http::wire::RequestParser parser{head_storage.data(), head_storage.size(), entries.data(), entries.size(), {}};
		Handle listener;
		Token token;
		std::shared_ptr<WebSocketConnection> connection;
		WebSocketReservation reservation;

		explicit Fixture(unsigned max_pending = 4, bool open = true, WebSocketTxPool *shared_pool = nullptr,
		    std::size_t max_pending_bytes = 16384, std::uint32_t close_timeout_ms = 50,
		    std::size_t close_drain_bytes = 8192)
		  : pool(shared_pool ? *shared_pool : local_pool), endpoint({4096, max_pending, 4, max_pending_bytes})
		{
			auto opening = control(Operation::OpenListener);
			opening.handle = {41, true};
			opening.port = 31000;
			CHECK(transport.append(opening));
			auto accepting = control(Operation::Accept);
			accepting.handle = {12, true};
			CHECK(transport.append(accepting));
			listener = transport.openListener("127.0.0.1", 0, 2).handle;
			auto handle = transport.accept(listener).handle;
			token = registry.claim(handle, Owner::Http);
			CHECK(token.valid() && !handle.valid);
			constexpr std::string_view head = "GET /ws?q=raw HTTP/1.1\r\nHost: test\r\nX-Test: stable\r\n"
			    "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
			    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
			std::memcpy(head_storage.data(), head.data(), head.size());
			CHECK(parser.advance(head.size()).state == http::wire::ParseStatus::Complete);
			connection = WebSocketConnection::create(registry, pool, token, endpoint, endpoint, parser, {}, 1000,
			        close_timeout_ms, close_drain_bytes);
			CHECK(connection != nullptr);
			reservation = endpoint.prepare(connection);
			CHECK(reservation.valid() && endpoint.connected == 0 && endpoint.count() == 0);
			if(open)
				commit();
		}

		void commit()
		{
			// Simulate the parent reaching full 101 before handing over the owner.
			CHECK(registry.transfer(token, Owner::Http, Owner::WebSocket));
			head_storage.fill('x'); // Original HTTP workspace can be reused immediately.
			CHECK(connection->dispatchOpening(reservation));
			CHECK(reservation.committed() && endpoint.connected == 1 && endpoint.count() == 1);
		}

		~Fixture()
		{
			if(registry.get(token, Owner::WebSocket).valid)
			{
				CHECK(transport.append(control(Operation::Close)));
				CHECK(registry.close(token, Owner::WebSocket).released);
			}
			else if(registry.get(token, Owner::Http).valid)
			{
				reservation.rollback();
				CHECK(transport.append(control(Operation::Close)));
				CHECK(registry.close(token, Owner::Http).released);
			}

			connection->disconnect();
			connection->disconnect();
			CHECK(endpoint.count() == 0);
			CHECK(local_pool.usedBlocks() == 0);
			CHECK(transport.append(control(Operation::Close)));
			CHECK(transport.close(listener).released);
			CHECK(transport.failures() == 0 && transport.remaining() == 0 && transport.owned() == 0);
		}

		void flush(std::uint64_t now = 0)
		{
			for(unsigned step = 0; step < 100; ++step)
			{
				if(WebSocketConnectionTestAccess::draining(*connection))
				{
					CHECK(!connection->wantsWrite(now) && connection->wantsRead());
					Step eof;
					eof.operation = Operation::Read;
					eof.io_status = IoStatus::PeerClosed;
					CHECK(transport.append(eof));
					connection->readReady(now);
					break;
				}

				if(!connection->wantsWrite(now))
					break;

				connection->writeReady(now);
			}

			CHECK(!connection->wantsWrite(now));
		}

	};

	void admissionAndCopy()
	{
		Fixture fixture(1, false);
		CHECK(!fixture.connection->wantsRead());
		CHECK(!fixture.connection->dispatchOpening(fixture.reservation));
		CHECK(fixture.endpoint.connected == 0 && fixture.reservation.valid());
		fixture.commit();
		CHECK(!fixture.connection->dispatchOpening(fixture.reservation));
		std::uint8_t producer[]{'A', 'B', 'C'};
		CHECK(fixture.connection->sendCopy(producer, sizeof(producer), true) == WebSocketSendResult::Queued);
		std::fill(std::begin(producer), std::end(producer), 'X');
		constexpr std::uint8_t header[]{0x82, 3};
		constexpr std::uint8_t payload[]{'A', 'B', 'C'};
		CHECK(fixture.transport.append(output(header, sizeof(header))));
		CHECK(fixture.transport.append(output(payload, sizeof(payload))));
		fixture.connection->writeReady(0);
		CHECK(fixture.pool.usedBlocks() == 1); // Active partial header still owns its block.
		measure_allocations = true;
		fixture.flush();
		measure_allocations = false;
		CHECK(allocations == 0 && fixture.transport.capturedSize() == 5);
		CHECK(fixture.connection->sendCopy(nullptr, 0, false) == WebSocketSendResult::Queued);
		constexpr std::uint8_t empty_header[]{0x81, 0};
		CHECK(fixture.transport.append(output(empty_header, sizeof(empty_header))));
		fixture.flush();
		CHECK(fixture.connection->sendCopy(nullptr, 1, true) == WebSocketSendResult::InvalidArgument);
		CHECK(fixture.connection->sendCopy(payload, 4097, true) == WebSocketSendResult::TooLarge);
		constexpr std::uint8_t invalid_text[]{0xc0, 0x80};
		measure_allocations = true;
		CHECK(fixture.connection->sendCopy(invalid_text, sizeof(invalid_text),
		    false) == WebSocketSendResult::InvalidArgument);
		measure_allocations = false;
		CHECK(allocations == 0 && !fixture.connection->wantsWrite(0));
		constexpr std::uint8_t text_header[]{0x81, 3};
		constexpr std::uint8_t text[]{0xe2, 0x82, 0xac};
		CHECK(fixture.connection->sendCopy(text, sizeof(text), false) == WebSocketSendResult::Queued);
		CHECK(fixture.transport.append(output(text_header, sizeof(text_header))));
		CHECK(fixture.transport.append(output(text, sizeof(text))));
		fixture.flush();
		CHECK(fixture.pool.usedBlocks() == 0);
	}

	void inputAndMetadata()
	{
		Fixture fixture;
		auto prefix = frame(1, {0xe2}, false);
		const auto middle = frame(0, {0x82}, false);
		prefix.insert(prefix.end(), middle.begin(), middle.end());
		fixture.connection->consumePrefix({prefix.data(), prefix.size()});
		CHECK(fixture.endpoint.messages == 0);
		const auto final = frame(0, {0xac});
		CHECK(fixture.transport.append(input(final)));
		for(std::size_t byte = 0; byte < final.size(); ++byte)
			fixture.connection->readReady();

		CHECK(fixture.endpoint.messages == 1 && !fixture.endpoint.binary);
		CHECK(fixture.endpoint.received == std::vector<std::uint8_t>({0xe2, 0x82, 0xac}));
		const auto binary = frame(2, {0, 0xff, 'b'});
		fixture.connection->consumePrefix({binary.data(), binary.size()});
		CHECK(fixture.endpoint.messages == 2 && fixture.endpoint.binary);
		CHECK(fixture.endpoint.dispatches == 3 && fixture.endpoint.received == std::vector<std::uint8_t>({0, 0xff,
		                                                                                                  'b'}));
		Step blocked;
		blocked.operation = Operation::Read;
		blocked.io_status = IoStatus::WouldBlock;
		CHECK(fixture.transport.append(blocked));
		fixture.connection->readReady();
		CHECK(fixture.connection->wantsRead());
		blocked.io_status = IoStatus::PeerClosed;
		CHECK(fixture.transport.append(blocked));
		fixture.connection->readReady();
		CHECK(fixture.connection->finished() && !fixture.connection->wantsRead());
	}

	void controlAndNoInterleave()
	{
		Fixture fixture;
		constexpr std::uint8_t header[]{0x82, 3};
		constexpr std::uint8_t payload[]{'A', 'B', 'C'};
		constexpr std::uint8_t pong_header[]{0x8a, 1};
		constexpr std::uint8_t pong_payload[]{'p'};
		CHECK(fixture.connection->sendCopy(payload, sizeof(payload), true) == WebSocketSendResult::Queued);
		CHECK(fixture.transport.append(output(header, sizeof(header))));
		CHECK(fixture.transport.append(output(payload, sizeof(payload))));
		CHECK(fixture.transport.append(output(pong_header, sizeof(pong_header))));
		CHECK(fixture.transport.append(output(pong_payload, sizeof(pong_payload))));
		fixture.connection->writeReady(0);
		const auto ping = frame(9, {'p'});
		fixture.connection->consumePrefix({ping.data(), ping.size()});
		fixture.flush();
		constexpr std::uint8_t expected[]{0x82, 3, 'A', 'B', 'C', 0x8a, 1, 'p'};
		CHECK(fixture.transport.capturedSize() == sizeof(expected));
		CHECK(std::memcmp(fixture.transport.capturedBytes(), expected, sizeof(expected)) == 0);
		CHECK(fixture.endpoint.messages == 0);
	}

	void closeAndTimeout()
	{
		{
			Fixture fixture;
			constexpr std::uint8_t payload[]{'a'};
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Queued);
			const auto writes = fixture.transport.calls(Operation::Write);
			CHECK(fixture.connection->wantsWrite(50));
			CHECK(fixture.connection->wantsWrite(1049));
			CHECK(!fixture.connection->wantsWrite(1050) && fixture.connection->finished());
			CHECK(fixture.transport.calls(Operation::Write) == writes);
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Closed);
		}
		{
			Fixture fixture;
			const auto closing = frame(8, {});
			fixture.connection->consumePrefix({closing.data(), closing.size()});
			CHECK(!fixture.connection->wantsRead());
			constexpr std::uint8_t header[]{0x88, 0};
			CHECK(fixture.transport.append(output(header, sizeof(header))));
			fixture.flush();
			CHECK(fixture.connection->finished());
			CHECK(fixture.transport.capturedSize() == 2); // No internal-only 1005 on wire.
		}
		{
			Fixture fixture;
			const auto invalid = frame(1, {0xc0, 0x80});
			fixture.connection->consumePrefix({invalid.data(), invalid.size()});
			constexpr std::uint8_t header[]{0x88, 2};
			constexpr std::uint8_t code[]{3, 0xef};
			CHECK(fixture.transport.append(output(header, sizeof(header))));
			CHECK(fixture.transport.append(output(code, sizeof(code))));
			fixture.flush();
			CHECK(fixture.connection->finished() && fixture.endpoint.messages == 0);
		}
		{
			Fixture fixture;
			constexpr std::uint8_t payload[]{'a'};
			constexpr std::uint8_t header[]{0x82, 1};
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Queued);
			CHECK(fixture.transport.append(output(header, sizeof(header), 2)));
			fixture.connection->writeReady(50);
			const auto writes = fixture.transport.calls(Operation::Write);
			CHECK(fixture.connection->wantsWrite(1049));
			CHECK(!fixture.connection->wantsWrite(1050) && fixture.connection->finished());
			fixture.connection->writeReady(1051);
			CHECK(fixture.transport.calls(Operation::Write) == writes);
		}
	}

	void staleToken()
	{
		Fixture fixture;
		constexpr std::uint8_t payload[]{'a'};
		CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Queued);
		const auto old = fixture.token;
		CHECK(fixture.transport.append(control(Operation::Close)));
		CHECK(fixture.registry.close(old, Owner::WebSocket).released);
		auto accepting = control(Operation::Accept);
		accepting.handle = {12, true};
		CHECK(fixture.transport.append(accepting));
		auto reused = fixture.transport.accept(fixture.listener).handle;
		fixture.token = fixture.registry.claim(reused, Owner::WebSocket);
		CHECK(fixture.token.generation != old.generation);
		const auto writes = fixture.transport.calls(Operation::Write);
		const auto reads = fixture.transport.calls(Operation::Read);
		fixture.connection->writeReady(0);
		fixture.connection->readReady();
		CHECK(fixture.connection->finished());
		CHECK(fixture.transport.calls(Operation::Write) == writes && fixture.transport.calls(Operation::Read) == reads);
		CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Closed);
	}

	void protocolCodesAndWriteFailure()
	{
		for(std::uint16_t code : std::initializer_list<std::uint16_t>{1002, 1009})
		{
			Fixture fixture;
			const std::vector<std::uint8_t> invalid = code == 1002 ? std::vector<std::uint8_t>{0x81, 0} :
			std::vector<std::uint8_t>{0x82, 0xfe, 0x10, 1};
			fixture.connection->consumePrefix({invalid.data(), invalid.size()});
			constexpr std::uint8_t header[]{0x88, 2};
			const std::uint8_t payload[]{static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code)};
			CHECK(fixture.transport.append(output(header, sizeof(header))));
			CHECK(fixture.transport.append(output(payload, sizeof(payload))));
			fixture.flush();
			CHECK(fixture.connection->finished() && fixture.endpoint.messages == 0);
		}
		{
			Fixture fixture;
			const auto closing = frame(8, {3, 0xe8, 'b'});
			fixture.connection->consumePrefix({closing.data(), closing.size()});
			constexpr std::uint8_t header[]{0x88, 3};
			constexpr std::uint8_t payload[]{3, 0xe8, 'b'};
			CHECK(fixture.transport.append(output(header, sizeof(header))));
			CHECK(fixture.transport.append(output(payload, sizeof(payload))));
			fixture.flush();
			CHECK(fixture.connection->finished());
		}
		{
			Fixture fixture;
			constexpr std::uint8_t payload[]{'a'};
			constexpr std::uint8_t header[]{0x82, 1};
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Queued);
			CHECK(fixture.transport.append(output(header, sizeof(header), 2)));
			fixture.connection->writeReady(0);
			Step stalled;
			stalled.operation = Operation::Write;
			stalled.io_status = IoStatus::WouldBlock;
			CHECK(fixture.transport.append(stalled));
			fixture.connection->writeReady(1);
			CHECK(fixture.connection->wantsWrite(2));
			stalled.io_status = IoStatus::Interrupted;
			CHECK(fixture.transport.append(stalled));
			fixture.connection->writeReady(2);
			CHECK(!fixture.connection->finished());
			stalled.io_status = IoStatus::Fatal;
			stalled.native_error = 123;
			CHECK(fixture.transport.append(stalled));
			fixture.connection->writeReady(3);
			CHECK(fixture.connection->finished());
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Closed);
		}
		{
			Fixture fixture;
			const auto ping = frame(9, {'p'});
			for(unsigned index = 0; index < 4; ++index)
				fixture.connection->consumePrefix({ping.data(), ping.size()});

			constexpr std::uint8_t header[]{0x88, 2};
			constexpr std::uint8_t payload[]{3, 0xf3};
			CHECK(fixture.transport.append(output(header, sizeof(header))));
			CHECK(fixture.transport.append(output(payload, sizeof(payload))));
			fixture.flush();
			CHECK(fixture.connection->finished());
		}
		{
			Fixture fixture(1, false);
			fail_nothrow = true;
			const auto failed = WebSocketConnection::create(fixture.registry, fixture.pool, fixture.token,
			        fixture.endpoint,
			        fixture.endpoint, fixture.parser, {}, 1000);
			CHECK(!failed && fixture.endpoint.connected == 0 && fixture.registry.active() == 1);
		}
	}

	template <class Function> void whileLocked(std::mutex &target, Function operation)
	{
		std::mutex state_mutex;
		std::condition_variable condition;
		bool locked = false;
		bool release = false;
		std::thread holder([&]
		    {
		    std::lock_guard<std::mutex> target_lock(target);
		    std::unique_lock<std::mutex> lock(state_mutex);
		    locked = true;
		    condition.notify_one();
		    condition.wait(lock, [&] {
		    return release;
				});
			});
		{
			std::unique_lock<std::mutex> lock(state_mutex);
			CHECK(condition.wait_for(lock, std::chrono::seconds(3), [&] {
				return locked;
			}));
		}
		operation();
		{
			std::lock_guard<std::mutex> lock(state_mutex);
			release = true;
		}
		condition.notify_one();
		holder.join();
	}

	void pressurePolicies()
	{
		WebSocketTxPool pool(4, 4096);
		Fixture first(4, true, &pool);
		Fixture second(4, true, &pool);
		Fixture third(4, true, &pool);
		Fixture fourth(4, true, &pool);
		constexpr std::uint8_t payload[]{'a'};
		for(auto *fixture : {&first, &second, &third, &fourth})
			CHECK(fixture->connection->sendCopy(payload, 1, true) == WebSocketSendResult::Queued);

		CHECK(pool.usedBlocks() == 4);
		CHECK(first.connection->sendCopy(payload, 1, true) == WebSocketSendResult::GlobalFull);
		CHECK(first.connection->wantsRead() && !first.connection->finished());
		constexpr std::uint8_t header[]{0x82, 1};
		CHECK(second.transport.append(output(header, 2, 2)));
		CHECK(second.transport.append(output(payload, 1)));
		second.connection->writeReady(0);
		CHECK(pool.usedBlocks() == 4); // Header completion does not release payload.
		second.connection->writeReady(0);
		CHECK(pool.usedBlocks() == 3);
		CHECK(first.connection->sendCopy(nullptr, 0, true) == WebSocketSendResult::Queued);
		CHECK(pool.usedBlocks() == 4); // Empty frame consumes a bounded descriptor/block.
		for(auto *fixture : {&first, &second, &third, &fourth})
			fixture->connection->disconnect();

		CHECK(pool.usedBlocks() == 0);
		{
			Fixture fixture(2);
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Queued);
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Queued);
			CHECK(fixture.transport.append(output(header, 2)));
			CHECK(fixture.transport.append(output(payload, 1)));
			constexpr std::uint8_t close_header[]{0x88, 2};
			constexpr std::uint8_t close_code[]{3, 0xf5}; // 1013 overload.
			CHECK(fixture.transport.append(output(close_header, 2)));
			CHECK(fixture.transport.append(output(close_code, 2)));
			fixture.connection->writeReady(0); // One header byte; close cannot interleave.
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::QueueFull);
			CHECK(!fixture.connection->wantsRead());
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Closed);
			CHECK(fixture.connection->wantsWrite(0) && fixture.pool.usedBlocks() == 1);
			fixture.flush();
			CHECK(fixture.connection->finished() && fixture.pool.usedBlocks() == 0);
			constexpr std::uint8_t expected[]{0x82, 1, 'a', 0x88, 2, 3, 0xf5};
			CHECK(fixture.transport.capturedSize() == sizeof(expected));
			CHECK(std::memcmp(fixture.transport.capturedBytes(), expected, sizeof(expected)) == 0);
		}
		{
			Fixture fixture;
			whileLocked(WebSocketConnectionTestAccess::mutex(*fixture.connection), [&]
			{
				CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Busy);
			});
			whileLocked(WebSocketTxPoolTestAccess::mutex(fixture.pool), [&]
			{
				CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Busy);
			});
			CHECK(fixture.connection->wantsRead() && fixture.pool.usedBlocks() == 0);
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Queued);
		}
	}

	void allocationAndRollback()
	{
		Fixture fixture(4, false);
		for(int ordinal = 0; ordinal < 5; ++ordinal)
		{
			const auto before = live_allocations.load();
			fail_after = ordinal;
			const auto failed = WebSocketConnection::create(fixture.registry, fixture.pool, fixture.token,
			        fixture.endpoint, fixture.endpoint, fixture.parser, {}, 1000);
			CHECK(!failed && fail_after == -1 && live_allocations == before);
			CHECK(fixture.endpoint.connected == 0 && fixture.pool.usedBlocks() == 0);
		}
		fixture.commit();
		std::array<std::uint8_t, 4096> payload{};
		const auto before = live_allocations.load();
		measure_allocations = true;
		for(unsigned index = 0; index < 4; ++index)
			CHECK(fixture.connection->sendCopy(payload.data(), payload.size(), true) == WebSocketSendResult::Queued);

		CHECK(fixture.pool.usedBlocks() == 4);
		CHECK(fixture.connection->sendCopy(payload.data(), 1, true) == WebSocketSendResult::QueueFull);
		CHECK(allocations == 0 && live_allocations == before);
		measure_allocations = false;
		fixture.connection->disconnect();
		CHECK(fixture.pool.usedBlocks() == 0);
	}

	void concurrentProducers()
	{
		Fixture fixture;
		std::array<std::thread, 4> producers;
		std::atomic<bool> valid{true};
		std::atomic<unsigned> accepted{0};
		for(unsigned id = 0; id < producers.size(); ++id)
		{
			producers[id] = std::thread([&, id]
			{
				std::array<std::uint8_t, 256> source;
				source.fill(static_cast<std::uint8_t>(id));
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
				for(;;)
				{
					const auto result = fixture.connection->sendCopy(source.data(), source.size(), true);
					if(result == WebSocketSendResult::Queued)
					{
						++accepted;
						source.fill(0xff);
						return;
					}

					if(result != WebSocketSendResult::Busy || std::chrono::steady_clock::now() >= deadline)
					{
						valid.store(false);
						return;
					}

					std::this_thread::yield();
				}
			});
		}
		for(auto &producer : producers)
			producer.join();

		CHECK(valid.load() && accepted.load() == 4 && fixture.pool.usedBlocks() == 4);
		constexpr std::uint8_t header[]{0x82, 126, 1, 0};
		for(unsigned index = 0; index < producers.size(); ++index)
		{
			CHECK(fixture.transport.append(output(header, sizeof(header), sizeof(header))));
			CHECK(fixture.transport.append(output(nullptr, 256, 256)));
		}
		allocations = 0;
		measure_allocations = true;
		fixture.flush();
		measure_allocations = false;
		CHECK(allocations == 0 && fixture.pool.usedBlocks() == 0);
		CHECK(fixture.transport.capturedSize() == 4 * 260);
		unsigned seen = 0;
		for(unsigned index = 0; index < 4; ++index)
		{
			const auto *bytes = fixture.transport.capturedBytes() + index * 260 + 4;
			CHECK(bytes[0] < 4 && !(seen & (1u << bytes[0])));
			seen |= 1u << bytes[0];
			for(unsigned byte = 0; byte < 256; ++byte)
				CHECK(bytes[byte] == bytes[0]);
		}
		CHECK(seen == 15);
	}

	void retainedClosedSession()
	{
		std::shared_ptr<WebSocketConnection> retained;
		{
			Fixture fixture;
			constexpr std::uint8_t payload[]{1};
			CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Queued);
			retained = fixture.connection;
		}
		// Registry and pool have ended. Closed producers and destruction must not
		// touch either dependency; disconnect emptied every move-only lease first.
		constexpr std::uint8_t payload[]{1};
		CHECK(retained->sendCopy(payload, 1, true) == WebSocketSendResult::Closed);
		CHECK(retained->finished());
		retained.reset();
	}

	void sendClose(Fixture &fixture, std::uint64_t now)
	{
		const auto closing = frame(8, {3, 0xe8});
		fixture.connection->consumePrefix({closing.data(), closing.size()});
		constexpr std::uint8_t header[]{0x88, 2};
		constexpr std::uint8_t code[]{3, 0xe8};
		CHECK(fixture.transport.append(output(header, sizeof(header), sizeof(header))));
		CHECK(fixture.transport.append(output(code, sizeof(code), sizeof(code))));
		fixture.connection->writeReady(now);
		fixture.connection->writeReady(now);
		CHECK(!fixture.connection->finished() && fixture.connection->wantsRead());
		CHECK(WebSocketConnectionTestAccess::draining(*fixture.connection));
		CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0);
	}

	void incrementalCloseDrain()
	{
		{
			Fixture fixture(4, true, nullptr, 16384, 250, 32);
			sendClose(fixture, 100);
			CHECK(!fixture.connection->wantsWrite(349) && !fixture.connection->finished());
			CHECK(!fixture.connection->wantsWrite(350) && fixture.connection->finished());
		}
		{
			Fixture fixture(4, true, nullptr, 16384, 250, 32);
			sendClose(fixture, 100);
			const std::vector<std::uint8_t> tail(32, 0xa5);
			CHECK(fixture.transport.append(input(tail, tail.size())));
			fixture.connection->readReady(101);
			CHECK(fixture.connection->finished() && fixture.transport.remaining() == 0);
		}
		{
			Fixture fixture;
			sendClose(fixture, 100);
			CHECK(!fixture.connection->wantsWrite(100) && fixture.connection->wantsRead());
			const auto shutdowns = fixture.transport.calls(Operation::ShutdownWrite);
			CHECK(!fixture.connection->wantsWrite(101));
			CHECK(fixture.transport.calls(Operation::ShutdownWrite) == shutdowns);
			const auto tail = frame(2, {1, 2, 3, 4});
			CHECK(fixture.transport.append(input(tail, tail.size())));
			const auto reads = fixture.transport.calls(Operation::Read);
			measure_allocations = true;
			fixture.connection->readReady(102);
			CHECK(fixture.transport.calls(Operation::Read) == reads + 1 && !fixture.connection->finished());
			Step blocked;
			blocked.operation = Operation::Read;
			blocked.io_status = IoStatus::WouldBlock;
			CHECK(fixture.transport.append(blocked));
			fixture.connection->readReady(103);
			blocked.io_status = IoStatus::Interrupted;
			CHECK(fixture.transport.append(blocked));
			fixture.connection->readReady(104);
			CHECK(!fixture.connection->finished() && fixture.endpoint.messages == 0);
			CHECK(fixture.connection->sendCopy(tail.data(), tail.size(), true) == WebSocketSendResult::Closed);
			blocked.io_status = IoStatus::PeerClosed;
			CHECK(fixture.transport.append(blocked));
			fixture.connection->readReady(105);
			measure_allocations = false;
			CHECK(allocations == 0);
			CHECK(fixture.connection->finished() && !fixture.connection->wantsRead());
			CHECK(fixture.transport.calls(Operation::ShutdownWrite) == shutdowns);
		}
		{
			Fixture fixture;
			sendClose(fixture, 200);
			CHECK(!fixture.connection->wantsWrite(249) && fixture.connection->wantsRead());
			const auto reads = fixture.transport.calls(Operation::Read);
			fixture.connection->readReady(250);
			CHECK(fixture.connection->finished() && fixture.transport.calls(Operation::Read) == reads);
		}
		{
			Fixture fixture;
			sendClose(fixture, 300);
			CHECK(!fixture.connection->wantsWrite(300));
			CHECK(!fixture.connection->wantsWrite(349) && !fixture.connection->finished());
			CHECK(!fixture.connection->wantsWrite(350) && fixture.connection->finished());
		}
		{
			Fixture fixture;
			sendClose(fixture, 400);
			const auto shutdowns = fixture.transport.calls(Operation::ShutdownWrite);
			CHECK(!fixture.connection->wantsWrite(450) && fixture.connection->finished());
			CHECK(fixture.transport.calls(Operation::ShutdownWrite) == shutdowns);
		}
		{
			Fixture fixture;
			sendClose(fixture, 500);
			CHECK(!fixture.connection->wantsWrite(500));
			const std::vector<std::uint8_t> flood(8192, 0x81);
			CHECK(fixture.transport.append(input(flood, flood.size())));
			for(unsigned step = 0; step < 32; ++step)
			{
				const auto reads = fixture.transport.calls(Operation::Read);
				fixture.connection->readReady(501);
				CHECK(fixture.transport.calls(Operation::Read) == reads + 1);
				CHECK(fixture.connection->finished() == (step == 31));
			}
			const auto reads = fixture.transport.calls(Operation::Read);
			fixture.connection->readReady(502);
			CHECK(fixture.transport.calls(Operation::Read) == reads && fixture.endpoint.messages == 0);
		}
		{
			Fixture fixture;
			sendClose(fixture, 550);
			// A 4097-byte masked frame rejected after its extended length leaves
			// 4101 unread bytes. The old 4096-byte discard left five and forced RST.
			const std::vector<std::uint8_t> tail(4101, 0xa5);
			CHECK(fixture.transport.append(input(tail, tail.size())));
			for(unsigned step = 0; step < 17; ++step)
				fixture.connection->readReady(551);

			CHECK(!fixture.connection->finished() && fixture.transport.remaining() == 0);
			Step eof;
			eof.operation = Operation::Read;
			eof.io_status = IoStatus::PeerClosed;
			CHECK(fixture.transport.append(eof));
			fixture.connection->readReady(552);
			CHECK(fixture.connection->finished() && fixture.endpoint.messages == 0);
		}
		{
			Fixture fixture;
			sendClose(fixture, 600);
			// A peer waiting for the server's TCP EOF sends no FIN of its own.
			// Discard still expires, so the parent can issue its one native close.
			Step blocked;
			blocked.operation = Operation::Read;
			blocked.io_status = IoStatus::WouldBlock;
			CHECK(fixture.transport.append(blocked));
			fixture.connection->readReady(600);
			CHECK(!fixture.connection->wantsWrite(649) && !fixture.connection->finished());
			CHECK(!fixture.connection->wantsWrite(650) && fixture.connection->finished());
			CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0);
		}
		{
			Fixture fixture;
			sendClose(fixture, 700);
			CHECK(!fixture.connection->wantsWrite(700));
			Step failure;
			failure.operation = Operation::Read;
			failure.io_status = IoStatus::Fatal;
			CHECK(fixture.transport.append(failure));
			fixture.connection->readReady(701);
			CHECK(fixture.connection->finished());
		}
		{
			Fixture fixture;
			sendClose(fixture, 800);
			CHECK(!fixture.connection->wantsWrite(800));
			CHECK(fixture.transport.append(control(Operation::Close)));
			CHECK(fixture.registry.close(fixture.token, Owner::WebSocket).released);
			auto accepting = control(Operation::Accept);
			accepting.handle = {12, true};
			CHECK(fixture.transport.append(accepting));
			auto reused = fixture.transport.accept(fixture.listener).handle;
			fixture.token = fixture.registry.claim(reused, Owner::WebSocket);
			const auto reads = fixture.transport.calls(Operation::Read);
			fixture.connection->readReady(801);
			CHECK(fixture.connection->finished() && fixture.transport.calls(Operation::Read) == reads);
		}
		{
			Fixture fixture;
			sendClose(fixture, 900);
			CHECK(!fixture.connection->wantsWrite(900));
			const auto reads = fixture.transport.calls(Operation::Read);
			fixture.connection->abort();
			fixture.connection->readReady(901);
			CHECK(fixture.connection->finished() && fixture.transport.calls(Operation::Read) == reads);
			CHECK(!fixture.connection->wantsWrite(901));
		}
	}

} // namespace

int main()
{
	{
		Fixture fixture(4, true, nullptr, 5);
		const std::uint8_t payload[]{1, 2, 3};
		CHECK(fixture.connection->sendCopy(payload, 3, true) == WebSocketSendResult::Queued);
		CHECK(fixture.connection->sendCopy(payload, 3, true) == WebSocketSendResult::QueueFull);
		CHECK(fixture.pool.usedBlocks() == 1);
		CHECK(fixture.connection->sendCopy(payload, 1, true) == WebSocketSendResult::Closed);
	}
	admissionAndCopy();
	inputAndMetadata();
	controlAndNoInterleave();
	closeAndTimeout();
	staleToken();
	protocolCodesAndWriteFailure();
	pressurePolicies();
	allocationAndRollback();
	concurrentProducers();
	retainedClosedSession();
	incrementalCloseDrain();
	std::printf("TCP_WEBSOCKET_CONNECTION_PASS checks=%u object_bytes=%zu hot_output_allocations=%zu\n",
	    checks, sizeof(WebSocketConnection), allocations);
	return 0;
}
