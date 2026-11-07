// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/esp_idf_tcp.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{

	using namespace NaWServer::tcp;
	unsigned checks = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("IDF_TCP_FAIL line=%d errno=%d\n", line, errno);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	enum class Call { Socket, Timeout, NoDelay, Nonblock, Reuse, Bind, Listen, Name, Accept, Read, Write, Poll,
		              Shutdown, Close,
		              Count };
	constexpr auto call_count = static_cast<std::size_t>(Call::Count);
	constexpr std::size_t socket_count = CONFIG_LWIP_MAX_SOCKETS;
	struct Fault
	{
		Call call = Call::Count;
		unsigned occurrence = 0;
		int error = 0;
	};
	struct Socket
	{
		bool live = false;
		bool guarded = false;
		bool nonblocking = false;
		bool no_delay = false;
	};
	struct Fixture
	{
		std::array<unsigned, call_count> calls{};
		std::array<Fault, 8> faults{};
		std::array<Socket, socket_count> sockets{};
		std::array<short, socket_count> poll_events{};
		std::array<pollfd, socket_count> polled{};
		Call previous = Call::Count;
		int previous_descriptor = -1;
		nfds_t polled_count = 0;
		int poll_timeout = 0;
		ssize_t receive_result = -1;
		ssize_t send_result = 1;
		int receive_error = EWOULDBLOCK;
		int send_error = EWOULDBLOCK;
		int socket_error = ECONNRESET;
		std::size_t requested = 0;
		int flags = 0;
	};
	Fixture fixture;

	unsigned calls(Call call)
	{
		return fixture.calls[static_cast<std::size_t>(call)];
	}

	int indexOf(int descriptor)
	{
		return descriptor - LWIP_SOCKET_OFFSET;
	}

	Socket &socketAt(int descriptor)
	{
		CHECK(indexOf(descriptor) >= 0 && indexOf(descriptor) < static_cast<int>(socket_count));
		return fixture.sockets[static_cast<std::size_t>(indexOf(descriptor))];
	}

	bool enter(Call call, int descriptor = -1)
	{
		const auto occurrence = ++fixture.calls[static_cast<std::size_t>(call)];
		fixture.previous = call;
		fixture.previous_descriptor = descriptor;
		for(const auto &fault : fixture.faults)
			if(fault.call == call && fault.occurrence == occurrence)
			{
				errno = fault.error;
				return false;
			}

		return true;
	}

	void fail(Call call, unsigned occurrence, int error, std::size_t slot = 0)
	{
		fixture.faults[slot] = {call, occurrence, error};
	}

	void reset()
	{
		for(const auto &socket : fixture.sockets)
			CHECK(!socket.live);

		fixture = {};
	}

	int allocateSocket()
	{
		for(std::size_t index = 0; index < socket_count; ++index)
			if(!fixture.sockets[index].live)
			{
				fixture.sockets[index] = {true, false, false};
				return static_cast<int>(index) + LWIP_SOCKET_OFFSET;
			}

		errno = EMFILE;
		return -1;
	}

	Handle listener(IdfTransport &transport)
	{
		const auto result = transport.openListener("127.0.0.1", 0, 3);
		CHECK(result.status == Status::Ok && result.handle.valid && result.port == 12345);
		CHECK(!result.cleanup);
		const auto &socket = socketAt(static_cast<int>(result.handle.value));
		CHECK(socket.live && socket.guarded && socket.nonblocking && socket.no_delay);
		return result.handle;
	}

	void release(IdfTransport &transport, Handle &handle)
	{
		const auto result = transport.close(handle);
		CHECK(result.status == Status::Ok && result.released && !handle.valid);
	}

	void lifecycleAndArguments()
	{
		reset();
		IdfTransport transport;
		CHECK(transport.ownedHandles() == 0);
		CHECK(transport.openListener("127.0.0.1", 0, 1).status == Status::InvalidState);
		CHECK(transport.stop().status == Status::Ok);
		CHECK(transport.start().status == Status::Ok);
		CHECK(transport.start().status == Status::InvalidState);
		CHECK(transport.openListener(nullptr, 0, 1).status == Status::InvalidArgument);
		CHECK(transport.openListener("localhost", 0, 1).status == Status::InvalidArgument);
		CHECK(transport.openListener("127.0.0.1", 0, 0).status == Status::InvalidArgument);
		std::uint8_t byte = 0;
		for(const Handle invalid : {Handle{}, Handle{LWIP_SOCKET_OFFSET, false},
		                            Handle{LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS, true},
		                            Handle{std::numeric_limits<std::uintptr_t>::max(), true}})
		{
			CHECK(transport.accept(invalid).status == Status::InvalidArgument);
			CHECK(transport.readSome(invalid, &byte, 1).status == IoStatus::InvalidArgument);
			CHECK(transport.writeSome(invalid, &byte, 1).status == IoStatus::InvalidArgument);
			CHECK(transport.shutdownWrite(invalid).status == Status::InvalidArgument);
			auto copy = invalid;
			CHECK(!transport.close(copy).released);
			CHECK(copy.value == invalid.value && copy.valid == invalid.valid);
		}
		CHECK(calls(Call::Socket) == 0 && calls(Call::Timeout) == 0);
		auto handle = listener(transport);
		CHECK(handle.value == LWIP_SOCKET_OFFSET);
		CHECK(transport.ownedHandles() == 1);
		CHECK(transport.stop().status == Status::InvalidState);
		CHECK(transport.readSome(handle, nullptr, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.readSome(handle, &byte, 0).status == IoStatus::InvalidArgument);
		CHECK(transport.writeSome(handle, nullptr, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.writeSome(handle, &byte, 0).status == IoStatus::InvalidArgument);
		CHECK(calls(Call::Read) == 0 && calls(Call::Write) == 0);
		const auto accepted = transport.accept(handle);
		CHECK(accepted.status == Status::Ok && accepted.handle.valid && !accepted.cleanup);
		auto client = accepted.handle;
		CHECK(socketAt(static_cast<int>(client.value)).nonblocking);
		CHECK(socketAt(static_cast<int>(client.value)).guarded);
		CHECK(socketAt(static_cast<int>(client.value)).no_delay);
		CHECK(transport.ownedHandles() == 2);
		release(transport, client);
		release(transport, handle);
		CHECK(transport.ownedHandles() == 0 && transport.stop().status == Status::Ok);
		CHECK(transport.accept({LWIP_SOCKET_OFFSET, true}).status == Status::InvalidState);
		CHECK(transport.readSome({LWIP_SOCKET_OFFSET, true}, &byte, 1).status == IoStatus::InvalidState);
		CHECK(transport.writeSome({LWIP_SOCKET_OFFSET, true}, &byte, 1).status == IoStatus::InvalidState);
		CHECK(transport.shutdownWrite({LWIP_SOCKET_OFFSET, true}).status == Status::InvalidState);
		auto stopped = Handle{LWIP_SOCKET_OFFSET, true};
		CHECK(transport.close(stopped).status == Status::InvalidState && stopped.valid);
		for(unsigned iteration = 0; iteration < 100; ++iteration)
		{
			CHECK(transport.start().status == Status::Ok);
			auto restarted = listener(transport);
			release(transport, restarted);
			CHECK(transport.stop().status == Status::Ok && transport.ownedHandles() == 0);
		}
	}

	void setupFailures()
	{
		for(const auto call : {Call::Socket, Call::Timeout, Call::NoDelay, Call::Nonblock, Call::Reuse,
		                       Call::Bind, Call::Listen, Call::Name})
		{
			reset();
			IdfTransport transport;
			CHECK(transport.start().status == Status::Ok);
			fail(call, 1, EINVAL);
			const auto result = transport.openListener("127.0.0.1", 0, 1);
			CHECK(result.status == Status::Fatal && result.native_error == EINVAL && !result.handle.valid);
			CHECK(result.cleanup.has_value() == (call != Call::Socket));
			if(result.cleanup) CHECK(result.cleanup->released && result.cleanup->status == Status::Ok);

			CHECK(transport.ownedHandles() == 0 && transport.stop().status == Status::Ok);
		}
		// Failed setup and failed cleanup keep independent original/cleanup errno.
		for(const bool cleanup_guard : {false, true})
		{
			reset();
			IdfTransport transport;
			CHECK(transport.start().status == Status::Ok);
			fail(Call::Bind, 1, EADDRINUSE);
			fail(cleanup_guard ? Call::Timeout : Call::Close, cleanup_guard ? 2 : 1, ENOMEM, 1);
			const auto result = transport.openListener("127.0.0.1", 0, 1);
			CHECK(result.status == Status::Fatal && result.native_error == EADDRINUSE && result.handle.valid);
			CHECK(result.cleanup && result.cleanup->status == Status::WouldBlock &&
			    result.cleanup->native_error == ENOMEM && !result.cleanup->released);
			CHECK(calls(Call::Close) == (cleanup_guard ? 0u : 1u));
			CHECK(transport.ownedHandles() == 1 && transport.stop().status == Status::InvalidState);
			auto handle = result.handle;
			release(transport, handle);
			CHECK(transport.stop().status == Status::Ok);
		}
		for(const bool cleanup_guard : {false, true})
		{
			reset();
			IdfTransport transport;
			CHECK(transport.start().status == Status::Ok);
			auto server = listener(transport);
			fail(Call::Nonblock, calls(Call::Nonblock) + 1, EINVAL);
			fail(cleanup_guard ? Call::Timeout : Call::Close,
			    cleanup_guard ? calls(Call::Timeout) + 2 : 1, EWOULDBLOCK, 1);
			const auto result = transport.accept(server);
			CHECK(result.status == Status::Fatal && result.native_error == EINVAL && result.handle.valid);
			CHECK(result.cleanup && !result.cleanup->released && result.cleanup->status == Status::WouldBlock);
			CHECK(transport.ownedHandles() == 2 && calls(Call::Close) == (cleanup_guard ? 0u : 1u));
			auto accepted = result.handle;
			release(transport, accepted);
			release(transport, server);
			CHECK(transport.stop().status == Status::Ok);
		}
	}

	void guardedCloseAndShutdown()
	{
		for(const int error : {EWOULDBLOCK, EAGAIN, EINPROGRESS, ENOMEM, EINTR, EINVAL})
		{
			for(const bool guard_failure : {false, true})
			{
				reset();
				IdfTransport transport;
				CHECK(transport.start().status == Status::Ok);
				auto handle = listener(transport);
				const auto expected = error == EINTR ? Status::Interrupted :
				    error == EINVAL ? Status::Fatal : Status::WouldBlock;
				fail(guard_failure ? Call::Timeout : Call::Shutdown,
				    guard_failure ? calls(Call::Timeout) + 1 : 1, error);
				const auto shutdown = transport.shutdownWrite(handle);
				CHECK(shutdown.status == expected && shutdown.native_error == error);
				CHECK(calls(Call::Shutdown) == (guard_failure ? 0u : 1u));
				CHECK(handle.valid && transport.ownedHandles() == 1);
				fail(guard_failure ? Call::Timeout : Call::Close,
				    guard_failure ? calls(Call::Timeout) + 1 : 1, error);
				const auto close = transport.close(handle);
				CHECK(close.status == expected && close.native_error == error && !close.released);
				CHECK(calls(Call::Close) == (guard_failure ? 0u : 1u));
				CHECK(handle.valid && transport.ownedHandles() == 1);
				CHECK(transport.stop().status == Status::InvalidState);
				// Explicit fixture recovery after clearing a deterministic injected fault.
				// Production never blindly retries Interrupted/Fatal outcomes.
				release(transport, handle);
				CHECK(transport.ownedHandles() == 0 && transport.stop().status == Status::Ok);
			}
		}
		for(const bool guard_failure : {false, true})
		{
			reset();
			IdfTransport transport;
			CHECK(transport.start().status == Status::Ok);
			auto handle = listener(transport);
			socketAt(static_cast<int>(handle.value)).live = false;
			fail(guard_failure ? Call::Timeout : Call::Close,
			    guard_failure ? calls(Call::Timeout) + 1 : 1, EBADF);
			const auto result = transport.close(handle);
			CHECK(result.status == Status::Fatal && result.native_error == EBADF && result.released);
			CHECK(!handle.valid && transport.ownedHandles() == 0);
			CHECK(calls(Call::Close) == (guard_failure ? 0u : 1u));
			CHECK(transport.stop().status == Status::Ok);
		}
		reset();
		IdfTransport transport;
		CHECK(transport.start().status == Status::Ok);
		auto handle = listener(transport);
		CHECK(transport.shutdownWrite(handle).status == Status::Ok);
		CHECK(calls(Call::Shutdown) == 1 && handle.valid && transport.ownedHandles() == 1);
		release(transport, handle);
		CHECK(calls(Call::Close) == 1 && transport.stop().status == Status::Ok);
	}

	void partialIoAndErrors()
	{
		reset();
		IdfTransport transport;
		CHECK(transport.start().status == Status::Ok);
		auto handle = listener(transport);
		std::array<std::uint8_t, 8> storage{};
		storage.fill(0xa5);
		fixture.receive_result = 3;
		auto read = transport.readSome(handle, storage.data() + 1, 3);
		CHECK(read.status == IoStatus::Progress && read.size == 3);
		CHECK(storage[0] == 0xa5 && storage[1] == 0 && storage[2] == 0x80 && storage[3] == 0xff &&
		    storage[4] == 0xa5 && fixture.requested == 3 && fixture.flags == MSG_DONTWAIT);
		fixture.send_result = 2;
		auto write = transport.writeSome(handle, storage.data(), storage.size());
		CHECK(write.status == IoStatus::Progress && write.size == 2 && fixture.requested == storage.size());
		CHECK(fixture.flags == MSG_DONTWAIT);
		fixture.receive_result = 0;
		CHECK(transport.readSome(handle, storage.data(), storage.size()).status == IoStatus::PeerClosed);
		fixture.send_result = 0;
		CHECK(transport.writeSome(handle, storage.data(), storage.size()).status == IoStatus::Fatal);
		fixture.receive_result = -1;
		fixture.send_result = -1;
		for(const int error : {EWOULDBLOCK, EAGAIN, EINTR, ECONNRESET, ENOMEM, EBADF})
		{
			fixture.receive_error = fixture.send_error = error;
			const auto expected = error == EINTR ? IoStatus::Interrupted :
			    error == EAGAIN || error == EWOULDBLOCK ? IoStatus::WouldBlock : IoStatus::Fatal;
			read = transport.readSome(handle, storage.data(), storage.size());
			write = transport.writeSome(handle, storage.data(), storage.size());
			CHECK(read.status == expected && read.size == 0 && read.native_error == error);
			CHECK(write.status == expected && write.size == 0 && write.native_error == error);
			fail(Call::Accept, calls(Call::Accept) + 1, error);
			const auto accepted = transport.accept(handle);
			CHECK(accepted.status == (error == EINTR ? Status::Interrupted :
			    error == EAGAIN || error == EWOULDBLOCK ? Status::WouldBlock : Status::Fatal));
			CHECK(accepted.native_error == error && !accepted.handle.valid && !accepted.cleanup);
		}
		fixture.receive_result = 0;
		fixture.send_result = 1;
		const auto maximum = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
		CHECK(transport.readSome(handle, storage.data(), std::numeric_limits<std::size_t>::max()).status ==
		    IoStatus::PeerClosed);
		CHECK(fixture.requested == maximum);
		CHECK(transport.writeSome(handle, storage.data(), std::numeric_limits<std::size_t>::max()).status ==
		    IoStatus::Progress);
		CHECK(fixture.requested == maximum);
		release(transport, handle);
		CHECK(transport.stop().status == Status::Ok);
	}

	void waitContract()
	{
		reset();
		IdfTransport transport;
		std::array<Interest, 70> interests{};
		std::array<Ready, 70> ready{};
		interests.fill({{LWIP_SOCKET_OFFSET, true}, true, false});
		ready.fill({true, true, true});
		CHECK(transport.wait(interests.data(), ready.data(), ready.size(), 0).status == WaitStatus::InvalidState);
		CHECK(transport.start().status == Status::Ok);
		CHECK(transport.wait(nullptr, ready.data(), ready.size(), 0).status == WaitStatus::InvalidArgument);
		for(const auto &entry : ready)
			CHECK(!entry.readable && !entry.writable && !entry.error);
		CHECK(transport.wait(interests.data(), nullptr, 1, 0).status == WaitStatus::InvalidArgument);
		CHECK(transport.wait(interests.data(), ready.data(), 0, 0).status == WaitStatus::InvalidArgument);
		CHECK(calls(Call::Poll) == 0);
		for(std::size_t index = 0; index < interests.size(); ++index)
			interests[index] = {{LWIP_SOCKET_OFFSET + index % socket_count, true}, index % 2 == 0, index % 2 != 0};

		fixture.poll_events = {POLLIN, POLLOUT, POLLHUP, POLLERR | POLLNVAL};
		const auto result = transport.wait(interests.data(), ready.data(), ready.size(), UINT_MAX);
		CHECK(result.status == WaitStatus::Ready && result.ready_count == ready.size());
		CHECK(fixture.polled_count == socket_count && fixture.poll_timeout == INT_MAX);
		for(std::size_t index = 0; index < socket_count; ++index)
		{
			CHECK(fixture.polled[index].fd == static_cast<int>(LWIP_SOCKET_OFFSET + index));
			CHECK(fixture.polled[index].events == (index % 2 ? POLLOUT : POLLIN));
		}
		for(std::size_t index = 0; index < ready.size(); ++index)
		{
			const auto expected = index % socket_count;
			CHECK(ready[index].readable == (expected == 0 || expected == 2));
			CHECK(ready[index].writable == (expected == 1));
			CHECK(ready[index].error == (expected == 3));
			CHECK(ready[index].native_error == (expected == 3 ? EBADF : 0));
		}
		interests[1].read = true;
		interests[1].write = false;
		CHECK(transport.wait(interests.data(), ready.data(), ready.size(), 17).status == WaitStatus::Ready);
		CHECK(fixture.polled[1].events == (POLLIN | POLLOUT));
		CHECK(!ready[1].readable && !ready[1].writable && !ready[1].error);
		CHECK(fixture.poll_timeout == 17);
		fixture.poll_events[3] = POLLERR;
		CHECK(transport.wait(interests.data(), ready.data(), ready.size(), 17).status == WaitStatus::Ready);
		CHECK(ready[3].error && ready[3].native_error == 0);
		CHECK(transport.listenerError({LWIP_SOCKET_OFFSET + 3, true}) == ECONNRESET);
		for(const int error : {EINTR, EBADF})
		{
			fail(Call::Poll, calls(Call::Poll) + 1, error);
			const auto failed = transport.wait(interests.data(), ready.data(), ready.size(), 0);
			CHECK(failed.status == (error == EINTR ? WaitStatus::Interrupted : WaitStatus::Fatal));
			CHECK(failed.ready_count == 0 && failed.native_error == error);
			for(const auto &entry : ready)
				CHECK(!entry.readable && !entry.writable && !entry.error);
		}
		fixture.poll_events = {};
		CHECK(transport.wait(interests.data(), ready.data(), ready.size(), 0).status == WaitStatus::Timeout);
		for(const auto &entry : ready)
			CHECK(!entry.readable && !entry.writable && !entry.error);
		for(const bool invalid_handle : {false, true})
		{
			const auto before = calls(Call::Poll);
			if(invalid_handle) interests.back().handle.value = LWIP_SOCKET_OFFSET + socket_count;
			else interests.back().read = interests.back().write = false;

			ready.fill({true, true, true});
			CHECK(transport.wait(interests.data(), ready.data(), ready.size(),
			    1).status == WaitStatus::InvalidArgument);
			CHECK(calls(Call::Poll) == before);
			for(const auto &entry : ready)
				CHECK(!entry.readable && !entry.writable && !entry.error);
			interests.back() = interests.front();
		}
		CHECK(transport.stop().status == Status::Ok);
	}

} // namespace

extern "C"
{

int lwip_socket(int domain, int type, int protocol)
{
	CHECK(domain == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
	return enter(Call::Socket) ? allocateSocket() : -1;
}

int lwip_setsockopt(int descriptor, int level, int option, const void *value, socklen_t size)
{
	if(level == IPPROTO_TCP)
	{
		CHECK(option == TCP_NODELAY && size == sizeof(int) && *static_cast<const int *>(value) == 1);
		if(!enter(Call::NoDelay, descriptor)) return -1;

		socketAt(descriptor).no_delay = true;
		return 0;
	}

	CHECK(level == SOL_SOCKET);
	if(option == SO_SNDTIMEO)
	{
#if LWIP_SO_SNDRCVTIMEO_NONSTANDARD
		CHECK(size == sizeof(int) && *static_cast<const int *>(value) == 50);
#else
		CHECK(size == sizeof(timeval));
		const auto timeout = *static_cast<const timeval *>(value);
		CHECK(timeout.tv_sec == 0 && timeout.tv_usec == 50000);
#endif
		if(!enter(Call::Timeout, descriptor)) return -1;

		socketAt(descriptor).guarded = true;
		return 0;
	}

	CHECK(option == SO_REUSEADDR && size == sizeof(int) && *static_cast<const int *>(value) == 1);
	return enter(Call::Reuse, descriptor) ? 0 : -1;
}

int lwip_getsockopt(int descriptor, int level, int option, void *value, socklen_t *size)
{
	CHECK(indexOf(descriptor) >= 0 && level == SOL_SOCKET && option == SO_ERROR && size && *size == sizeof(int));
	*static_cast<int *>(value) = fixture.socket_error;
	return 0;
}

int lwip_ioctl(int descriptor, long command, void *value)
{
	CHECK(command == FIONBIO && *static_cast<const std::uint32_t *>(value) == 1);
	if(!enter(Call::Nonblock, descriptor)) return -1;

	socketAt(descriptor).nonblocking = true;
	return 0;
}

int lwip_bind(int descriptor, const sockaddr *address, socklen_t size)
{
	CHECK(size == sizeof(sockaddr_in) && address->sa_family == AF_INET);
	CHECK(socketAt(descriptor).guarded && socketAt(descriptor).nonblocking);
	return enter(Call::Bind, descriptor) ? 0 : -1;
}

int lwip_listen(int descriptor, int backlog)
{
	CHECK(backlog > 0);
	return enter(Call::Listen, descriptor) ? 0 : -1;
}

int lwip_getsockname(int descriptor, sockaddr *address, socklen_t *size)
{
	CHECK(*size == sizeof(sockaddr_in));
	if(!enter(Call::Name, descriptor)) return -1;

	reinterpret_cast<sockaddr_in *>(address)->sin_port = htons(12345);
	return 0;
}

int lwip_accept(int descriptor, sockaddr *address, socklen_t *size)
{
	CHECK(!address && !size && socketAt(descriptor).nonblocking);
	return enter(Call::Accept, descriptor) ? allocateSocket() : -1;
}

int lwip_getpeername(int descriptor, sockaddr *address, socklen_t *size)
{
	CHECK(socketAt(descriptor).live && *size == sizeof(sockaddr_in));
	auto &peer = *reinterpret_cast<sockaddr_in *>(address);
	peer.sin_family = AF_INET;
	peer.sin_addr.s_addr = htonl(0x7f000001);
	peer.sin_port = htons(12346);
	return 0;
}

ssize_t lwip_recv(int descriptor, void *destination, size_t capacity, int flags)
{
	enter(Call::Read, descriptor);
	fixture.requested = capacity;
	fixture.flags = flags;
	CHECK(socketAt(descriptor).nonblocking && capacity > 0 && flags == MSG_DONTWAIT);
	if(fixture.receive_result > 0)
	{
		CHECK(fixture.receive_result == 3 && capacity >= 3);
		const std::uint8_t binary[]{0, 0x80, 0xff};
		std::memcpy(destination, binary, sizeof(binary));
	}

	if(fixture.receive_result < 0) errno = fixture.receive_error;

	return fixture.receive_result;
}

ssize_t lwip_send(int descriptor, const void *bytes, size_t size, int flags)
{
	enter(Call::Write, descriptor);
	fixture.requested = size;
	fixture.flags = flags;
	CHECK(socketAt(descriptor).nonblocking && bytes && size > 0 && flags == MSG_DONTWAIT);
	if(fixture.send_result < 0) errno = fixture.send_error;

	return fixture.send_result;
}

int lwip_poll(pollfd *descriptors, nfds_t count, int timeout)
{
	CHECK(count <= socket_count);
	fixture.polled_count = count;
	fixture.poll_timeout = timeout;
	if(!enter(Call::Poll)) return -1;

	int result = 0;
	for(nfds_t index = 0; index < count; ++index)
	{
		fixture.polled[index] = descriptors[index];
		descriptors[index].revents = fixture.poll_events[static_cast<std::size_t>(indexOf(descriptors[index].fd))];
		if(descriptors[index].revents) ++result;
	}
	return result;
}

int lwip_shutdown(int descriptor, int how)
{
	CHECK(how == SHUT_WR && fixture.previous == Call::Timeout && fixture.previous_descriptor == descriptor);
	return enter(Call::Shutdown, descriptor) ? 0 : -1;
}

int lwip_close(int descriptor)
{
	CHECK(fixture.previous == Call::Timeout && fixture.previous_descriptor == descriptor);
	if(!enter(Call::Close, descriptor)) return -1;

	CHECK(socketAt(descriptor).live);
	socketAt(descriptor).live = false;
	return 0;
}

}

int main()
{
	lifecycleAndArguments();
	setupFailures();
	guardedCloseAndShutdown();
	partialIoAndErrors();
	waitContract();
	reset();
	std::printf("IDF_TCP_PASS checks=%u offset=%d timeout_mode=%d transport_bytes=%zu\n",
	    checks, LWIP_SOCKET_OFFSET, LWIP_SO_SNDRCVTIMEO_NONSTANDARD, sizeof(IdfTransport));
	return 0;
}
