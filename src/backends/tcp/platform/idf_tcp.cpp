// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/esp_idf_tcp.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <limits>

namespace NaWServer::tcp
{

	namespace
	{

		static_assert(LWIP_SO_SNDTIMEO, "IDF TCP requires finite SO_SNDTIMEO support");
		static_assert(CONFIG_LWIP_MAX_SOCKETS > 0, "IDF TCP requires socket capacity");

		bool validNativeHandle(Handle handle)
		{
			// Unsigned subtraction rejects values below the offset too, including
			// when an SDK configuration puts the first valid socket at zero.
			return handle.valid && handle.value - static_cast<std::uintptr_t>(LWIP_SOCKET_OFFSET) <
			       static_cast<std::uintptr_t>(CONFIG_LWIP_MAX_SOCKETS);
		}

		Status operationStatus(int native_error)
		{
			if(native_error == EAGAIN || native_error == EWOULDBLOCK)
				return Status::WouldBlock;

			if(native_error == EINTR)
				return Status::Interrupted;

			return Status::Fatal;
		}

		Status retainedStatus(int native_error)
		{
			// SDK 5.4: sockets.c only frees after successful prepare-delete;
			// api/err.c maps ERR_MEM/INPROGRESS/WOULDBLOCK to these errno values.
			if(native_error == ENOMEM || native_error == EINPROGRESS)
				return Status::WouldBlock;

			return operationStatus(native_error);
		}

		IoStatus ioStatus(int native_error)
		{
			if(native_error == EAGAIN || native_error == EWOULDBLOCK)
				return IoStatus::WouldBlock;

			if(native_error == EINTR)
				return IoStatus::Interrupted;

			return IoStatus::Fatal;
		}

		OperationResult installTimeout(int descriptor)
		{
#if LWIP_SO_SNDRCVTIMEO_NONSTANDARD
			const int timeout = IdfTransport::sdk_send_timeout_ms;
#else
			const timeval timeout{0, static_cast<long>(IdfTransport::sdk_send_timeout_ms) * 1000};
#endif
			if(lwip_setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0)
				return {};

			const int native_error = errno;
			return {retainedStatus(native_error), native_error};
		}

		OperationResult configureSocket(int descriptor)
		{
			const auto guard = installTimeout(descriptor);
			if(guard.status != Status::Ok)
				return guard;

			// HTTP headers/bodies and WS headers/payloads are separate writes.
			// Holding a small tail for Nagle can outlive the bounded close drain.
			const int no_delay = 1;
			if(lwip_setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0)
				return {operationStatus(errno), errno};

			std::uint32_t nonblocking = 1;
			if(lwip_ioctl(descriptor, FIONBIO, &nonblocking) == 0)
				return {};

			const int native_error = errno;
			return {operationStatus(native_error), native_error};
		}

	} // namespace

	IdfTransport::~IdfTransport()
	{
		(void)stop();
	}

	OperationResult IdfTransport::start()
	{
		if(started_.load())
			return {Status::InvalidState, 0};

		started_.store(true);
		return {};
	}

	OperationResult IdfTransport::stop()
	{
		if(owned_count_.load())
			return {Status::InvalidState, 0};

		started_.store(false);
		return {};
	}

	std::size_t IdfTransport::ownedHandles() const
	{
		return owned_count_.load();
	}

