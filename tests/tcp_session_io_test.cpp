// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "backends/tcp/session_io.h"
#include "support/tcp_scripted_transport.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace NaWServer;
using namespace NaWServer::tcp;
using namespace NaWServer::tcp::detail;
using namespace NaWServer::tcp::test;

namespace
{

	unsigned checks = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_SESSION_IO_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	std::uint64_t scriptTime(void *context)
	{
		return static_cast<ScriptedTransport *>(context)->milliseconds();
	}

	Step control(Operation operation, Status status = Status::Ok, int native_error = 0)
	{
		Step step;
		step.operation = operation;
		step.status = status;
		step.native_error = native_error;
		return step;
	}

	Step ioFailure(Operation operation, IoStatus status, int native_error = 0)
	{
		Step step;
		step.operation = operation;
		step.io_status = status;
		step.native_error = native_error;
		return step;
	}

	Step input(const std::uint8_t *bytes, std::size_t size, std::size_t maximum_chunk = 127)
	{
		Step step;
		step.operation = Operation::Read;
		step.bytes = bytes;
		step.size = size;
		step.maximum_chunk = maximum_chunk;
		return step;
	}

	Step output(const std::uint8_t *bytes, std::size_t size, std::size_t maximum_chunk = 127)
	{
		auto step = input(bytes, size, maximum_chunk);
		step.operation = Operation::Write;
		return step;
	}

	Step waiting(WaitStatus status, std::uint32_t elapsed, bool writing = false)
	{
		Step step;
		step.operation = Operation::Wait;
		step.wait_status = status;
		step.elapsed_ms = elapsed;
		if(status == WaitStatus::Ready) step.readiness[0] = {!writing, writing, false};

		return step;
	}

	struct Fixture
	{
		ScriptedTransport transport;
		std::atomic<bool> stop{false};
		Handle listener;
		Handle connection;

		explicit Fixture(std::uint64_t initial_time = 0)
		  : transport(initial_time)
		{
			auto step = control(Operation::OpenListener);
			step.handle = {1, true};
			step.port = 12345;
			CHECK(transport.append(step));
			listener = transport.openListener("127.0.0.1", 0, 3).handle;
			step = control(Operation::Accept);
			step.handle = {0, true};
			CHECK(transport.append(step));
			connection = transport.accept(listener).handle;
			CHECK(listener.valid && connection.valid && transport.owned() == 2);
		}

		Clock clock()
		{
			return {&transport, scriptTime};
		}

		void close()
		{
			CHECK(transport.append(control(Operation::Close)));
			CHECK(transport.append(control(Operation::Close)));
			CHECK(transport.close(connection).released);
			CHECK(transport.close(listener).released);
			CHECK(transport.owned() == 0 && transport.failures() == 0 && transport.remaining() == 0);
		}

	};

	// Corrupt result injection does not write beyond the destination. The class
	// under test must reject even an adapter violating the normalized I/O contract.
	class FaultTransport final : public Transport
	{
	public:
		ScriptedTransport &transport;
		bool override_read = false, override_write = false, override_wait = false;
		IoResult io_result;
		WaitResult wait_result;
		Ready readiness;
		std::uint32_t progress_elapsed = 0;
		std::atomic<bool> *stop_after_wait = nullptr;

		explicit FaultTransport(ScriptedTransport &scripted)
		  : transport(scripted)
		{
		}

		ListenerResult openListener(const char *address, std::uint16_t port, int backlog) override
		{
			return transport.openListener(address, port, backlog);
		}

		AcceptResult accept(Handle listener) override
		{
			return transport.accept(listener);
		}

		IoResult readSome(Handle handle, std::uint8_t *destination, std::size_t capacity) override
		{
			const auto result = override_read ? io_result : transport.readSome(handle, destination, capacity);
			if(result.status == IoStatus::Progress) transport.advance(progress_elapsed);

			return result;
		}

		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override
		{
			const auto result = override_write ? io_result : transport.writeSome(handle, bytes, size);
			if(result.status == IoStatus::Progress) transport.advance(progress_elapsed);

			return result;
		}

