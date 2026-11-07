// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <NaWServer/backends/linux_tcp.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <limits>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace NaWServer::tcp
{

	namespace
	{

		bool validNativeHandle(Handle handle)
		{
			return handle.valid && handle.value <= static_cast<std::uintptr_t>(INT_MAX);
		}

		Status operationStatus(int native_error)
		{
			if(native_error == EAGAIN || native_error == EWOULDBLOCK)
				return Status::WouldBlock;

			if(native_error == EINTR)
				return Status::Interrupted;

			return Status::Fatal;
		}

		IoStatus ioStatus(int native_error)
		{
			if(native_error == EAGAIN || native_error == EWOULDBLOCK)
				return IoStatus::WouldBlock;

			if(native_error == EINTR)
				return IoStatus::Interrupted;

			return IoStatus::Fatal;
		}

		bool pendingAcceptError(int native_error)
		{
			// Linux can return a pending connection's network error from accept4.
			// Reject that connection without treating the listener as broken.
			return native_error == ECONNABORTED || native_error == ENETDOWN || native_error == EPROTO ||
			       native_error == ENOPROTOOPT || native_error == EHOSTDOWN || native_error == ENONET ||
			       native_error == EHOSTUNREACH || native_error == EOPNOTSUPP || native_error == ENETUNREACH;
		}

	} // namespace

	LinuxTransport::~LinuxTransport()
	{
		(void)stop();
	}

	OperationResult LinuxTransport::start()
	{
		if(started_.load())
			return {Status::InvalidState, 0};

		started_.store(true);
		return {};
	}

	OperationResult LinuxTransport::stop()
	{
		if(owned_count_.load())
			return {Status::InvalidState, 0};

		started_.store(false);
		return {};
	}

	std::size_t LinuxTransport::ownedHandles() const
	{
		return owned_count_.load();
	}

	ListenerResult LinuxTransport::openListener(const char *address, std::uint16_t port, int backlog)
	{
		if(!address || backlog <= 0)
			return {Status::InvalidArgument, 0, {}, 0};

		if(!started_.load())
			return {Status::InvalidState, 0, {}, 0};

		sockaddr_in endpoint{};
		endpoint.sin_family = AF_INET;
		endpoint.sin_port = htons(port);
		if(inet_pton(AF_INET, address, &endpoint.sin_addr) != 1)
			return {Status::InvalidArgument, 0, {}, 0};

		const int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
		if(listener < 0)
			return {Status::Fatal, errno, {}, 0};

		Handle handle{static_cast<std::uintptr_t>(listener), true};
		owned_count_.fetch_add(1);
		const auto failed = [this, &handle](int native_error)
			{
				const auto cleanup = close(handle);
				return ListenerResult{Status::Fatal, native_error, handle, 0, cleanup};
			};
		const int reuse = 1;
		if(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
			return failed(errno);

		if(bind(listener, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) != 0)
			return failed(errno);

		if(listen(listener, backlog) != 0)
			return failed(errno);

		socklen_t size = sizeof(endpoint);
		if(getsockname(listener, reinterpret_cast<sockaddr *>(&endpoint), &size) != 0)
			return failed(errno);

		return {Status::Ok, 0, handle, ntohs(endpoint.sin_port)};
	}

	AcceptResult LinuxTransport::accept(Handle listener)
	{
		if(!validNativeHandle(listener))
			return {Status::InvalidArgument, 0, {}};

		if(!started_.load())
			return {Status::InvalidState, 0, {}};

		const int accepted = accept4(static_cast<int>(listener.value), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if(accepted < 0)
		{
			const int native_error = errno;
			return {pendingAcceptError(native_error) ? Status::WouldBlock : operationStatus(native_error),
			        native_error, {}};
		}

		owned_count_.fetch_add(1);
		return {Status::Ok, 0, {static_cast<std::uintptr_t>(accepted), true}};
	}

	PeerAddress LinuxTransport::peerAddress(Handle handle)
	{
		if(!validNativeHandle(handle) || !started_.load()) return {};

		sockaddr_in peer{};
		socklen_t size = sizeof(peer);
		if(getpeername(static_cast<int>(handle.value), reinterpret_cast<sockaddr *>(&peer),
		    &size) != 0 || peer.sin_family != AF_INET) return {};

		return {ntohl(peer.sin_addr.s_addr), ntohs(peer.sin_port), true};
	}

	IoResult LinuxTransport::readSome(Handle handle, std::uint8_t *destination, std::size_t capacity)
	{
		if(!validNativeHandle(handle) || !destination || !capacity)
			return {IoStatus::InvalidArgument, 0, 0};

		if(!started_.load())
			return {IoStatus::InvalidState, 0, 0};

		const auto requested = std::min(capacity, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
		const auto received = recv(static_cast<int>(handle.value), destination, requested, 0);
		if(received > 0)
			return {IoStatus::Progress, static_cast<std::size_t>(received), 0};

		if(received == 0)
			return {IoStatus::PeerClosed, 0, 0};

		const int native_error = errno;
		return {ioStatus(native_error), 0, native_error};
	}

	IoResult LinuxTransport::writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size)
	{
		if(!validNativeHandle(handle) || !bytes || !size)
			return {IoStatus::InvalidArgument, 0, 0};

		if(!started_.load())
			return {IoStatus::InvalidState, 0, 0};

		const auto requested = std::min(size, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
		const auto written = send(static_cast<int>(handle.value), bytes, requested, MSG_NOSIGNAL);
		if(written > 0)
			return {IoStatus::Progress, static_cast<std::size_t>(written), 0};

		if(written == 0)
			return {IoStatus::Fatal, 0, 0};

		const int native_error = errno;
		return {ioStatus(native_error), 0, native_error};
	}

	WaitResult LinuxTransport::wait(const Interest *interests, Ready *ready,
	    std::size_t count, std::uint32_t timeout_ms)
	{
		if(ready)
			for(std::size_t index = 0; index < count; ++index)
				ready[index] = {};

		if(!interests || !ready || !count)
			return {WaitStatus::InvalidArgument, 0, 0};

		if(!started_.load())
			return {WaitStatus::InvalidState, 0, 0};

		std::array<pollfd, maximum_wait_handles> descriptors{};
		std::size_t used = 0;
		for(std::size_t index = 0; index < count; ++index)
		{
			const auto &interest = interests[index];
			if(!validNativeHandle(interest.handle) || (!interest.read && !interest.write))
				return {WaitStatus::InvalidArgument, 0, 0};

			const int descriptor = static_cast<int>(interest.handle.value);
			std::size_t position = 0;
			while(position < used && descriptors[position].fd != descriptor)
				++position;

			if(position == used)
			{
				if(used == descriptors.size())
					return {WaitStatus::InvalidArgument, 0, 0};

				descriptors[used++].fd = descriptor;
			}

			if(interest.read)
				descriptors[position].events |= POLLIN;

			if(interest.write)
				descriptors[position].events |= POLLOUT;
		}
		const auto timeout = static_cast<int>(std::min<std::uint32_t>(timeout_ms, INT_MAX));
		const int result = poll(descriptors.data(), used, timeout);
		if(result == 0)
			return {WaitStatus::Timeout, 0, 0};

		if(result < 0)
		{
			const int native_error = errno;
			return {native_error == EINTR ? WaitStatus::Interrupted : WaitStatus::Fatal, 0, native_error};
		}

		std::size_t ready_count = 0;
		for(std::size_t index = 0; index < count; ++index)
		{
			const auto &interest = interests[index];
			std::size_t position = 0;
			while(descriptors[position].fd != static_cast<int>(interest.handle.value))
				++position;

			const auto events = descriptors[position].revents;
			// HUP can coexist with unread bytes. Let recv observe those and EOF;
			// writeSome observes its own failure without a process-wide SIGPIPE.
			ready[index].readable = interest.read && (events & (POLLIN | POLLHUP));
			ready[index].writable = interest.write && (events & (POLLOUT | POLLHUP));
			ready[index].error = events & (POLLERR | POLLNVAL);
			if(events & POLLNVAL) ready[index].native_error = EBADF;

			if(ready[index].readable || ready[index].writable || ready[index].error)
				++ready_count;
		}
		return {WaitStatus::Ready, ready_count, 0};
	}

	int LinuxTransport::listenerError(Handle listener)
	{
		if(!validNativeHandle(listener) || !started_.load()) return 0;

		int native_error = 0;
		socklen_t size = sizeof(native_error);
		if(getsockopt(static_cast<int>(listener.value), SOL_SOCKET, SO_ERROR, &native_error, &size) != 0)
			return errno;

		return native_error;
	}

	OperationResult LinuxTransport::shutdownWrite(Handle handle)
	{
		if(!validNativeHandle(handle))
			return {Status::InvalidArgument, 0};

		if(!started_.load())
			return {Status::InvalidState, 0};

		if(shutdown(static_cast<int>(handle.value), SHUT_WR) == 0)
			return {};

		const int native_error = errno;
		return {operationStatus(native_error), native_error};
	}

	CloseResult LinuxTransport::close(Handle &handle)
	{
		if(!validNativeHandle(handle))
			return {Status::InvalidArgument, 0, false};

		if(!started_.load())
			return {Status::InvalidState, 0, false};

		const int result = ::close(static_cast<int>(handle.value));
		const int native_error = result == 0 ? 0 : errno;
		// Linux consumes a valid owned descriptor before reporting a late error,
		// including EINTR. EBADF already means no descriptor remains to retry.
		handle = {};
		owned_count_.fetch_sub(1);
		return {result == 0 ? Status::Ok : operationStatus(native_error), native_error, true};
	}

} // namespace NaWServer::tcp
