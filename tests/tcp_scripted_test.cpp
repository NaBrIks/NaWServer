// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/tcp_scripted_transport.h"
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace NaWServer::tcp;
using namespace NaWServer::tcp::test;

namespace
{

	unsigned checks = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_SCRIPTED_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	Step control(Operation operation, Status status = Status::Ok, int native_error = 0)
	{
		Step step;
		step.operation = operation;
		step.status = status;
		step.native_error = native_error;
		return step;
	}

	Step input(const std::uint8_t *bytes, std::size_t size, std::size_t maximum_chunk)
	{
		Step step;
		step.operation = Operation::Read;
		step.bytes = bytes;
		step.size = size;
		step.maximum_chunk = maximum_chunk;
		return step;
	}

	Step output(std::size_t size, std::size_t maximum_chunk)
	{
		Step step;
		step.operation = Operation::Write;
		step.size = size;
		step.maximum_chunk = maximum_chunk;
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

	Step waiting(WaitStatus status, std::uint32_t elapsed_ms = 0, int native_error = 0)
	{
		Step step;
		step.operation = Operation::Wait;
		step.wait_status = status;
		step.elapsed_ms = elapsed_ms;
		step.native_error = native_error;
		return step;
	}

	struct Connections
	{
		Handle listener;
		Handle connection;
	};

	Connections setup(ScriptedTransport &transport)
	{
		auto listener = control(Operation::OpenListener);
		listener.handle = {41, true};
		listener.port = 31234;
		CHECK(transport.append(listener));
		auto connection = control(Operation::Accept);
		connection.handle = {0, true};
		CHECK(transport.append(connection));
		const auto opened = transport.openListener("127.0.0.1", 0, 3);
		CHECK(opened.status == Status::Ok && opened.handle.valid && opened.port == 31234);
		const auto accepted = transport.accept(opened.handle);
		CHECK(accepted.status == Status::Ok && accepted.handle.valid && accepted.handle.value == 0);
		CHECK(transport.owned() == 2);
		return {opened.handle, accepted.handle};
	}

	void finish(ScriptedTransport &transport, Connections &connections)
	{
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.append(control(Operation::Close)));
		const auto closed = transport.close(connections.connection);
		CHECK(closed.status == Status::Ok && closed.released && !connections.connection.valid);
		CHECK(transport.close(connections.listener).released && !connections.listener.valid);
		CHECK(transport.owned() == 0 && transport.released() == 2);
		CHECK(transport.remaining() == 0 && transport.failures() == 0);
	}

	void shortIoCorpus()
	{
		std::array<std::uint8_t, 513> payload{};
		for(std::size_t index = 0; index < payload.size(); ++index)
		{
			payload[index] = static_cast<std::uint8_t>(index);
		}
		for(const auto maximum_chunk : {1u, 2u, 3u, 7u, 127u})
		{
			ScriptedTransport transport;
			auto connections = setup(transport);
			CHECK(transport.append(input(payload.data(), payload.size(), maximum_chunk)));
			CHECK(transport.append(ioFailure(Operation::Read, IoStatus::PeerClosed)));
			auto expected_output = output(payload.size(), maximum_chunk);
			expected_output.bytes = payload.data();
			CHECK(transport.append(expected_output));
			std::size_t offset = 0;
			for(std::size_t attempt = 0; attempt < payload.size() && offset < payload.size(); ++attempt)
			{
				std::array<std::uint8_t, 520> destination;
				destination.fill(0xa5);
				const auto received = transport.readSome(connections.connection,
				        destination.data() + 2, payload.size() - offset);
				CHECK(received.status == IoStatus::Progress && received.size > 0 &&
				    received.size <= maximum_chunk && received.size <= payload.size() - offset);
				CHECK(destination[0] == 0xa5 && destination[1] == 0xa5);
				CHECK(std::memcmp(destination.data() + 2, payload.data() + offset, received.size) == 0);
				CHECK(std::all_of(destination.begin() + 2 + received.size, destination.end(),
				    [](std::uint8_t byte) {
					return byte == 0xa5;
				}));
				offset += received.size;
			}
			CHECK(offset == payload.size());
			std::uint8_t guard = 0xa5;
			const auto eof = transport.readSome(connections.connection, &guard, 1);
			CHECK(eof.status == IoStatus::PeerClosed && eof.size == 0 && guard == 0xa5);

			// A peer's shutdown-write leaves our outgoing half usable.
			auto producer = payload;
			offset = 0;
			for(std::size_t attempt = 0; attempt < payload.size() && offset < payload.size(); ++attempt)
			{
				const auto sent = transport.writeSome(connections.connection,
				        producer.data() + offset, producer.size() - offset);
				CHECK(sent.status == IoStatus::Progress && sent.size > 0 && sent.size <= maximum_chunk);
				offset += sent.size;
			}
			CHECK(offset == payload.size() && transport.capturedSize() == payload.size());
			producer.fill(0xcc);
			CHECK(std::memcmp(transport.capturedBytes(), payload.data(), payload.size()) == 0);
			finish(transport, connections);
		}
	}