		WaitResult wait(const Interest *interests, Ready *ready, std::size_t count, std::uint32_t timeout) override
		{
			WaitResult result;
			if(override_wait)
			{
				CHECK(count == 1 && ready);
				*ready = readiness;
				result = wait_result;
			}
			else
			{
				result = transport.wait(interests, ready, count, timeout);
			}

			if(stop_after_wait) stop_after_wait->store(true);

			return result;
		}

		OperationResult shutdownWrite(Handle handle) override
		{
			return transport.shutdownWrite(handle);
		}

		CloseResult close(Handle &handle) override
		{
			return transport.close(handle);
		}

	};

	void partialIoAndInterruption()
	{
		Fixture fixture;
		const std::uint8_t payload[]{0, 255, 1, 0, 2};
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::Interrupted, 100)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Interrupted, 1)));
		CHECK(fixture.transport.append(input(payload, sizeof(payload), 2)));
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 100, 10);
		std::array<std::uint8_t, 7> received;
		received.fill(0xa5);
		std::size_t offset = 0;
		for(unsigned attempt = 0; attempt < 5 && offset < sizeof(payload); ++attempt)
		{
			const auto read = session.read(received.data() + 1 + offset, sizeof(payload) - offset);
			CHECK(read.status == BodyReadStatus::Data && read.size > 0 && read.size <= 2);
			offset += read.size;
		}
		CHECK(offset == sizeof(payload) && received.front() == 0xa5 && received.back() == 0xa5);
		CHECK(std::memcmp(received.data() + 1, payload, sizeof(payload)) == 0);
		CHECK(session.failure() == IoFailure::None && session.nativeError() == 0 && session.now() == 1);
		CHECK(fixture.transport.append(output(payload, 2, 1)));
		CHECK(fixture.transport.append(ioFailure(Operation::Write, IoStatus::WouldBlock, 101)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Ready, 1, true)));
		CHECK(fixture.transport.append(ioFailure(Operation::Write, IoStatus::Interrupted, 102)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Interrupted, 1)));
		CHECK(fixture.transport.append(output(payload + 2, sizeof(payload) - 2, 2)));
		CHECK(session.write(payload, sizeof(payload)));
		CHECK(session.failure() == IoFailure::None && fixture.transport.capturedSize() == sizeof(payload));
		CHECK(std::memcmp(fixture.transport.capturedBytes(), payload, sizeof(payload)) == 0);
		CHECK(session.now() == 3 && session.deadline() == 100);
		fixture.close();
	}

	void stalledReadAndNoNewResponseBudget()
	{
		Fixture fixture;
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Timeout, 3)));
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Timeout, 2)));
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 5, 100, 3);
		std::uint8_t byte = 0xa5;
		const auto result = session.read(&byte, 1);
		CHECK(result.status == BodyReadStatus::Timeout && result.size == 0 && byte == 0xa5);
		CHECK(session.failure() == IoFailure::Timeout && session.now() == 5);
		CHECK(!session.write(&byte, 1));
		CHECK(fixture.transport.calls(Operation::Write) == 0 && session.deadline() == 5);
		fixture.close();
	}

	void noProgressDeadline()
	{
		Fixture fixture;
		for(const auto elapsed : {3u, 2u})
		{
			CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
			CHECK(fixture.transport.append(waiting(WaitStatus::Ready, elapsed)));
		}
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 100, 5, 3);
		std::uint8_t byte = 0xa5;
		CHECK(session.read(&byte, 1).status == BodyReadStatus::Timeout);
		CHECK(session.failure() == IoFailure::Timeout && session.now() == 5);
		CHECK(fixture.transport.calls(Operation::Read) == 2 && fixture.transport.calls(Operation::Wait) == 2);
		CHECK(byte == 0xa5);
		fixture.close();
	}

	void phaseAndProgressLifetime()
	{
		Fixture fixture;
		std::uint8_t byte = 42;
		CHECK(fixture.transport.append(input(&byte, 1)));
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 100, 5);
		CHECK(session.read(&byte, 1).status == BodyReadStatus::Data);
		fixture.transport.advance(6);
		CHECK(session.read(&byte, 1).status == BodyReadStatus::Timeout);
		CHECK(fixture.transport.calls(Operation::Read) == 1);
		// A real switch to response output starts its own stall interval, not total.
		CHECK(fixture.transport.append(output(&byte, 1)));
		CHECK(session.write(&byte, 1) && session.deadline() == 100 && session.now() == 6);
		fixture.close();
	}

	void trickleCannotExtendTotal()
	{
		Fixture fixture;
		std::array<std::uint8_t, 20> payload{};
		CHECK(fixture.transport.append(output(payload.data(), payload.size(), 1)));
		FaultTransport transport(fixture.transport);
		transport.progress_elapsed = 1;
		SessionIo session(transport, fixture.connection, fixture.stop, fixture.clock(), 5, 3);
		CHECK(!session.write(payload.data(), payload.size()));
		CHECK(session.failure() == IoFailure::Timeout && session.now() == 5);
		CHECK(fixture.transport.capturedSize() == 5 && fixture.transport.calls(Operation::Write) == 5);
		CHECK(fixture.transport.remaining() == 1);
		// Consume the intentional fixture remainder outside the ended exchange.
		for(std::size_t offset = 5; offset < payload.size(); ++offset)
		{
			CHECK(fixture.transport.writeSome(fixture.connection, payload.data() + offset, 1).size == 1);
		}
		fixture.close();
	}

	void readUntilAndHalfClose()
	{
		Fixture fixture;
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 100, 10);
		std::uint8_t byte = 42;
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Timeout, 2)));
		CHECK(session.readUntil(&byte, 1, 2).status == BodyReadStatus::Timeout);
		CHECK(session.deadline() == 100 && session.now() == 2);
		CHECK(fixture.transport.append(output(&byte, 1)));
		CHECK(session.write(&byte, 1) && session.failure() == IoFailure::None);
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::PeerClosed)));
		CHECK(session.read(&byte, 1).status == BodyReadStatus::Disconnected);
		CHECK(session.failure() == IoFailure::PeerClosed);
		CHECK(fixture.transport.append(output(&byte, 1)));
		CHECK(session.write(&byte, 1) && session.failure() == IoFailure::None);
		fixture.transport.advance(11);
		CHECK(!session.write(&byte, 1) && session.failure() == IoFailure::Timeout);
		CHECK(fixture.transport.calls(Operation::Write) == 2);
		fixture.close();
	}

	void stopAndValidation()
	{
		Fixture fixture;
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 100, 10);
		std::uint8_t byte = 0xa5;
		CHECK(session.read(nullptr, 1).status == BodyReadStatus::TransportError);
		CHECK(session.read(&byte, 0).status == BodyReadStatus::TransportError);
		CHECK(!session.write(nullptr, 1));
		CHECK(session.write(nullptr, 0));
		CHECK(fixture.transport.calls(Operation::Read) == 0 && fixture.transport.calls(Operation::Write) == 0);
		fixture.stop = true;
		CHECK(session.read(&byte, 1).status == BodyReadStatus::TransportError);
		CHECK(session.failure() == IoFailure::Stopped);
		CHECK(!session.write(&byte, 1) && session.failure() == IoFailure::Stopped);
		session.finish();
		CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0 && fixture.transport.calls(Operation::Read) == 0);
		fixture.stop = false;
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Ready, 1)));
		FaultTransport transport(fixture.transport);
		transport.stop_after_wait = &fixture.stop;
		SessionIo stopping(transport, fixture.connection, fixture.stop, fixture.clock(), 100, 10);
		CHECK(stopping.read(&byte, 1).status == BodyReadStatus::TransportError);
		CHECK(stopping.failure() == IoFailure::Stopped && fixture.transport.calls(Operation::Read) == 1);
		fixture.close();
	}

	void invalidAdapterResults()
	{
		for(const auto result : {IoResult{IoStatus::Progress, 0, 501}, IoResult{IoStatus::Progress, 2, 502},
		                         IoResult{IoStatus::WouldBlock, 1, 503}, IoResult{IoStatus::PeerClosed, 1, 504}})
		{
			Fixture fixture;
			FaultTransport transport(fixture.transport);
			transport.io_result = result;
			transport.override_read = true;
			transport.override_write = true;
			SessionIo session(transport, fixture.connection, fixture.stop, fixture.clock(), 100, 10);
			std::uint8_t byte = 0xa5;
			CHECK(session.read(&byte, 1).status == BodyReadStatus::TransportError);
			CHECK(session.failure() == IoFailure::Transport && session.nativeError() == result.native_error);
			CHECK(!session.write(&byte, 1) && session.failure() == IoFailure::Transport);
			CHECK(session.nativeError() == result.native_error && byte == 0xa5);
			CHECK(fixture.transport.calls(Operation::Wait) == 0);
			fixture.close();
		}
		for(const auto result : {WaitResult{WaitStatus::Ready, 0, 601}, WaitResult{WaitStatus::Ready, 2, 602},
		                         WaitResult{WaitStatus::Timeout, 1, 603}, WaitResult{WaitStatus::Fatal, 0, 604}})
		{
			Fixture fixture;
			CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
			FaultTransport transport(fixture.transport);
			transport.override_wait = true;
			transport.wait_result = result;
			transport.readiness = {true, false, false};
			SessionIo session(transport, fixture.connection, fixture.stop, fixture.clock(), 100, 10);
			std::uint8_t byte = 0xa5;
			CHECK(session.read(&byte, 1).status == BodyReadStatus::TransportError);
			CHECK(session.failure() == IoFailure::Transport && session.nativeError() == result.native_error);
			fixture.close();
		}
	}

	void fatalNativeError()
	{
		Fixture fixture;
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::Fatal, 777)));
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 100, 10);
		std::uint8_t byte = 0xa5;
		CHECK(session.read(&byte, 1).status == BodyReadStatus::TransportError);
		CHECK(session.failure() == IoFailure::Transport && session.nativeError() == 777);
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::Fatal, 888)));
		session.finish();
		CHECK(session.failure() == IoFailure::Transport && session.nativeError() == 777);
		CHECK(fixture.transport.calls(Operation::Read) == 2 && fixture.transport.calls(Operation::Close) == 0);
		CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0);
		fixture.close();
	}

	void finishByteBound()
	{
		Fixture fixture;
		std::array<std::uint8_t, 4096> discarded{};
		CHECK(fixture.transport.append(input(discarded.data(), discarded.size(), 256)));
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 1000, 100);
		session.finish();
		CHECK(fixture.transport.calls(Operation::Read) == 16 && fixture.transport.calls(Operation::Wait) == 0);
		CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0);
		CHECK(fixture.transport.remaining() == 1 && session.failure() == IoFailure::None);
		CHECK(fixture.transport.calls(Operation::Close) == 0 && fixture.transport.owned() == 2);
		std::uint8_t byte = 0;
		CHECK(fixture.transport.readSome(fixture.connection, &byte, 1).status == IoStatus::WouldBlock);
		fixture.close();
	}

	void finishTimeBound()
	{
		for(const auto total_deadline : {1000u, 5u})
		{
			Fixture fixture;
			const auto bound = std::min(total_deadline, 50u);
			for(std::uint32_t time = 0; time < bound;)
			{
				const auto step = std::min(bound - time, 20u);
				CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
				CHECK(fixture.transport.append(waiting(WaitStatus::Timeout, step)));
				time += step;
			}
			SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), total_deadline,
			    100);
			session.finish();
			CHECK(session.now() == bound && session.failure() == IoFailure::None);
			CHECK(fixture.transport.remaining() == 0 && fixture.transport.calls(Operation::Close) == 0);
			CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0);
			fixture.close();
		}
	}

	void configuredCloseGrace()
	{
		Fixture fixture;
		for(unsigned step = 0; step < 4; ++step)
		{
			CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
			CHECK(fixture.transport.append(waiting(WaitStatus::Timeout, 20)));
		}
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::PeerClosed)));
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 1000, 1000,
		    20, 250, 8192);
		session.finish();
		CHECK(session.now() == 80 && session.failure() == IoFailure::None);
		CHECK(fixture.transport.remaining() == 0 && fixture.transport.calls(Operation::Close) == 0);
		CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0);
		fixture.close();
	}

	void finishEofAndInvalidConfiguration()
	{
		Fixture fixture;
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::PeerClosed)));
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), 100, 10);
		session.finish();
		CHECK(session.failure() == IoFailure::None && fixture.transport.calls(Operation::Read) == 1);
		const auto calls = fixture.transport.calls(Operation::ShutdownWrite);
		SessionIo invalid(fixture.transport, {}, fixture.stop, {}, 100, 0, 0);
		std::uint8_t byte = 0;
		CHECK(!invalid.write(&byte, 1) && invalid.failure() == IoFailure::Transport);
		CHECK(invalid.read(&byte, 1).status == BodyReadStatus::TransportError);
		invalid.finish();
		CHECK(fixture.transport.calls(Operation::ShutdownWrite) == calls);
		fixture.close();
	}

	void finishProgressUsesAbsoluteBudget()
	{
		Fixture fixture;
		constexpr std::uint8_t bytes[]{1, 2, 3, 4, 5};
		CHECK(fixture.transport.append(input(bytes, sizeof(bytes), 1)));
		FaultTransport transport(fixture.transport);
		transport.progress_elapsed = 10;
		SessionIo session(transport, fixture.connection, fixture.stop, fixture.clock(), 1000, 100);
		session.finish();
		CHECK(session.now() == 50 && session.failure() == IoFailure::None);
		CHECK(fixture.transport.calls(Operation::Read) == 5 && fixture.transport.calls(Operation::Wait) == 0);
		CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0);
		fixture.close();
	}

	void finishInterruptedAndStopped()
	{
		Fixture fixture;
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::Interrupted)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Interrupted, 7)));
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Ready, 3)));
		FaultTransport transport(fixture.transport);
		transport.stop_after_wait = &fixture.stop;
		SessionIo session(transport, fixture.connection, fixture.stop, fixture.clock(), 1000, 100);
		session.finish();
		CHECK(session.failure() == IoFailure::Stopped && session.now() == 7);
		CHECK(fixture.transport.calls(Operation::Read) == 1 && fixture.transport.calls(Operation::Wait) == 1);
		CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0);
		// The remaining read/wait belong to the fixture, not the stopped owner.
		std::uint8_t byte = 0;
		CHECK(fixture.transport.readSome(fixture.connection, &byte, 1).status == IoStatus::WouldBlock);
		const Interest interest{fixture.connection, true, false};
		Ready ready;
		CHECK(fixture.transport.wait(&interest, &ready, 1, 20).status == WaitStatus::Ready);
		fixture.close();
	}

	void boundedClockArithmetic()
	{
		const auto maximum = std::numeric_limits<std::uint64_t>::max();
		Fixture fixture(maximum - 4);
		CHECK(fixture.transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
		CHECK(fixture.transport.append(waiting(WaitStatus::Timeout, 4)));
		SessionIo session(fixture.transport, fixture.connection, fixture.stop, fixture.clock(), maximum, 100,
		    std::numeric_limits<std::uint32_t>::max());
		std::uint8_t byte = 0xa5;
		CHECK(session.read(&byte, 1).status == BodyReadStatus::Timeout);
		CHECK(session.now() == maximum && session.failure() == IoFailure::Timeout);
		session.finish();
		CHECK(fixture.transport.calls(Operation::ShutdownWrite) == 0);
		fixture.close();
	}

} // namespace

int main()
{
	partialIoAndInterruption();
	stalledReadAndNoNewResponseBudget();
	noProgressDeadline();
	phaseAndProgressLifetime();
	trickleCannotExtendTotal();
	readUntilAndHalfClose();
	stopAndValidation();
	invalidAdapterResults();
	fatalNativeError();
	finishByteBound();
	finishTimeBound();
	configuredCloseGrace();
	finishEofAndInvalidConfiguration();
	finishProgressUsesAbsoluteBudget();
	finishInterruptedAndStopped();
	boundedClockArithmetic();
	std::printf("TCP_SESSION_IO checks=%u session_bytes=%zu PASS\n", checks, sizeof(SessionIo));
}
