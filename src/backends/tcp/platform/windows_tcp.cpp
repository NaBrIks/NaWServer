// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <NaWServer/backends/windows_tcp.h>
#include <algorithm>
#include <climits>

namespace NaWServer::tcp
{

	namespace
	{

		Status operationStatus(int native_error)
		{
			if(native_error == WSAEWOULDBLOCK)
				return Status::WouldBlock;

			if(native_error == WSAEINTR)
				return Status::Interrupted;

			return Status::Fatal;
		}

		IoStatus ioStatus(int native_error)
		{
			if(native_error == WSAEWOULDBLOCK)
				return IoStatus::WouldBlock;

			if(native_error == WSAEINTR)
				return IoStatus::Interrupted;

			return IoStatus::Fatal;
		}

		bool validNativeHandle(Handle handle)
		{
			return handle.valid && handle.value != static_cast<std::uintptr_t>(INVALID_SOCKET);
		}

	} // namespace

	WindowsTransport::~WindowsTransport()
	{
		// Never close another owner's socket or force cleanup with live handles.
		(void)stop();
	}

	OperationResult WindowsTransport::start()
	{
		if(started_.load())
			return {Status::InvalidState, 0};

		WSADATA data{};
		const int result = WSAStartup(MAKEWORD(2, 2), &data);
		if(result != 0)
			return {Status::Fatal, result};

		started_.store(true);
		return {};
	}

	OperationResult WindowsTransport::stop()
	{
		if(owned_count_.load() != 0)
			return {Status::InvalidState, 0};

		if(!started_.load())
			return {};

		if(WSACleanup() == SOCKET_ERROR)
			return {Status::Fatal, WSAGetLastError()};

		started_.store(false);
		return {};
	}

	std::size_t WindowsTransport::ownedHandles() const
	{
		return owned_count_.load();
	}

