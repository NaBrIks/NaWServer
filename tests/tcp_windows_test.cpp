// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <NaWServer/backends/windows_tcp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

namespace
{

	using namespace NaWServer::tcp;
	using Clock = std::chrono::steady_clock;
	std::atomic<unsigned> checked{0};

	void check(bool value, int line)
	{
		checked.fetch_add(1);
		if(!value)
		{
			std::printf("TCP_WINDOWS_FAIL line=%d native=%d\n", line, WSAGetLastError());
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	SOCKET connectClient(std::uint16_t port)
	{
		const SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(client != INVALID_SOCKET);
		const DWORD timeout = 3000;
		CHECK(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
		    reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0);
		CHECK(setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
		    reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0);
		sockaddr_in endpoint{};
		endpoint.sin_family = AF_INET;
		endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		endpoint.sin_port = htons(port);
		CHECK(connect(client, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) == 0);
		return client;
	}

	void waitFor(WindowsTransport &transport, Handle handle, bool read, Clock::time_point deadline)
	{
		CHECK(Clock::now() < deadline);
		const Interest interest{handle, read, !read};
		Ready ready;
		const WaitResult result = transport.wait(&interest, &ready, 1, 20);
		CHECK(result.status == WaitStatus::Ready || result.status == WaitStatus::Timeout ||
		    result.status == WaitStatus::Interrupted);
	}

	Handle acceptClient(WindowsTransport &transport, Handle listener)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		for(;;)
		{
			const AcceptResult accepted = transport.accept(listener);
			if(accepted.status == Status::Ok)
			{
				CHECK(accepted.handle.valid);
				return accepted.handle;
			}

			CHECK(accepted.status == Status::WouldBlock || accepted.status == Status::Interrupted);
			waitFor(transport, listener, true, deadline);
		}
	}

	void sendClient(SOCKET client, const std::uint8_t *bytes, std::size_t size)
	{
		std::size_t offset = 0;
		while(offset < size)
		{
			const int sent = send(client, reinterpret_cast<const char *>(bytes + offset),
			        static_cast<int>(std::min(size - offset, std::size_t{71})), 0);
			CHECK(sent > 0);
			offset += static_cast<std::size_t>(sent);
		}
	}

	void receiveTransport(WindowsTransport &transport, Handle connection,
	    std::uint8_t *destination, std::size_t size)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		std::size_t offset = 0;
		while(offset < size)
		{
			CHECK(Clock::now() < deadline);
			std::array<std::uint8_t, 129> guarded;
			guarded.fill(0xa5);
			const auto capacity = std::min(size - offset, std::size_t{127});
			const IoResult result = transport.readSome(connection, guarded.data() + 1, capacity);
			CHECK(guarded.front() == 0xa5 && guarded[capacity + 1] == 0xa5);
			if(result.status == IoStatus::Progress)
			{
				CHECK(result.size > 0 && result.size <= capacity);
				std::copy_n(guarded.data() + 1, result.size, destination + offset);
				offset += result.size;
				continue;
			}

			CHECK(result.status == IoStatus::WouldBlock || result.status == IoStatus::Interrupted);
			CHECK(result.size == 0);
			waitFor(transport, connection, true, deadline);
		}
	}