	void errorCorpus()
	{
		ScriptedTransport transport;
		auto connections = setup(transport);
		std::array<std::uint8_t, 4> bytes{0xa5, 0xa5, 0xa5, 0xa5};
		for(const auto operation : {Operation::Read, Operation::Write})
		{
			for(const auto status : {IoStatus::WouldBlock, IoStatus::Interrupted, IoStatus::Fatal})
			{
				CHECK(transport.append(ioFailure(operation, status, 1234)));
				const auto result = operation == Operation::Read ?
				    transport.readSome(connections.connection, bytes.data(), bytes.size()) :
				    transport.writeSome(connections.connection, bytes.data(), bytes.size());
				CHECK(result.status == status && result.size == 0 && result.native_error == 1234);
				CHECK((bytes == std::array<std::uint8_t, 4>{0xa5, 0xa5, 0xa5, 0xa5}));
			}
		}
		CHECK(transport.capturedSize() == 0);
		finish(transport, connections);
	}

	void validationCorpus()
	{
		ScriptedTransport transport;
		auto connections = setup(transport);
		const auto calls_before = transport.calls(Operation::Read) + transport.calls(Operation::Write);
		std::uint8_t byte = 0xa5;
		CHECK(transport.readSome(connections.connection, nullptr, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.readSome(connections.connection, &byte, 0).status == IoStatus::InvalidArgument);
		CHECK(transport.writeSome(connections.connection, nullptr, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.writeSome(connections.connection, &byte, 0).status == IoStatus::InvalidArgument);
		CHECK(transport.readSome({}, &byte, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.writeSome({}, &byte, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.readSome({999, true}, &byte, 1).status == IoStatus::InvalidState);
		CHECK(transport.writeSome({999, true}, &byte, 1).status == IoStatus::InvalidState);
		CHECK(transport.readSome(connections.listener, &byte, 1).status == IoStatus::InvalidState);
		CHECK(transport.accept({}).status == Status::InvalidArgument);
		CHECK(transport.accept(connections.connection).status == Status::InvalidState);
		CHECK(transport.shutdownWrite({}).status == Status::InvalidArgument);
		CHECK(transport.shutdownWrite(connections.listener).status == Status::InvalidState);
		CHECK(transport.openListener(nullptr, 1, 1).status == Status::InvalidArgument);
		CHECK(transport.openListener("", 1, 1).status == Status::InvalidArgument);
		CHECK(transport.openListener("127.0.0.1", 1, 0).status == Status::InvalidArgument);
		CHECK(transport.remaining() == 0 && transport.failures() == 0 && byte == 0xa5);
		CHECK(transport.calls(Operation::Read) + transport.calls(Operation::Write) == calls_before);
		CHECK(transport.append(control(Operation::ShutdownWrite)));
		CHECK(transport.shutdownWrite(connections.connection).status == Status::Ok);
		CHECK(transport.writeSome(connections.connection, &byte, 1).status == IoStatus::InvalidState);
		CHECK(transport.shutdownWrite(connections.connection).status == Status::InvalidState);
		finish(transport, connections);
	}

	void waitCorpus()
	{
		ScriptedTransport transport;
		auto connections = setup(transport);
		const Interest interests[]{{connections.connection, true, true}, {connections.connection, true, false}};
		Ready ready[2]{{true, true, true}, {true, true, true}};
		auto step = waiting(WaitStatus::Ready, 2);
		step.readiness[0] = {true, true, false};
		step.readiness[1] = {true, false, false};
		CHECK(transport.append(step));
		const auto result = transport.wait(interests, ready, 2, 5);
		CHECK(result.status == WaitStatus::Ready && result.ready_count == 2 && transport.milliseconds() == 2);
		CHECK(ready[0].readable && ready[0].writable && !ready[0].error);
		CHECK(ready[1].readable && !ready[1].writable && !ready[1].error);
		for(const auto status : {WaitStatus::Timeout, WaitStatus::Interrupted, WaitStatus::Fatal})
		{
			CHECK(transport.append(waiting(status, 1, 4567)));
			std::fill_n(ready, 2, Ready{true, true, true});
			const auto stopped = transport.wait(interests, ready, 2, 3);
			CHECK(stopped.status == status && stopped.ready_count == 0 && stopped.native_error == 4567);
			CHECK(!ready[0].readable && !ready[0].writable && !ready[0].error);
			CHECK(!ready[1].readable && !ready[1].writable && !ready[1].error);
		}
		CHECK(transport.milliseconds() == 5);
		transport.advance(7);
		CHECK(transport.milliseconds() == 12);
		std::fill_n(ready, 2, Ready{true, true, true});
		CHECK(transport.wait(nullptr, ready, 2, 1).status == WaitStatus::InvalidArgument);
		CHECK(!ready[0].readable && !ready[0].writable && !ready[0].error);
		CHECK(!ready[1].readable && !ready[1].writable && !ready[1].error);
		CHECK(transport.wait(interests, nullptr, 2, 1).status == WaitStatus::InvalidArgument);
		CHECK(transport.wait(interests, ready, 0, 1).status == WaitStatus::InvalidArgument);
		const Interest no_interest{connections.connection, false, false};
		ready[0] = {true, true, true};
		CHECK(transport.wait(&no_interest, ready, 1, 1).status == WaitStatus::InvalidArgument);
		CHECK(!ready[0].readable && !ready[0].writable && !ready[0].error);
		const Interest unknown{{999, true}, true, false};
		CHECK(transport.wait(&unknown, ready, 1, 1).status == WaitStatus::InvalidState);
		std::array<Interest, 9> too_many_interests;
		std::array<Ready, 9> too_many_ready;
		too_many_interests.fill(interests[0]);
		too_many_ready.fill({true, true, true});
		CHECK(transport.wait(too_many_interests.data(), too_many_ready.data(), 9, 1).status ==
		    WaitStatus::InvalidArgument);
		for(const auto &entry : too_many_ready)
		{
			CHECK(!entry.readable && !entry.writable && !entry.error);
		}
		CHECK(transport.remaining() == 0 && transport.calls(Operation::Wait) == 4);
		finish(transport, connections);
	}

	void readinessIsHint()
	{
		ScriptedTransport transport;
		auto connections = setup(transport);
		auto step = waiting(WaitStatus::Ready, 1);
		step.readiness[0].readable = true;
		CHECK(transport.append(step));
		CHECK(transport.append(ioFailure(Operation::Read, IoStatus::WouldBlock)));
		const Interest interest{connections.connection, true, false};
		Ready ready;
		CHECK(transport.wait(&interest, &ready, 1, 2).status == WaitStatus::Ready && ready.readable);
		std::uint8_t byte = 0xa5;
		const auto result = transport.readSome(connections.connection, &byte, 1);
		CHECK(result.status == IoStatus::WouldBlock && result.size == 0 && byte == 0xa5);
		finish(transport, connections);
	}

	void closeOwnership()
	{
		ScriptedTransport transport;
		auto connections = setup(transport);
		const auto borrowed_old_connection = connections.connection;
		auto retained = control(Operation::Close, Status::Fatal, 111);
		retained.released = false;
		CHECK(transport.append(retained));
		const auto first = transport.close(connections.connection);
		CHECK(first.status == Status::Fatal && first.native_error == 111 && !first.released);
		CHECK(connections.connection.valid && transport.owned() == 2 && transport.released() == 0);
		auto released = control(Operation::Close, Status::Fatal, 222);
		released.released = true;
		CHECK(transport.append(released));
		const auto second = transport.close(connections.connection);
		CHECK(second.status == Status::Fatal && second.native_error == 222 && second.released);
		CHECK(!connections.connection.valid && transport.owned() == 1 && transport.released() == 1);
		CHECK(transport.close(connections.connection).status == Status::InvalidArgument);
		auto old = borrowed_old_connection;
		CHECK(transport.close(old).status == Status::InvalidState);
		CHECK(transport.calls(Operation::Close) == 2);
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.close(connections.listener).released);
		CHECK(transport.released() == 2 && transport.owned() == 0 && transport.failures() == 0);
	}

	void failuresAreExplicit()
	{
		{
			ScriptedTransport transport;
			auto connections = setup(transport);
			std::uint8_t byte = 0xa5;
			const auto result = transport.readSome(connections.connection, &byte, 1);
			CHECK(result.status == IoStatus::Fatal && result.native_error == ScriptedTransport::script_error);
			CHECK(result.size == 0 && byte == 0xa5 && transport.failures() == 1);
			CHECK(transport.append(output(1, 1)));
			const auto mismatch = transport.readSome(connections.connection, &byte, 1);
			CHECK(mismatch.status == IoStatus::Fatal && transport.remaining() == 1 && transport.failures() == 2);
			CHECK(transport.writeSome(connections.connection, &byte, 1).status == IoStatus::Progress);
			CHECK(transport.append(input(&byte, 1, 0)));
			CHECK(transport.readSome(connections.connection, &byte, 1).status == IoStatus::Fatal);
			CHECK(transport.failures() == 3);
			CHECK(transport.append(output(0, 1)));
			const auto no_progress = transport.writeSome(connections.connection, &byte, 1);
			CHECK(no_progress.status == IoStatus::Fatal && no_progress.size == 0 && transport.failures() == 4);
			CHECK(transport.append(control(Operation::Close)));
			CHECK(transport.append(control(Operation::Close)));
			CHECK(transport.close(connections.connection).released);
			CHECK(transport.close(connections.listener).released);
			CHECK(transport.owned() == 0);
		}
		{
			ScriptedTransport transport;
			for(unsigned index = 0; index < 128; ++index)
			{
				CHECK(transport.append(control(Operation::Close)));
			}
			CHECK(!transport.append(control(Operation::Close)) && transport.failures() == 1);
		}
		{
			ScriptedTransport transport;
			auto connections = setup(transport);
			std::array<std::uint8_t, 16385> bytes{};
			CHECK(transport.append(output(bytes.size(), bytes.size())));
			const auto result = transport.writeSome(connections.connection, bytes.data(), bytes.size());
			CHECK(result.status == IoStatus::Fatal && result.native_error == ScriptedTransport::capacity_error);
			CHECK(result.size == 0 && transport.capturedSize() == 0 && transport.failures() == 1);
		}
	}

	void ownerMismatch()
	{
		ScriptedTransport transport;
		auto connections = setup(transport);
		auto accepted = control(Operation::Accept);
		accepted.handle = {1, true};
		CHECK(transport.append(accepted));
		auto other = transport.accept(connections.listener).handle;
		CHECK(other.valid);
		std::uint8_t byte = 0xa5;
		auto step = output(1, 1);
		step.expected_handle = connections.connection;
		CHECK(transport.append(step));
		const auto wrong = transport.writeSome(other, &byte, 1);
		CHECK(wrong.status == IoStatus::Fatal && wrong.size == 0 && transport.failures() == 1);
		CHECK(transport.capturedSize() == 0 && transport.remaining() == 1);
		CHECK(transport.writeSome(connections.connection, &byte, 1).status == IoStatus::Progress);
		auto receive = input(&byte, 1, 1);
		receive.expected_handle = connections.connection;
		CHECK(transport.append(receive));
		std::uint8_t destination = 0xcc;
		CHECK(transport.readSome(other, &destination, 1).status == IoStatus::Fatal);
		CHECK(destination == 0xcc && transport.remaining() == 1);
		CHECK(transport.readSome(connections.connection, &destination, 1).status == IoStatus::Progress);
		CHECK(destination == byte);
		const Interest expected[]{{connections.connection, true, false}, {other, false, true}};
		const Interest wrong_interests[]{{connections.connection, true, false}, {connections.connection, false, true}};
		auto wait_step = waiting(WaitStatus::Ready);
		wait_step.expected_interests = expected;
		wait_step.expected_interest_count = 2;
		wait_step.readiness[0].readable = true;
		wait_step.readiness[1].writable = true;
		CHECK(transport.append(wait_step));
		Ready ready[2]{{true, true, true}, {true, true, true}};
		CHECK(transport.wait(wrong_interests, ready, 2, 1).status == WaitStatus::Fatal);
		CHECK(transport.remaining() == 1 && !ready[0].readable && !ready[0].writable && !ready[0].error);
		CHECK(!ready[1].readable && !ready[1].writable && !ready[1].error);
		const auto ready_result = transport.wait(expected, ready, 2, 1);
		CHECK(ready_result.status == WaitStatus::Ready && ready_result.ready_count == 2);
		auto close_step = control(Operation::Close);
		close_step.expected_handle = connections.connection;
		CHECK(transport.append(close_step));
		CHECK(transport.close(other).status == Status::Fatal);
		CHECK(other.valid && transport.owned() == 3 && transport.remaining() == 1);
		CHECK(transport.close(connections.connection).released);
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.close(other).released);
		CHECK(transport.close(connections.listener).released);
		CHECK(transport.owned() == 0 && transport.released() == 3 && transport.failures() == 4);
	}

	void failedSetupRetainsOwnership()
	{
		ScriptedTransport transport;
		auto listener_step = control(Operation::OpenListener, Status::Fatal, 555);
		listener_step.handle = {0, true};
		CHECK(transport.append(listener_step));
		auto listener = transport.openListener("127.0.0.1", 12345, 3);
		CHECK(listener.status == Status::Fatal && listener.native_error == 555 && listener.handle.valid);
		CHECK(transport.owned() == 1);
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.close(listener.handle).released && transport.owned() == 0);
		auto connections = setup(transport);
		auto accepted_step = control(Operation::Accept, Status::Fatal, 666);
		accepted_step.handle = {1, true};
		CHECK(transport.append(accepted_step));
		auto accepted = transport.accept(connections.listener);
		CHECK(accepted.status == Status::Fatal && accepted.native_error == 666 && accepted.handle.valid);
		CHECK(transport.owned() == 3);
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.close(accepted.handle).released);
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.close(connections.connection).released);
		CHECK(transport.close(connections.listener).released);
		CHECK(transport.owned() == 0 && transport.released() == 4 && transport.remaining() == 0);
		CHECK(transport.failures() == 0);
	}

	void outputMismatch()
	{
		ScriptedTransport transport;
		auto connections = setup(transport);
		const std::array<std::uint8_t, 3> expected{1, 0, 255};
		const std::array<std::uint8_t, 3> wrong{1, 2, 255};
		auto step = output(expected.size(), 2);
		step.bytes = expected.data();
		CHECK(transport.append(step));
		const auto result = transport.writeSome(connections.connection, wrong.data(), wrong.size());
		CHECK(result.status == IoStatus::Fatal && result.size == 0 && transport.failures() == 1);
		CHECK(transport.capturedSize() == 0 && transport.remaining() == 1);
		CHECK(transport.writeSome(connections.connection, expected.data(), expected.size()).size == 2);
		CHECK(transport.writeSome(connections.connection, expected.data() + 2, 1).size == 1);
		CHECK(transport.capturedSize() == expected.size());
		CHECK(std::memcmp(transport.capturedBytes(), expected.data(), expected.size()) == 0);
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.append(control(Operation::Close)));
		CHECK(transport.close(connections.connection).released);
		CHECK(transport.close(connections.listener).released && transport.owned() == 0);
	}

	void saturatedClock()
	{
		const auto maximum_time = std::numeric_limits<std::uint64_t>::max();
		ScriptedTransport transport(maximum_time - 3);
		auto connections = setup(transport);
		CHECK(transport.append(waiting(WaitStatus::Timeout, 2)));
		const Interest interest{connections.connection, true, false};
		Ready ready;
		CHECK(transport.wait(&interest, &ready, 1, 2).status == WaitStatus::Timeout);
		CHECK(transport.milliseconds() == maximum_time - 1);
		transport.advance(5);
		CHECK(transport.milliseconds() == maximum_time);
		transport.advance(1);
		CHECK(transport.milliseconds() == maximum_time);
		finish(transport, connections);
	}

	void startupFailure()
	{
		ScriptedTransport transport;
		CHECK(transport.append(control(Operation::OpenListener, Status::Fatal, 333)));
		const auto result = transport.openListener("127.0.0.1", 12345, 3);
		CHECK(result.status == Status::Fatal && result.native_error == 333 && !result.handle.valid);
		CHECK(transport.owned() == 0 && transport.remaining() == 0);
		auto connections = setup(transport);
		for(const auto status : {Status::WouldBlock, Status::Interrupted, Status::Fatal})
		{
			CHECK(transport.append(control(Operation::Accept, status, 444)));
			const auto accepted = transport.accept(connections.listener);
			CHECK(accepted.status == status && accepted.native_error == 444 && !accepted.handle.valid);
			CHECK(transport.owned() == 2);
		}
		finish(transport, connections);
	}

} // namespace

int main()
{
	shortIoCorpus();
	errorCorpus();
	validationCorpus();
	waitCorpus();
	readinessIsHint();
	closeOwnership();
	failuresAreExplicit();
	ownerMismatch();
	failedSetupRetainsOwnership();
	outputMismatch();
	saturatedClock();
	startupFailure();
	std::printf("TCP_SCRIPTED checks=%u fixture_bytes=%zu PASS\n", checks, sizeof(ScriptedTransport));
}
