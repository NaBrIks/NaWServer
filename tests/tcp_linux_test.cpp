// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <NaWServer/backends/linux_tcp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

	using namespace NaWServer::tcp;
	using Clock = std::chrono::steady_clock;
	std::atomic<unsigned> checks{0};
	volatile sig_atomic_t alarms = 0;

	void check(bool value, int line)
	{
		checks.fetch_add(1);
		if(!value)
		{
			std::printf("TCP_LINUX_FAIL line=%d errno=%d\n", line, errno);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	bool sameSignalPolicy(const struct sigaction &first, const struct sigaction &second)
	{
		if(first.sa_handler != second.sa_handler || first.sa_flags != second.sa_flags)
			return false;

		for(int number = 1; number < NSIG; ++number)
			if(sigismember(&first.sa_mask, number) != sigismember(&second.sa_mask, number))
				return false;

		return true;
	}

	unsigned descriptorCount()
	{
		auto *directory = opendir("/proc/self/fd");
		CHECK(directory != nullptr);
		unsigned count = 0;
		while(const auto *entry = readdir(directory))
			if(entry->d_name[0] != '.')
				++count;

		CHECK(closedir(directory) == 0);
		return count;
	}

	void waitFor(LinuxTransport &transport, Handle handle, bool read, Clock::time_point deadline)
	{
		CHECK(Clock::now() < deadline);
		const Interest interest{handle, read, !read};
		Ready ready;
		const auto result = transport.wait(&interest, &ready, 1, 20);
		CHECK(result.status == WaitStatus::Ready || result.status == WaitStatus::Timeout ||
		    result.status == WaitStatus::Interrupted);
	}

	Handle acceptClient(LinuxTransport &transport, Handle listener)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		for(;;)
		{
			const auto result = transport.accept(listener);
			if(result.status == Status::Ok)
			{
				CHECK(result.handle.valid);
				return result.handle;
			}

			CHECK(result.status == Status::WouldBlock || result.status == Status::Interrupted);
			waitFor(transport, listener, true, deadline);
		}
	}

	int connectClient(std::uint16_t port)
	{
		const int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
		CHECK(client >= 0);
		const timeval timeout{3, 0};
		CHECK(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
		CHECK(setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		CHECK(connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
		return client;
	}

	struct Fixture
	{
		LinuxTransport transport;
		Handle listener;
		Handle connection;
		int client = -1;
		std::uint16_t port;
		Fixture()
		{
			CHECK(transport.start().status == Status::Ok);
			const auto opened = transport.openListener("127.0.0.1", 0, 7);
			CHECK(opened.status == Status::Ok && opened.handle.valid && opened.port);
			listener = opened.handle;
			port = opened.port;
			CHECK(transport.accept(listener).status == Status::WouldBlock);
			client = connectClient(port);
			connection = acceptClient(transport, listener);
			CHECK(transport.ownedHandles() == 2);
		}

		~Fixture()
		{
			if(client >= 0)
				CHECK(::close(client) == 0);

			if(connection.valid)
				CHECK(transport.close(connection).released && !connection.valid);

			CHECK(transport.close(listener).released && !listener.valid);
			CHECK(transport.ownedHandles() == 0 && transport.stop().status == Status::Ok);
		}

	};

	void sendClient(int client, const std::uint8_t *bytes, std::size_t size)
	{
		while(size)
		{
			const auto sent = send(client, bytes, std::min(size, std::size_t{71}), MSG_NOSIGNAL);
			CHECK(sent > 0);
			bytes += sent;
			size -= static_cast<std::size_t>(sent);
		}
	}

	void receiveTransport(LinuxTransport &transport, Handle connection, std::uint8_t *bytes, std::size_t size)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		while(size)
		{
			std::array<std::uint8_t, 129> guarded;
			guarded.fill(0xa5);
			const auto capacity = std::min(size, std::size_t{127});
			const auto result = transport.readSome(connection, guarded.data() + 1, capacity);
			CHECK(guarded.front() == 0xa5 && guarded[capacity + 1] == 0xa5);
			if(result.status == IoStatus::Progress)
			{
				CHECK(result.size > 0 && result.size <= capacity);
				std::memcpy(bytes, guarded.data() + 1, result.size);
				bytes += result.size;
				size -= result.size;
				continue;
			}

			CHECK(result.size == 0 &&
			    (result.status == IoStatus::WouldBlock || result.status == IoStatus::Interrupted));
			waitFor(transport, connection, true, deadline);
		}
	}

	void writeTransport(LinuxTransport &transport, Handle connection, const std::uint8_t *bytes, std::size_t size)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		while(size)
		{
			const auto capacity = std::min(size, std::size_t{17});
			const auto result = transport.writeSome(connection, bytes, capacity);
			if(result.status == IoStatus::Progress)
			{
				CHECK(result.size > 0 && result.size <= capacity);
				bytes += result.size;
				size -= result.size;
				continue;
			}

			CHECK(result.size == 0 &&
			    (result.status == IoStatus::WouldBlock || result.status == IoStatus::Interrupted));
			waitFor(transport, connection, false, deadline);
		}
	}

	void binaryAndHalfClose()
	{
		Fixture fixture;
		CHECK(fixture.transport.stop().status == Status::InvalidState);
		const int listener_flags = fcntl(static_cast<int>(fixture.listener.value), F_GETFL);
		const int listener_descriptor_flags = fcntl(static_cast<int>(fixture.listener.value), F_GETFD);
		const int connection_flags = fcntl(static_cast<int>(fixture.connection.value), F_GETFL);
		const int connection_descriptor_flags = fcntl(static_cast<int>(fixture.connection.value), F_GETFD);
		CHECK(listener_flags >= 0 && (listener_flags & O_NONBLOCK));
		CHECK(listener_descriptor_flags >= 0 && (listener_descriptor_flags & FD_CLOEXEC));
		CHECK(connection_flags >= 0 && (connection_flags & O_NONBLOCK));
		CHECK(connection_descriptor_flags >= 0 && (connection_descriptor_flags & FD_CLOEXEC));
		std::uint8_t empty;
		CHECK(fixture.transport.readSome(fixture.connection, &empty, 1).status == IoStatus::WouldBlock);
		std::array<std::uint8_t, 4096> payload;
		for(std::size_t index = 0; index < payload.size(); ++index)
			payload[index] = static_cast<std::uint8_t>(index * 37);

		sendClient(fixture.client, payload.data(), payload.size());
		CHECK(shutdown(fixture.client, SHUT_WR) == 0);
		std::array<std::uint8_t, 4096> received;
		receiveTransport(fixture.transport, fixture.connection, received.data(), received.size());
		CHECK(payload == received);
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		for(;;)
		{
			const auto result = fixture.transport.readSome(fixture.connection, &empty, 1);
			if(result.status == IoStatus::PeerClosed)
				break;

			CHECK(!result.size && (result.status == IoStatus::WouldBlock || result.status == IoStatus::Interrupted));
			waitFor(fixture.transport, fixture.connection, true, deadline);
		}
		writeTransport(fixture.transport, fixture.connection, payload.data(), payload.size());
		CHECK(fixture.transport.shutdownWrite(fixture.connection).status == Status::Ok);
		std::size_t offset = 0;
		while(offset < received.size())
		{
			const auto count = recv(fixture.client, received.data() + offset, received.size() - offset, 0);
			CHECK(count > 0);
			offset += static_cast<std::size_t>(count);
		}
		CHECK(payload == received && recv(fixture.client, &empty, 1, 0) == 0);
	}

	void invalidAndWait()
	{
		LinuxTransport transport;
		CHECK(transport.ownedHandles() == 0);
		CHECK(transport.openListener("127.0.0.1", 0, 1).status == Status::InvalidState);
		const Interest inactive_interest{{0, true}, true, false};
		Ready inactive_event{true, true, true};
		CHECK(transport.wait(&inactive_interest, &inactive_event, 1, 0).status == WaitStatus::InvalidState);
		CHECK(!inactive_event.readable && !inactive_event.writable && !inactive_event.error);
		Handle invalid;
		CHECK(transport.close(invalid).status == Status::InvalidArgument);
		CHECK(transport.start().status == Status::Ok && transport.start().status == Status::InvalidState);
		CHECK(transport.openListener(nullptr, 0, 1).status == Status::InvalidArgument);
		CHECK(transport.openListener("localhost", 0, 1).status == Status::InvalidArgument);
		CHECK(transport.openListener("127.0.0.1", 0, 0).status == Status::InvalidArgument);
		std::uint8_t byte = 0;
		const Handle too_wide{static_cast<std::uintptr_t>(INT_MAX) + 1, true};
		CHECK(transport.readSome(too_wide, &byte, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.shutdownWrite(too_wide).status == Status::InvalidArgument);
		CHECK(transport.ownedHandles() == 0 && transport.stop().status == Status::Ok);
		Fixture fixture;
		CHECK(fixture.transport.readSome(fixture.connection, nullptr, 1).status == IoStatus::InvalidArgument);
		CHECK(fixture.transport.readSome(fixture.connection, &byte, 0).status == IoStatus::InvalidArgument);
		CHECK(fixture.transport.writeSome(fixture.connection, nullptr, 1).status == IoStatus::InvalidArgument);
		CHECK(fixture.transport.writeSome(fixture.connection, &byte, 0).status == IoStatus::InvalidArgument);
		Ready ready{true, true, true};
		CHECK(fixture.transport.wait(nullptr, &ready, 1, 0).status == WaitStatus::InvalidArgument);
		CHECK(!ready.readable && !ready.writable && !ready.error);
		CHECK(fixture.transport.wait(nullptr, &ready, 0, 0).status == WaitStatus::InvalidArgument);
		Interest interest{fixture.listener, true, false};
		CHECK(fixture.transport.wait(&interest, nullptr, 1, 0).status == WaitStatus::InvalidArgument);
		CHECK(fixture.transport.wait(&interest, &ready, 1, 0).status == WaitStatus::Timeout);
		CHECK(!ready.readable && !ready.writable && !ready.error);
		interest = {fixture.connection, false, false};
		CHECK(fixture.transport.wait(&interest, &ready, 1, 0).status == WaitStatus::InvalidArgument);
		std::array<Interest, 70> duplicates;
		std::array<Ready, 70> events;
		duplicates.fill({fixture.connection, false, true});
		const auto repeated = fixture.transport.wait(duplicates.data(), events.data(), events.size(),
		        std::numeric_limits<std::uint32_t>::max());
		CHECK(repeated.status == WaitStatus::Ready && repeated.ready_count == events.size());
		for(const auto &event : events)
			CHECK(event.writable && !event.readable && !event.error);

		std::array<Handle, 65> listeners;
		std::array<Interest, 65> interests;
		std::array<Ready, 65> results;
		for(std::size_t index = 0; index < listeners.size(); ++index)
		{
			const auto opened = fixture.transport.openListener("127.0.0.1", 0, 1);
			CHECK(opened.status == Status::Ok);
			listeners[index] = opened.handle;
			interests[index] = {opened.handle, true, false};
			results[index] = {true, true, true};
		}
		CHECK(fixture.transport.wait(interests.data(), results.data(), 64, 0).status == WaitStatus::Timeout);
		results.fill({true, true, true});
		CHECK(fixture.transport.wait(interests.data(), results.data(), interests.size(), 0).status ==
		    WaitStatus::InvalidArgument);
		for(std::size_t index = 0; index < listeners.size(); ++index)
		{
			CHECK(!results[index].readable && !results[index].writable && !results[index].error);
			CHECK(fixture.transport.close(listeners[index]).released);
		}
		const int stale = open("/dev/null", O_RDONLY | O_CLOEXEC);
		CHECK(stale >= 0 && ::close(stale) == 0);
		const Handle borrowed_stale{static_cast<std::uintptr_t>(stale), true};
		interest = {borrowed_stale, true, false};
		const auto invalid_native = fixture.transport.wait(&interest, &ready, 1, 0);
		CHECK(invalid_native.status == WaitStatus::Ready && invalid_native.ready_count == 1 && ready.error);
		CHECK(!ready.readable && !ready.writable);
		const auto invalid_read = fixture.transport.readSome(borrowed_stale, &byte, 1);
		CHECK(invalid_read.status == IoStatus::Fatal && invalid_read.native_error == EBADF && !invalid_read.size);
		CHECK(fixture.transport.ownedHandles() == 2);
	}

	void zeroDescriptor()
	{
		const pid_t child = fork();
		CHECK(child >= 0);
		if(child == 0)
		{
			(void)::close(STDIN_FILENO);
			LinuxTransport transport;
			CHECK(transport.start().status == Status::Ok);
			auto listener = transport.openListener("127.0.0.1", 0, 1);
			CHECK(listener.status == Status::Ok && listener.handle.valid && listener.handle.value == 0);
			CHECK(transport.close(listener.handle).released && !listener.handle.valid);
			CHECK(transport.ownedHandles() == 0 && transport.stop().status == Status::Ok);
			std::_Exit(0);
		}

		int status = 0;
		CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}

	void highDescriptors()
	{
		rlimit limit{};
		CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur > 1100);
		std::array<int, 1100> reservations;
		std::size_t count = 0;
		for(;;)
		{
			CHECK(count < reservations.size());
			const int descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
			CHECK(descriptor >= 0);
			reservations[count++] = descriptor;
			if(descriptor >= 1024)
				break;
		}
		{
			Fixture fixture;
			CHECK(fixture.listener.value > 1024 && fixture.connection.value > 1024);
			const std::uint8_t payload = 0xa7;
			sendClient(fixture.client, &payload, 1);
			const Interest interest{fixture.connection, true, false};
			Ready ready;
			const auto result = fixture.transport.wait(&interest, &ready, 1, 3000);
			CHECK(result.status == WaitStatus::Ready && result.ready_count == 1 && ready.readable && !ready.error);
			std::uint8_t actual;
			receiveTransport(fixture.transport, fixture.connection, &actual, 1);
			CHECK(actual == payload);
		}
		while(count)
			CHECK(::close(reservations[--count]) == 0);
	}

	void alarmHandler(int)
	{
		++alarms;
	}

	void interruptedWait()
	{
		Fixture fixture;
		struct sigaction action {};
		struct sigaction previous {};
		action.sa_handler = alarmHandler;
		CHECK(sigemptyset(&action.sa_mask) == 0);
		CHECK(sigaction(SIGALRM, &action, &previous) == 0);
		itimerval timer{};
		itimerval old_timer{};
		timer.it_value.tv_usec = timer.it_interval.tv_usec = 10000;
		CHECK(setitimer(ITIMER_REAL, &timer, &old_timer) == 0);
		const Interest interest{fixture.connection, true, false};
		Ready ready{true, true, true};
		const auto result = fixture.transport.wait(&interest, &ready, 1, 1000);
		CHECK(setitimer(ITIMER_REAL, &old_timer, nullptr) == 0);
		CHECK(sigaction(SIGALRM, &previous, nullptr) == 0);
		CHECK(result.status == WaitStatus::Interrupted && result.native_error == EINTR && result.ready_count == 0);
		CHECK(alarms > 0 && !ready.readable && !ready.writable && !ready.error);
		CHECK(fixture.transport.ownedHandles() == 2);
	}

	void resetAndSignalPolicy()
	{
		const pid_t child = fork();
		CHECK(child >= 0);
		if(child == 0)
		{
			// Establish the isolated child's default signal precondition, then prove
			// transport startup, broken-pipe sends and cleanup leave it unchanged.
			struct sigaction action {};
			action.sa_handler = SIG_DFL;
			CHECK(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGPIPE, &action, nullptr) == 0);
			struct sigaction before {};
			CHECK(sigaction(SIGPIPE, nullptr, &before) == 0);
			{
				Fixture fixture;
				const linger reset{1, 0};
				CHECK(setsockopt(fixture.client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
				CHECK(::close(fixture.client) == 0);
				fixture.client = -1;
				const Interest interest{fixture.connection, true, false};
				Ready ready;
				CHECK(fixture.transport.wait(&interest, &ready, 1, 3000).status == WaitStatus::Ready);
				std::uint8_t byte = 1;
				const auto received = fixture.transport.readSome(fixture.connection, &byte, 1);
				CHECK(received.status == IoStatus::Fatal && received.native_error == ECONNRESET);
				for(unsigned attempt = 0; attempt < 2; ++attempt)
				{
					const auto sent = fixture.transport.writeSome(fixture.connection, &byte, 1);
					CHECK(sent.status == IoStatus::Fatal && sent.native_error == EPIPE);
				}
			}
			struct sigaction after {};
			CHECK(sigaction(SIGPIPE, nullptr, &after) == 0 && sameSignalPolicy(before, after));
			std::_Exit(0);
		}

		int status = 0;
		CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}

	void restartAndBindConflict()
	{
		LinuxTransport transport;
		std::uint16_t port = 0;
		for(unsigned iteration = 0; iteration < 100; ++iteration)
		{
			CHECK(transport.start().status == Status::Ok);
			auto opened = transport.openListener("127.0.0.1", port, 7);
			CHECK(opened.status == Status::Ok && opened.handle.valid && opened.port);
			port = opened.port;
			const auto conflicting = transport.openListener("127.0.0.1", port, 7);
			CHECK(conflicting.status == Status::Fatal && conflicting.native_error == EADDRINUSE);
			CHECK(!conflicting.handle.valid && conflicting.cleanup && conflicting.cleanup->released);
			CHECK(transport.ownedHandles() == 1);
			const int client = connectClient(port);
			auto connection = acceptClient(transport, opened.handle);
			CHECK(transport.shutdownWrite(connection).status == Status::Ok);
			std::uint8_t byte;
			CHECK(recv(client, &byte, 1, 0) == 0);
			CHECK(::close(client) == 0);
			CHECK(transport.close(connection).released && !connection.valid);
			CHECK(transport.close(opened.handle).released && !opened.handle.valid);
			CHECK(transport.ownedHandles() == 0 && transport.stop().status == Status::Ok);
		}
	}

} // namespace

int main()
{
	const auto descriptors_before = descriptorCount();
	struct sigaction before {};
	CHECK(sigaction(SIGPIPE, nullptr, &before) == 0);
	invalidAndWait();
	zeroDescriptor();
	highDescriptors();
	interruptedWait();
	resetAndSignalPolicy();
	binaryAndHalfClose();
	std::array<std::thread, 3> owners;
	for(auto &owner : owners)
		owner = std::thread(binaryAndHalfClose);

	for(auto &owner : owners)
		owner.join();

	restartAndBindConflict();
	struct sigaction after {};
	CHECK(sigaction(SIGPIPE, nullptr, &after) == 0 && sameSignalPolicy(before, after));
	const auto descriptors_after = descriptorCount();
	CHECK(descriptors_after == descriptors_before);
	std::printf("TCP_LINUX_PASS checks=%u transport_bytes=%zu wait_poll_bytes=512 restarts=100 fds_before=%u "
	    "fds_after=%u\n", checks.load(), sizeof(LinuxTransport), descriptors_before, descriptors_after);
}
