// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

// Test-client plumbing only. Protocol assertions and production transports stay
// in their existing suites; this is neither a public API nor a production HAL.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <NaWServer/backends/windows_tcp.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <NaWServer/backends/linux_tcp.h>
#endif
#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>

namespace NaWServer::test::host
{
#ifdef _WIN32
	using Handle = SOCKET;
	using TcpTransport = tcp::WindowsTransport;
	inline constexpr Handle invalid = INVALID_SOCKET;
	inline constexpr int error = SOCKET_ERROR;
	inline constexpr int shutdown_send = SD_SEND;
	inline constexpr int interrupted_error = WSAEINTR;
	inline constexpr int would_block_error = WSAEWOULDBLOCK;
	inline constexpr int aborted_error = WSAECONNABORTED;
	inline constexpr int refused_error = WSAECONNREFUSED;
#else
	using Handle = int;
	using TcpTransport = tcp::LinuxTransport;
	inline constexpr Handle invalid = -1;
	inline constexpr int error = -1;
	inline constexpr int shutdown_send = SHUT_WR;
	inline constexpr int interrupted_error = EINTR;
	inline constexpr int would_block_error = EWOULDBLOCK;
	inline constexpr int aborted_error = ECONNABORTED;
	inline constexpr int refused_error = ECONNREFUSED;
#endif

	inline int initialize()
	{
#ifdef _WIN32
		WSADATA data{};
		return WSAStartup(MAKEWORD(2, 2), &data);
#else
		return 0;
#endif
	}

	inline int cleanup()
	{
#ifdef _WIN32
		return WSACleanup();
#else
		return 0;
#endif
	}

	inline int lastError()
	{
#ifdef _WIN32
		return WSAGetLastError();
#else
		return errno;
#endif
	}

	inline bool peerReset(int native_error)
	{
#ifdef _WIN32
		return native_error == WSAECONNRESET || native_error == WSAECONNABORTED;
#else
		return native_error == ECONNRESET || native_error == ECONNABORTED;
#endif
	}

	inline int closeSocket(Handle socket)
	{
#ifdef _WIN32
		return closesocket(socket);
#else
		return close(socket);
#endif
	}

	inline bool setTimeouts(Handle socket, std::uint32_t milliseconds)
	{
#ifdef _WIN32
		const DWORD timeout = milliseconds;
		const auto *bytes = reinterpret_cast<const char *>(&timeout);
#else
		const timeval timeout{static_cast<time_t>(milliseconds / 1000),
			                  static_cast<suseconds_t>((milliseconds % 1000) * 1000)};
		const auto *bytes = &timeout;
#endif
		return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, bytes, sizeof(timeout)) == 0 &&
		       setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, bytes, sizeof(timeout)) == 0;
	}

	inline int sendSome(Handle socket, const char *bytes, std::size_t size, int flags = 0)
	{
		const auto bounded = static_cast<int>(std::min(size, static_cast<std::size_t>(INT_MAX)));
#ifdef _WIN32
		return send(socket, bytes, bounded, flags);
#else
		// An expected peer close remains an explicit send error, never SIGPIPE.
		return static_cast<int>(send(socket, bytes, static_cast<std::size_t>(bounded), flags | MSG_NOSIGNAL));
#endif
	}

	inline int receiveSome(Handle socket, char *bytes, std::size_t size, int flags = 0)
	{
		const auto bounded = static_cast<int>(std::min(size, static_cast<std::size_t>(INT_MAX)));
#ifdef _WIN32
		return recv(socket, bytes, bounded, flags);
#else
		return static_cast<int>(recv(socket, bytes, static_cast<std::size_t>(bounded), flags));
#endif
	}

} // namespace NaWServer::test::host