	ListenerResult IdfTransport::openListener(const char *address, std::uint16_t port, int backlog)
	{
		if(!address || backlog <= 0)
			return {Status::InvalidArgument, 0, {}, 0};

		if(!started_.load())
			return {Status::InvalidState, 0, {}, 0};

		sockaddr_in endpoint{};
		endpoint.sin_family = AF_INET;
		endpoint.sin_port = lwip_htons(port);
		if(lwip_inet_pton(AF_INET, address, &endpoint.sin_addr) != 1)
			return {Status::InvalidArgument, 0, {}, 0};

		const int listener = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if(listener < 0)
		{
			const int native_error = errno;
			return {operationStatus(native_error), native_error, {}, 0};
		}

		Handle handle{static_cast<std::uintptr_t>(listener), true};
		owned_count_.fetch_add(1);
		const auto failed = [this, &handle](int native_error)
			{
				const auto cleanup = close(handle);
				return ListenerResult{Status::Fatal, native_error, handle, 0, cleanup};
			};
		const auto configured = configureSocket(listener);
		if(configured.status != Status::Ok)
			return failed(configured.native_error);

#if SO_REUSE
		const int reuse = 1;
		if(lwip_setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
			return failed(errno);

#endif
		if(lwip_bind(listener, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) != 0)
			return failed(errno);

		if(lwip_listen(listener, backlog) != 0)
			return failed(errno);

		socklen_t size = sizeof(endpoint);
		if(lwip_getsockname(listener, reinterpret_cast<sockaddr *>(&endpoint), &size) != 0)
			return failed(errno);

		return {Status::Ok, 0, handle, lwip_ntohs(endpoint.sin_port)};
	}

	AcceptResult IdfTransport::accept(Handle listener)
	{
		if(!validNativeHandle(listener))
			return {Status::InvalidArgument, 0, {}};

		if(!started_.load())
			return {Status::InvalidState, 0, {}};

		const int accepted = lwip_accept(static_cast<int>(listener.value), nullptr, nullptr);
		if(accepted < 0)
		{
			const int native_error = errno;
			return {operationStatus(native_error), native_error, {}};
		}

		Handle handle{static_cast<std::uintptr_t>(accepted), true};
		owned_count_.fetch_add(1);
		const auto configured = configureSocket(accepted);
		if(configured.status != Status::Ok)
		{
			const auto cleanup = close(handle);
			return {Status::Fatal, configured.native_error, handle, cleanup};
		}

		return {Status::Ok, 0, handle};
	}

	PeerAddress IdfTransport::peerAddress(Handle handle)
	{
		if(!validNativeHandle(handle) || !started_.load()) return {};

		sockaddr_in peer{};
		socklen_t size = sizeof(peer);
		if(lwip_getpeername(static_cast<int>(handle.value), reinterpret_cast<sockaddr *>(&peer),
		    &size) != 0 || peer.sin_family != AF_INET) return {};

		return {lwip_ntohl(peer.sin_addr.s_addr), lwip_ntohs(peer.sin_port), true};
	}

	IoResult IdfTransport::readSome(Handle handle, std::uint8_t *destination, std::size_t capacity)
	{
		if(!validNativeHandle(handle) || !destination || !capacity)
			return {IoStatus::InvalidArgument, 0, 0};

		if(!started_.load())
			return {IoStatus::InvalidState, 0, 0};

		const auto requested = std::min(capacity, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
		const auto received = lwip_recv(static_cast<int>(handle.value), destination, requested, MSG_DONTWAIT);
		if(received > 0)
			return {IoStatus::Progress, static_cast<std::size_t>(received), 0};

		if(received == 0)
			return {IoStatus::PeerClosed, 0, 0};

		const int native_error = errno;
		return {ioStatus(native_error), 0, native_error};
	}

	IoResult IdfTransport::writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size)
	{
		if(!validNativeHandle(handle) || !bytes || !size)
			return {IoStatus::InvalidArgument, 0, 0};

		if(!started_.load())
			return {IoStatus::InvalidState, 0, 0};

		const auto requested = std::min(size, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
		const auto sent = lwip_send(static_cast<int>(handle.value), bytes, requested, MSG_DONTWAIT);
		if(sent > 0)
			return {IoStatus::Progress, static_cast<std::size_t>(sent), 0};

		if(sent == 0)
			return {IoStatus::Fatal, 0, 0};

		const int native_error = errno;
		return {ioStatus(native_error), 0, native_error};
	}

	WaitResult IdfTransport::wait(const Interest *interests, Ready *ready,
	    std::size_t count, std::uint32_t timeout_ms)
	{
		if(ready)
			for(std::size_t index = 0; index < count; ++index)
				ready[index] = {};

		if(!interests || !ready || !count)
			return {WaitStatus::InvalidArgument, 0, 0};

		if(!started_.load())
			return {WaitStatus::InvalidState, 0, 0};

		std::array<pollfd, CONFIG_LWIP_MAX_SOCKETS> descriptors{};
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
				descriptors[used++].fd = descriptor;

			if(interest.read)
				descriptors[position].events |= POLLIN;

			if(interest.write)
				descriptors[position].events |= POLLOUT;
		}
		// Native handles were range-checked: distinct descriptors cannot exceed
		// CONFIG_LWIP_MAX_SOCKETS. Duplicates do not consume another scratch entry.
		const int timeout = static_cast<int>(std::min<std::uint32_t>(timeout_ms, INT_MAX));
		const int result = lwip_poll(descriptors.data(), static_cast<nfds_t>(used), timeout);
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
			ready[index].readable = interest.read && (events & (POLLIN | POLLHUP));
			ready[index].writable = interest.write && (events & (POLLOUT | POLLHUP));
			ready[index].error = events & (POLLERR | POLLNVAL);
			if(events & POLLNVAL) ready[index].native_error = EBADF;

			if(ready[index].readable || ready[index].writable || ready[index].error)
				++ready_count;
		}
		return {WaitStatus::Ready, ready_count, 0};
	}

	int IdfTransport::listenerError(Handle listener)
	{
		if(!validNativeHandle(listener) || !started_.load()) return 0;

		int native_error = 0;
		socklen_t size = sizeof(native_error);
		if(lwip_getsockopt(static_cast<int>(listener.value), SOL_SOCKET, SO_ERROR, &native_error, &size) != 0)
			return errno;

		return native_error;
	}

	OperationResult IdfTransport::shutdownWrite(Handle handle)
	{
		if(!validNativeHandle(handle))
			return {Status::InvalidArgument, 0};

		if(!started_.load())
			return {Status::InvalidState, 0};

		const auto guard = installTimeout(static_cast<int>(handle.value));
		if(guard.status != Status::Ok)
			return guard;

		if(lwip_shutdown(static_cast<int>(handle.value), SHUT_WR) == 0)
			return {};

		const int native_error = errno;
		return {retainedStatus(native_error), native_error};
	}

	CloseResult IdfTransport::close(Handle &handle)
	{
		if(!validNativeHandle(handle))
			return {Status::InvalidArgument, 0, false};

		if(!started_.load())
			return {Status::InvalidState, 0, false};

		const auto guard = installTimeout(static_cast<int>(handle.value));
		if(guard.status != Status::Ok && guard.native_error != EBADF)
			return {guard.status, guard.native_error, false};

		const int result = guard.status == Status::Ok ? lwip_close(static_cast<int>(handle.value)) : -1;
		const int native_error = result == 0 ? 0 : guard.status == Status::Ok ? errno : guard.native_error;
		const bool released = result == 0 || native_error == EBADF;
		if(released)
		{
			handle = {};
			owned_count_.fetch_sub(1);
		}

		return {result == 0 ? Status::Ok : retainedStatus(native_error), native_error, released};
	}

} // namespace NaWServer::tcp