	ListenerResult WindowsTransport::openListener(const char *address, std::uint16_t port, int backlog)
	{
		if(!address || backlog <= 0)
			return {Status::InvalidArgument, 0, {}, 0};

		if(!started_.load())
			return {Status::InvalidState, 0, {}, 0};

		sockaddr_in endpoint{};
		endpoint.sin_family = AF_INET;
		endpoint.sin_port = htons(port);
		if(InetPtonA(AF_INET, address, &endpoint.sin_addr) != 1)
			return {Status::InvalidArgument, 0, {}, 0};

		const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if(listener == INVALID_SOCKET)
			return {Status::Fatal, WSAGetLastError(), {}, 0};

		Handle handle{static_cast<std::uintptr_t>(listener), true};
		owned_count_.fetch_add(1);
		const auto failed = [this, &handle](int native_error)
			{
				const CloseResult cleanup = close(handle);
				return ListenerResult{Status::Fatal,
				                      cleanup.released ? native_error : cleanup.native_error, handle, 0, cleanup};
			};

		const BOOL exclusive = TRUE;
		if(setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
		    reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
		{
			return failed(WSAGetLastError());
		}

		u_long nonblocking = 1;
		if(ioctlsocket(listener, FIONBIO, &nonblocking) == SOCKET_ERROR)
			return failed(WSAGetLastError());

		if(bind(listener, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR)
			return failed(WSAGetLastError());

		if(listen(listener, backlog) == SOCKET_ERROR)
			return failed(WSAGetLastError());

		int endpoint_size = sizeof(endpoint);
		if(getsockname(listener, reinterpret_cast<sockaddr *>(&endpoint), &endpoint_size) == SOCKET_ERROR)
			return failed(WSAGetLastError());

		return {Status::Ok, 0, handle, ntohs(endpoint.sin_port)};
	}

	AcceptResult WindowsTransport::accept(Handle listener)
	{
		if(!validNativeHandle(listener))
			return {Status::InvalidArgument, 0, {}};

		if(!started_.load())
			return {Status::InvalidState, 0, {}};

		const SOCKET accepted = ::accept(static_cast<SOCKET>(listener.value), nullptr, nullptr);
		if(accepted == INVALID_SOCKET)
		{
			const int native_error = WSAGetLastError();
			return {operationStatus(native_error), native_error, {}};
		}

		Handle handle{static_cast<std::uintptr_t>(accepted), true};
		owned_count_.fetch_add(1);
		u_long nonblocking = 1;
		if(ioctlsocket(accepted, FIONBIO, &nonblocking) == SOCKET_ERROR)
		{
			const int native_error = WSAGetLastError();
			const CloseResult cleanup = close(handle);
			return {Status::Fatal, cleanup.released ? native_error : cleanup.native_error, handle, cleanup};
		}

		return {Status::Ok, 0, handle};
	}

	PeerAddress WindowsTransport::peerAddress(Handle handle)
	{
		if(!handle.valid || !started_.load()) return {};

		sockaddr_in peer{};
		int size = sizeof(peer);
		if(getpeername(static_cast<SOCKET>(handle.value), reinterpret_cast<sockaddr *>(&peer),
		    &size) != 0 || peer.sin_family != AF_INET) return {};

		return {ntohl(peer.sin_addr.s_addr), ntohs(peer.sin_port), true};
	}

	IoResult WindowsTransport::readSome(Handle handle, std::uint8_t *destination, std::size_t capacity)
	{
		if(!validNativeHandle(handle) || !destination || capacity == 0)
			return {IoStatus::InvalidArgument, 0, 0};

		if(!started_.load())
			return {IoStatus::InvalidState, 0, 0};

		const int received = recv(static_cast<SOCKET>(handle.value), reinterpret_cast<char *>(destination),
		        static_cast<int>(std::min(capacity, static_cast<std::size_t>(INT_MAX))), 0);
		if(received > 0)
			return {IoStatus::Progress, static_cast<std::size_t>(received), 0};

		if(received == 0)
			return {IoStatus::PeerClosed, 0, 0};

		const int native_error = WSAGetLastError();
		return {ioStatus(native_error), 0, native_error};
	}

	IoResult WindowsTransport::writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size)
	{
		if(!validNativeHandle(handle) || !bytes || size == 0)
			return {IoStatus::InvalidArgument, 0, 0};

		if(!started_.load())
			return {IoStatus::InvalidState, 0, 0};

		const int written = send(static_cast<SOCKET>(handle.value), reinterpret_cast<const char *>(bytes),
		        static_cast<int>(std::min(size, static_cast<std::size_t>(INT_MAX))), 0);
		if(written > 0)
			return {IoStatus::Progress, static_cast<std::size_t>(written), 0};

		if(written == 0)
			return {IoStatus::Fatal, 0, 0};

		const int native_error = WSAGetLastError();
		return {ioStatus(native_error), 0, native_error};
	}

	WaitResult WindowsTransport::wait(const Interest *interests, Ready *ready,
	    std::size_t count, std::uint32_t timeout_ms)
	{
		if(ready)
		{
			for(std::size_t index = 0; index < count; ++index)
				ready[index] = {};
		}

		if(!interests || !ready || count == 0)
			return {WaitStatus::InvalidArgument, 0, 0};

		if(!started_.load())
			return {WaitStatus::InvalidState, 0, 0};

		fd_set readable;
		fd_set writable;
		fd_set exceptional;
		FD_ZERO(&readable);
		FD_ZERO(&writable);
		FD_ZERO(&exceptional);
		for(std::size_t index = 0; index < count; ++index)
		{
			const Interest &interest = interests[index];
			if(!validNativeHandle(interest.handle) || (!interest.read && !interest.write))
				return {WaitStatus::InvalidArgument, 0, 0};

			const SOCKET socket_handle = static_cast<SOCKET>(interest.handle.value);
			if(!FD_ISSET(socket_handle, &exceptional))
			{
				if(exceptional.fd_count == FD_SETSIZE)
					return {WaitStatus::InvalidArgument, 0, 0};

				FD_SET(socket_handle, &exceptional);
			}

			if(interest.read)
				FD_SET(socket_handle, &readable);

			if(interest.write)
				FD_SET(socket_handle, &writable);
		}

		timeval timeout{};
		timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
		timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
		const int result = select(0, readable.fd_count ? &readable : nullptr,
		        writable.fd_count ? &writable : nullptr, &exceptional, &timeout);
		if(result == 0)
			return {WaitStatus::Timeout, 0, 0};

		if(result == SOCKET_ERROR)
		{
			const int native_error = WSAGetLastError();
			return {native_error == WSAEINTR ? WaitStatus::Interrupted : WaitStatus::Fatal, 0, native_error};
		}

		std::size_t ready_count = 0;
		for(std::size_t index = 0; index < count; ++index)
		{
			const Interest &interest = interests[index];
			const SOCKET socket_handle = static_cast<SOCKET>(interest.handle.value);
			Ready &event = ready[index];
			event.readable = interest.read && FD_ISSET(socket_handle, &readable);
			event.writable = interest.write && FD_ISSET(socket_handle, &writable);
			event.error = FD_ISSET(socket_handle, &exceptional);

			if(event.readable || event.writable || event.error)
				++ready_count;
		}

		return {WaitStatus::Ready, ready_count, 0};
	}

	int WindowsTransport::listenerError(Handle listener)
	{
		if(!validNativeHandle(listener) || !started_.load()) return 0;

		int native_error = 0;
		int size = sizeof(native_error);
		if(getsockopt(static_cast<SOCKET>(listener.value), SOL_SOCKET, SO_ERROR,
		    reinterpret_cast<char *>(&native_error), &size) != 0) return WSAGetLastError();

		return native_error;
	}

	OperationResult WindowsTransport::shutdownWrite(Handle handle)
	{
		if(!validNativeHandle(handle))
			return {Status::InvalidArgument, 0};

		if(!started_.load())
			return {Status::InvalidState, 0};

		if(shutdown(static_cast<SOCKET>(handle.value), SD_SEND) == 0)
			return {};

		const int native_error = WSAGetLastError();
		return {operationStatus(native_error), native_error};
	}

	CloseResult WindowsTransport::close(Handle &handle)
	{
		if(!validNativeHandle(handle))
			return {Status::InvalidArgument, 0, false};

		if(!started_.load())
			return {Status::InvalidState, 0, false};

		const int result = closesocket(static_cast<SOCKET>(handle.value));
		const int native_error = result == 0 ? 0 : WSAGetLastError();
		const bool released = result == 0 || native_error == WSAENOTSOCK;
		if(released)
		{
			handle = {};
			owned_count_.fetch_sub(1);
		}

		return {result == 0 ? Status::Ok : operationStatus(native_error), native_error, released};
	}

} // namespace NaWServer::tcp