	void writeTransport(WindowsTransport &transport, Handle connection,
	    const std::uint8_t *bytes, std::size_t size)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		std::size_t offset = 0;
		while(offset < size)
		{
			CHECK(Clock::now() < deadline);
			const auto capacity = std::min(size - offset, std::size_t{17});
			const IoResult result = transport.writeSome(connection, bytes + offset, capacity);
			if(result.status == IoStatus::Progress)
			{
				CHECK(result.size > 0 && result.size <= capacity);
				offset += result.size;
				continue;
			}

			CHECK(result.status == IoStatus::WouldBlock || result.status == IoStatus::Interrupted);
			CHECK(result.size == 0);
			waitFor(transport, connection, false, deadline);
		}
	}

	void receiveClient(SOCKET client, std::uint8_t *destination, std::size_t size)
	{
		std::size_t offset = 0;
		while(offset < size)
		{
			const int received = recv(client, reinterpret_cast<char *>(destination + offset),
			        static_cast<int>(std::min(size - offset, std::size_t{53})), 0);
			CHECK(received > 0);
			offset += static_cast<std::size_t>(received);
		}
	}

	void closeOwned(WindowsTransport &transport, Handle &handle)
	{
		const CloseResult result = transport.close(handle);
		CHECK(result.status == Status::Ok && result.released && !handle.valid);
		CHECK(transport.close(handle).status == Status::InvalidArgument);
	}

	void lifecycleAndValidation()
	{
		WindowsTransport transport;
		CHECK(transport.ownedHandles() == 0);
		CHECK(transport.openListener("127.0.0.1", 0, 1).status == Status::InvalidState);
		CHECK(transport.stop().status == Status::Ok);
		CHECK(transport.start().status == Status::Ok);
		CHECK(transport.start().status == Status::InvalidState);
		CHECK(transport.openListener(nullptr, 0, 1).status == Status::InvalidArgument);
		CHECK(transport.openListener("localhost", 0, 1).status == Status::InvalidArgument);
		CHECK(transport.openListener("127.0.0.999", 0, 1).status == Status::InvalidArgument);
		CHECK(transport.openListener("127.0.0.1", 0, 0).status == Status::InvalidArgument);
		auto listener = transport.openListener("127.0.0.1", 0, 2);
		CHECK(listener.status == Status::Ok && listener.handle.valid && listener.port != 0);
		CHECK(transport.ownedHandles() == 1);
		CHECK(transport.stop().status == Status::InvalidState);
		CHECK(transport.accept(listener.handle).status == Status::WouldBlock);
		const auto conflict = transport.openListener("127.0.0.1", listener.port, 1);
		CHECK(conflict.status == Status::Fatal && conflict.native_error != 0 && !conflict.handle.valid);
		CHECK(transport.ownedHandles() == 1);

		std::uint8_t byte = 0;
		CHECK(transport.accept({}).status == Status::InvalidArgument);
		CHECK(transport.readSome({}, &byte, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.writeSome({}, &byte, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.readSome(listener.handle, nullptr, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.readSome(listener.handle, &byte, 0).status == IoStatus::InvalidArgument);
		CHECK(transport.writeSome(listener.handle, nullptr, 1).status == IoStatus::InvalidArgument);
		CHECK(transport.writeSome(listener.handle, &byte, 0).status == IoStatus::InvalidArgument);
		CHECK(transport.shutdownWrite({}).status == Status::InvalidArgument);
		const Interest interest{listener.handle, true, false};
		Ready ready{true, true, true};
		CHECK(transport.wait(nullptr, &ready, 1, 0).status == WaitStatus::InvalidArgument);
		CHECK(!ready.readable && !ready.writable && !ready.error);
		CHECK(transport.wait(&interest, nullptr, 1, 0).status == WaitStatus::InvalidArgument);
		CHECK(transport.wait(&interest, &ready, 0, 0).status == WaitStatus::InvalidArgument);
		ready = {true, true, true};
		const auto idle = transport.wait(&interest, &ready, 1, 0);
		CHECK(idle.status == WaitStatus::Timeout && idle.ready_count == 0);
		CHECK(!ready.readable && !ready.writable && !ready.error);
		const Interest no_events{listener.handle, false, false};
		CHECK(transport.wait(&no_events, &ready, 1, 0).status == WaitStatus::InvalidArgument);

		std::array<Interest, FD_SETSIZE + 1> overflow;
		std::array<Ready, FD_SETSIZE + 1> overflow_ready;
		for(std::size_t index = 0; index < overflow.size(); ++index)
		{
			overflow[index] = {{index, true}, true, false};
			overflow_ready[index] = {true, true, true};
		}

		CHECK(transport.wait(overflow.data(), overflow_ready.data(), overflow.size(), 0).status ==
		    WaitStatus::InvalidArgument);
		for(const Ready &event : overflow_ready)
			CHECK(!event.readable && !event.writable && !event.error);

		const auto original_port = listener.port;
		closeOwned(transport, listener.handle);
		CHECK(transport.ownedHandles() == 0);
		listener = transport.openListener("127.0.0.1", original_port, 1);
		CHECK(listener.status == Status::Ok && listener.port == original_port);
		closeOwned(transport, listener.handle);
		CHECK(transport.stop().status == Status::Ok);
		ready = {true, true, true};
		CHECK(transport.wait(&interest, &ready, 1, 0).status == WaitStatus::InvalidState);
		CHECK(!ready.readable && !ready.writable && !ready.error);
	}

	void binaryHalfClose()
	{
		WindowsTransport transport;
		CHECK(transport.start().status == Status::Ok);
		auto listener = transport.openListener("127.0.0.1", 0, 2);
		CHECK(listener.status == Status::Ok);
		const SOCKET client = connectClient(listener.port);
		Handle connection = acceptClient(transport, listener.handle);
		CHECK(transport.ownedHandles() == 2);
		std::uint8_t byte = 0;
		CHECK(transport.readSome(connection, &byte, 1).status == IoStatus::WouldBlock);
		std::array<Interest, 3> interests{{{connection, true, true},
			{connection, false, true}, {connection, true, false}}};
		std::array<Ready, 3> ready;
		const auto writable = transport.wait(interests.data(), ready.data(), interests.size(), 0);
		CHECK(writable.status == WaitStatus::Ready && writable.ready_count == 2);
		CHECK(ready[0].writable && !ready[0].readable && ready[1].writable && !ready[2].readable);

		std::array<std::uint8_t, 4096> original;
		for(std::size_t index = 0; index < original.size(); ++index)
			original[index] = static_cast<std::uint8_t>(index * 37);

		sendClient(client, original.data(), original.size());
		CHECK(shutdown(client, SD_SEND) == 0);
		std::array<std::uint8_t, 4096> received;
		receiveTransport(transport, connection, received.data(), received.size());
		CHECK(received == original);
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		for(;;)
		{
			const IoResult result = transport.readSome(connection, &byte, 1);
			if(result.status == IoStatus::PeerClosed)
			{
				CHECK(result.size == 0);
				break;
			}

			CHECK(result.status == IoStatus::WouldBlock || result.status == IoStatus::Interrupted);
			waitFor(transport, connection, true, deadline);
		}

		// Input EOF must not prevent writing the response on the other TCP half.
		writeTransport(transport, connection, original.data(), original.size());
		CHECK(transport.shutdownWrite(connection).status == Status::Ok);
		received.fill(0);
		receiveClient(client, received.data(), received.size());
		CHECK(received == original);
		CHECK(recv(client, reinterpret_cast<char *>(&byte), 1, 0) == 0);
		closeOwned(transport, connection);
		CHECK(closesocket(client) == 0);
		closeOwned(transport, listener.handle);
		CHECK(transport.ownedHandles() == 0);
		CHECK(transport.stop().status == Status::Ok);
	}

	void independentOwners()
	{
		WindowsTransport transport;
		CHECK(transport.start().status == Status::Ok);
		auto listener = transport.openListener("127.0.0.1", 0, 2);
		CHECK(listener.status == Status::Ok);
		std::array<SOCKET, 2> clients;
		std::array<Handle, 2> connections;
		for(std::size_t index = 0; index < clients.size(); ++index)
		{
			clients[index] = connectClient(listener.port);
			connections[index] = acceptClient(transport, listener.handle);
		}

		CHECK(transport.ownedHandles() == 3);
		std::array<std::thread, 2> owners;
		for(std::size_t index = 0; index < owners.size(); ++index)
		{
			owners[index] = std::thread([&transport, &connections, index]
			{
				std::array<std::uint8_t, 257> input;
				receiveTransport(transport, connections[index], input.data(), input.size());
				writeTransport(transport, connections[index], input.data(), input.size());
				CHECK(transport.shutdownWrite(connections[index]).status == Status::Ok);
				closeOwned(transport, connections[index]);
			});
		}

		for(std::size_t index = 0; index < clients.size(); ++index)
		{
			std::array<std::uint8_t, 257> input;
			input.fill(static_cast<std::uint8_t>(17 + index));
			sendClient(clients[index], input.data(), input.size());
			CHECK(shutdown(clients[index], SD_SEND) == 0);
			std::array<std::uint8_t, 257> output;
			receiveClient(clients[index], output.data(), output.size());
			CHECK(input == output);
			CHECK(closesocket(clients[index]) == 0);
		}

		for(std::thread &owner : owners)
			owner.join();

		CHECK(transport.ownedHandles() == 1);
		closeOwned(transport, listener.handle);
		CHECK(transport.stop().status == Status::Ok);
	}

	void peerReset()
	{
		WindowsTransport transport;
		CHECK(transport.start().status == Status::Ok);
		auto listener = transport.openListener("127.0.0.1", 0, 1);
		CHECK(listener.status == Status::Ok);
		const SOCKET client = connectClient(listener.port);
		Handle connection = acceptClient(transport, listener.handle);
		const linger immediate_reset{1, 0};
		CHECK(setsockopt(client, SOL_SOCKET, SO_LINGER,
		    reinterpret_cast<const char *>(&immediate_reset), sizeof(immediate_reset)) == 0);
		CHECK(closesocket(client) == 0);
		const auto deadline = Clock::now() + std::chrono::seconds(3);
		for(;;)
		{
			std::uint8_t byte = 0;
			const IoResult result = transport.readSome(connection, &byte, 1);
			CHECK(result.size == 0);
			if(result.status == IoStatus::Fatal)
			{
				CHECK(result.native_error == WSAECONNRESET || result.native_error == WSAECONNABORTED);
				break;
			}

			CHECK(result.status == IoStatus::WouldBlock || result.status == IoStatus::Interrupted);
			waitFor(transport, connection, true, deadline);
		}

		closeOwned(transport, connection);
		closeOwned(transport, listener.handle);
		CHECK(transport.ownedHandles() == 0);
		CHECK(transport.stop().status == Status::Ok);
	}

	void restartCycles()
	{
		WindowsTransport transport;
		for(unsigned cycle = 0; cycle < 100; ++cycle)
		{
			CHECK(transport.start().status == Status::Ok);
			auto listener = transport.openListener("127.0.0.1", 0, 1);
			CHECK(listener.status == Status::Ok);
			const SOCKET client = connectClient(listener.port);
			Handle connection = acceptClient(transport, listener.handle);
			const std::uint8_t input = static_cast<std::uint8_t>(cycle);
			sendClient(client, &input, 1);
			std::uint8_t output = 0;
			receiveTransport(transport, connection, &output, 1);
			CHECK(output == input);
			writeTransport(transport, connection, &output, 1);
			receiveClient(client, &output, 1);
			CHECK(output == input);
			CHECK(closesocket(client) == 0);
			closeOwned(transport, connection);
			closeOwned(transport, listener.handle);
			CHECK(transport.ownedHandles() == 0);
			CHECK(transport.stop().status == Status::Ok);
		}
	}

} // namespace

int main()
{
	// The test peer owns a separate Winsock reference from each tested adapter.
	WSADATA data{};
	CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
	lifecycleAndValidation();
	binaryHalfClose();
	independentOwners();
	peerReset();
	restartCycles();
	CHECK(WSACleanup() == 0);
	std::printf("TCP_WINDOWS_PASS checks=%u restart_cycles=100 transport_bytes=%zu wait_sets_bytes=%zu\n",
	    checked.load(), sizeof(WindowsTransport), sizeof(fd_set) * 3);
	return 0;
}
