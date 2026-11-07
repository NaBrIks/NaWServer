// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <NaWServer/websocket.h>
#include "support/tcp_scripted_transport.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace sockets = NaWServer::test::host;

namespace
{

	using namespace NaWServer;
	using namespace NaWServer::tcp;
	using Clock = std::chrono::steady_clock;
	std::atomic<unsigned> checks{0};

	void check(bool value, int line)
	{
		checks.fetch_add(1);
		if(!value)
		{
			std::printf("TCP_BACKEND_LIFECYCLE_FAIL line=%d native=%d\n", line, sockets::lastError());
			std::fflush(stdout);
			std::_Exit(1);
		}
	}

#define CHECK(value) check((value), __LINE__)

	sockets::Handle connectClient(std::uint16_t port)
	{
		const sockets::Handle client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(client != sockets::invalid);
		CHECK(sockets::setTimeouts(client, 3000));
		sockaddr_in endpoint{};
		endpoint.sin_family = AF_INET;
		endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		endpoint.sin_port = htons(port);
		CHECK(connect(client, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) == 0);
		return client;
	}

	void sendRequest(sockets::Handle client, const char *path)
	{
		const std::string request = std::string("GET ") + path + " HTTP/1.1\r\nHost: lifecycle\r\n\r\n";
		std::size_t offset = 0;
		while(offset < request.size())
		{
			const int sent = sockets::sendSome(client, request.data() + offset,
			        static_cast<int>(request.size() - offset), 0);
			CHECK(sent > 0);
			offset += static_cast<std::size_t>(sent);
		}
		CHECK(shutdown(client, sockets::shutdown_send) == 0);
	}

	std::string receiveResponse(sockets::Handle client, bool allow_reset = false)
	{
		std::string response;
		char buffer[512];
		for(;;)
		{
			const int received = sockets::receiveSome(client, buffer, sizeof(buffer), 0);
			if(received > 0)
			{
				response.append(buffer, static_cast<std::size_t>(received));
				continue;
			}

			if(received == sockets::error)
				CHECK(allow_reset && sockets::peerReset(sockets::lastError()));

			break;
		}
		CHECK(sockets::closeSocket(client) == 0);
		return response;
	}

	std::string request(std::uint16_t port, const char *path)
	{
		const sockets::Handle client = connectClient(port);
		sendRequest(client, path);
		return receiveResponse(client);
	}

	void refusedConnection(std::uint16_t port)
	{
		const sockets::Handle client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(client != sockets::invalid);
		sockaddr_in endpoint{};
		endpoint.sin_family = AF_INET;
		endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		endpoint.sin_port = htons(port);
		CHECK(connect(client, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) == sockets::error);
		CHECK(sockets::lastError() == sockets::refused_error);
		CHECK(sockets::closeSocket(client) == 0);
	}

	struct Endpoint
	{
		std::atomic<unsigned> calls{0};

		SendResult operator()(Response &response)
		{
			calls.fetch_add(1);
			return response.text(200, "lifecycle-alive");
		}

	};

	void beginEndRestart()
	{
		sockets::TcpTransport transport;
		CHECK(transport.start().status == Status::Ok);
		TcpOptions options;
		options.port = 0;
		TcpHttpBackend backend(transport, options);
		Endpoint endpoint;
		Route route("/health", HttpMethod::Get, EndpointRef(endpoint));
		HandlerBase *const routes[]{&route};
		HttpServer server(backend, routes);
		CHECK(!server.running() && !server.cleanupRequired() && backend.port() == 0);
		CHECK(server.end() == HttpError::Ok);
		for(unsigned cycle = 0; cycle < 10; ++cycle)
		{
			CHECK(server.begin() == HttpError::Ok);
			CHECK(server.running() && !server.cleanupRequired() && backend.port() != 0);
			CHECK(server.begin() == HttpError::Ok);
			CHECK(backend.start(server, {}) == HttpError::InvalidState);
			const auto response = request(backend.port(), "/health");
			CHECK(response.find("HTTP/1.1 200 ") == 0);
			CHECK(response.substr(response.find("\r\n\r\n") + 4) == "lifecycle-alive");
			CHECK(endpoint.calls.load() == cycle + 1);
			const auto old_port = backend.port();
			CHECK(server.end() == HttpError::Ok);
			CHECK(!server.running() && !server.cleanupRequired() && backend.port() == 0);
			CHECK(transport.ownedHandles() == 0);
			CHECK(server.end() == HttpError::Ok);
			if(cycle == 9)
				refusedConnection(old_port);

			CHECK(endpoint.calls.load() == cycle + 1);
		}
		CHECK(transport.stop().status == Status::Ok);
	}

	void invalidOptionsAndLimits()
	{
		sockets::TcpTransport transport;
		CHECK(transport.start().status == Status::Ok);
		Endpoint endpoint;
		Route route("/health", HttpMethod::Get, EndpointRef(endpoint));
		HandlerBase *const routes[]{&route};
		for(unsigned invalid = 0; invalid < 10; ++invalid)
		{
			TcpOptions options;
			options.port = 0;
			switch(invalid)
			{
			case 0: options.bind_address = nullptr; break;
			case 1: options.bind_address = ""; break;
			case 2: options.bind_address = "127.0.0.123456789"; break;
			case 3: options.bind_address = "localhost"; break;
			case 4: options.backlog = 0; break;
			case 5: options.no_progress_timeout_ms = 0; break;
			case 6: options.wait_ceiling_ms = 0; break;
			case 7: options.stop_timeout_ms = 0; break;
			case 8: options.close_timeout_ms = 0; break;
			case 9: options.close_drain_bytes = 0; break;
			}

			TcpHttpBackend backend(transport, options);
			HttpServer server(backend, routes);
			CHECK(server.begin() == HttpError::InvalidArgument);
			CHECK(!server.running() && !server.cleanupRequired() && backend.port() == 0);
			CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);
		}

		TcpOptions options;
		options.port = 0;
		TcpHttpBackend backend(transport, options);
		HttpServer handler(backend, routes);
		for(unsigned invalid = 0; invalid < 9; ++invalid)
		{
			HttpLimits limits;
			switch(invalid)
			{
			case 0: limits.max_body_size = 0; break;
			case 1: limits.max_header_bytes = 0; break;
			case 2: limits.max_headers = 0; break;
			case 3: limits.max_target_size = 0; break;
			case 4: limits.max_connections = 0; break;
			case 5: limits.exchange_timeout_ms = 0; break;
			case 6: limits.max_headers = std::numeric_limits<std::size_t>::max(); break;
			case 7: limits.max_header_bytes = std::numeric_limits<std::size_t>::max(); break;
			case 8: limits.max_body_size = std::numeric_limits<std::size_t>::max(); break;
			}

			CHECK(backend.start(handler, limits) == HttpError::InvalidArgument);
			CHECK(backend.stop() == HttpError::Ok && backend.port() == 0 && transport.ownedHandles() == 0);
		}

		CHECK(endpoint.calls.load() == 0);
		CHECK(transport.stop().status == Status::Ok);
	}

	void bindConflictAndAddressCopy()
	{
		sockets::TcpTransport transport;
		CHECK(transport.start().status == Status::Ok);
		auto occupied = transport.openListener("127.0.0.1", 0, 1);
		CHECK(occupied.status == Status::Ok);
		TcpOptions options;
		options.port = occupied.port;
		TcpHttpBackend backend(transport, options);
		Endpoint endpoint;
		Route route("/health", HttpMethod::Get, EndpointRef(endpoint));
		HandlerBase *const routes[]{&route};
		HttpServer server(backend, routes);
		CHECK(server.begin() == HttpError::TransportError);
		CHECK(!server.running() && !server.cleanupRequired());
		CHECK(transport.ownedHandles() == 1 && backend.port() == 0);
		CHECK(server.end() == HttpError::Ok);
		CHECK(transport.close(occupied.handle).released);
		CHECK(server.begin() == HttpError::Ok);
		CHECK(request(backend.port(), "/health").find("HTTP/1.1 200 ") == 0);
		CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);

		char address[] = "127.0.0.1";
		options.port = 0;
		options.bind_address = address;
		TcpHttpBackend copied_address(transport, options);
		std::fill(std::begin(address), std::end(address) - 1, 'x');
		HttpServer copied_server(copied_address, routes);
		CHECK(copied_server.begin() == HttpError::Ok);
		CHECK(request(copied_address.port(), "/health").find("HTTP/1.1 200 ") == 0);
		CHECK(copied_server.end() == HttpError::Ok && transport.ownedHandles() == 0);
		CHECK(transport.stop().status == Status::Ok);
	}

	void unsupportedWebSocketBeforeListener()
	{
		// With no Winsock startup in this adapter, any attempted listener would
		// fail. Unsupported must be determined before entering the TCP boundary.
		sockets::TcpTransport transport;
		TcpOptions options;
		options.port = 0;
		TcpHttpBackend backend(transport, options);
		class CustomWebSocket final : public HandlerBase
		{
		public:
			const char *websocket_uri() const override
			{
				return "/custom-ws";
			}

		};
		CustomWebSocket websocket;
		HandlerBase *const routes[]{&websocket};
		HttpServer server(backend, routes);
		CHECK(server.begin() == HttpError::Unsupported);
		CHECK(!server.running() && !server.cleanupRequired() && backend.port() == 0);
		CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);
	}

	void invalidWebSocketStorageBeforeListener()
	{
		// There is no platform startup: validation must finish before socket I/O.
		sockets::TcpTransport transport;
		WebSocketEndpoint websocket("/ws");
		HandlerBase *const routes[]{&websocket};
		for(unsigned invalid = 0; invalid < 5; ++invalid)
		{
			TcpOptions options;
			switch(invalid)
			{
			case 0: options.websocket_tx_blocks = 0; break;
			case 1: options.websocket_tx_block_size = 0; break;
			case 2: options.websocket_tx_block_size = 4095; break;
			case 3: options.websocket_tx_block_size = std::numeric_limits<std::size_t>::max(); break;
			case 4: options.max_websocket_clients = 0; break;
			}

			TcpHttpBackend backend(transport, options);
			HttpServer server(backend, routes);
			CHECK(server.begin() == HttpError::InvalidArgument);
			CHECK(!server.running() && !server.cleanupRequired() && backend.port() == 0);
			CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);
		}

		// HTTP-only composition has no pool and ignores WS-only storage options.
		CHECK(transport.start().status == Status::Ok);
		TcpOptions options;
		options.port = 0;
		options.websocket_tx_blocks = 0;
		options.websocket_tx_block_size = 0;
		TcpHttpBackend backend(transport, options);
		Endpoint endpoint;
		Route route("/health", HttpMethod::Get, EndpointRef(endpoint));
		HandlerBase *const http_routes[]{&route};
		HttpServer server(backend, http_routes);
		CHECK(server.begin() == HttpError::Ok);
		CHECK(request(backend.port(), "/health").find("HTTP/1.1 200 ") == 0);
		CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);
		CHECK(transport.stop().status == Status::Ok);
	}

	void failedStartupCleanup()
	{
		tcp::test::ScriptedTransport transport;
		tcp::test::Step opening;
		opening.operation = tcp::test::Operation::OpenListener;
		opening.status = Status::Fatal;
		opening.native_error = 123;
		opening.handle = {0, true};
		opening.cleanup = CloseResult{Status::WouldBlock, 456, false};
		CHECK(transport.append(opening));
		tcp::test::Step cleanup;
		cleanup.operation = tcp::test::Operation::Close;
		cleanup.expected_handle = {0, true};
		CHECK(transport.append(cleanup));
		TcpOptions options;
		options.port = 0;
		TcpHttpBackend backend(transport, options);
		Endpoint endpoint;
		Route route("/health", HttpMethod::Get, EndpointRef(endpoint));
		HandlerBase *const routes[]{&route};
		HttpServer server(backend, routes);
		CHECK(server.begin() == HttpError::CleanupRequired);
		CHECK(!server.running() && server.cleanupRequired() && backend.port() == 0);
		CHECK(transport.owned() == 1 && transport.calls(tcp::test::Operation::OpenListener) == 1);
		CHECK(server.begin() == HttpError::CleanupRequired);
		CHECK(transport.calls(tcp::test::Operation::OpenListener) == 1);
		CHECK(server.end() == HttpError::Ok);
		CHECK(!server.running() && !server.cleanupRequired());
		CHECK(transport.owned() == 0 && transport.released() == 1 && transport.remaining() == 0);
		CHECK(transport.failures() == 0 && endpoint.calls.load() == 0);
		CHECK(transport.calls(tcp::test::Operation::Close) == 1);
		CHECK(server.end() == HttpError::Ok && transport.calls(tcp::test::Operation::Close) == 1);

		opening.handle = {};
		opening.cleanup = std::nullopt;
		CHECK(transport.append(opening));
		CHECK(server.begin() == HttpError::TransportError);
		CHECK(!server.running() && !server.cleanupRequired());
		CHECK(transport.owned() == 0 && transport.remaining() == 0 && transport.failures() == 0);
		CHECK(server.end() == HttpError::Ok);

		// A valid failed setup with no cleanup result has not tried native close.
		// The backend must make its one first attempt, rather than pinning forever.
		opening.handle = {21, true};
		CHECK(transport.append(opening));
		cleanup.expected_handle = opening.handle;
		CHECK(transport.append(cleanup));
		const auto closes_before = transport.calls(tcp::test::Operation::Close);
		CHECK(server.begin() == HttpError::CleanupRequired);
		CHECK(!server.running() && server.cleanupRequired() && transport.owned() == 1);
		CHECK(transport.calls(tcp::test::Operation::Close) == closes_before);
		CHECK(server.end() == HttpError::Ok);
		CHECK(!server.running() && !server.cleanupRequired() && transport.owned() == 0);
		CHECK(transport.calls(tcp::test::Operation::Close) == closes_before + 1);
		CHECK(transport.remaining() == 0 && transport.failures() == 0 && endpoint.calls.load() == 0);
		CHECK(server.end() == HttpError::Ok);
		CHECK(transport.calls(tcp::test::Operation::Close) == closes_before + 1);
	}

	struct CallbackStop
	{
		HttpServer *server = nullptr;
		std::atomic<HttpError> stopped{HttpError::Ok};
		std::atomic<unsigned> calls{0};

		SendResult operator()(Response &response)
		{
			calls.fetch_add(1);
			stopped.store(server->end());
			return response.text(200, "callback-retained");
		}

	};

	void stopFromCallback()
	{
		sockets::TcpTransport transport;
		CHECK(transport.start().status == Status::Ok);
		TcpOptions options;
		options.port = 0;
		TcpHttpBackend backend(transport, options);
		CallbackStop endpoint;
		Route route("/stop", HttpMethod::Get, EndpointRef(endpoint));
		HandlerBase *const routes[]{&route};
		HttpServer server(backend, routes);
		endpoint.server = &server;
		CHECK(server.begin() == HttpError::Ok);
		CHECK(request(backend.port(), "/stop").find("callback-retained") != std::string::npos);
		CHECK(endpoint.stopped.load() == HttpError::InvalidState && endpoint.calls.load() == 1);
		CHECK(server.running() && !server.cleanupRequired());
		CHECK(request(backend.port(), "/stop").find("HTTP/1.1 200 ") == 0);
		CHECK(endpoint.calls.load() == 2);
		CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);
		CHECK(transport.stop().status == Status::Ok);
	}

	struct BlockingEndpoint
	{
		std::mutex mutex;
		std::condition_variable changed;
		bool entered = false;
		bool released = false;
		std::atomic<unsigned> calls{0};
		std::atomic<unsigned> returned{0};

		SendResult operator()(Response &response)
		{
			calls.fetch_add(1);
			{
				std::unique_lock<std::mutex> lock(mutex);
				entered = true;
				changed.notify_all();
				CHECK(changed.wait_for(lock, std::chrono::seconds(3), [this] {
					return released;
				}));
			}
			returned.fetch_add(1);
			return response.text(200, "released");
		}

		void awaitEntry()
		{
			std::unique_lock<std::mutex> lock(mutex);
			CHECK(changed.wait_for(lock, std::chrono::seconds(3), [this] {
				return entered;
			}));
		}

		void release()
		{
			std::lock_guard<std::mutex> lock(mutex);
			released = true;
			changed.notify_all();
		}

	};

	// Inject only a native wait/listener fault; all listener reopen and HTTP I/O
	// still use real sockets. The sampler runs concurrently through stop/start.
	class RecoveringTransport final : public Transport
	{
	public:
		sockets::TcpTransport native;
		std::atomic<unsigned> fault{0};
		ListenerResult openListener(const char *address, std::uint16_t port, int backlog) override
		{
			return native.openListener(address, port, backlog);
		}

		AcceptResult accept(Handle handle) override
		{
			return native.accept(handle);
		}

		IoResult readSome(Handle handle, std::uint8_t *bytes, std::size_t size) override
		{
			return native.readSome(handle, bytes, size);
		}

		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override
		{
			return native.writeSome(handle, bytes, size);
		}

		WaitResult wait(const Interest *interests, Ready *ready, std::size_t count, std::uint32_t timeout) override
		{
			const auto injected = fault.exchange(0);
			if(!injected) return native.wait(interests, ready, count, timeout);

			std::fill_n(ready, count, Ready{});
			if(injected == 1) return {WaitStatus::Fatal, 0, 8101};

			ready[0] = {false, false, true, 8102};
			return {WaitStatus::Ready, 1, 0};
		}

		OperationResult shutdownWrite(Handle handle) override
		{
			return native.shutdownWrite(handle);
		}

		CloseResult close(Handle &handle) override
		{
			return native.close(handle);
		}

	};

	void realListenerRecovery(bool managed)
	{
		RecoveringTransport transport;
		CHECK(transport.native.start().status == Status::Ok);
		TcpOptions options;
		options.port = 0;
		options.wait_ceiling_ms = 5;
		options.recovery.retry_delay_ms = 10;
		options.http_memory.enabled = managed;
		TcpHttpBackend backend(transport, options);
		Endpoint endpoint;
		Route route("/health", HttpMethod::Get, EndpointRef(endpoint));
		HandlerBase *const routes[]{&route};
		HttpServer server(backend, routes);
		std::atomic<bool> sampling{true};
		std::atomic<unsigned> samples{0};
		std::thread sampler([&] {
		    while(sampling.load())
			{
				const auto value = backend.diagnostics();
				CHECK(value.recoveries <= value.retries);
				if(value.state == TcpRuntimeState::Listening) CHECK(value.port != 0);

				++samples;
				std::this_thread::yield();
			}
			});
		CHECK(server.begin() == HttpError::Ok);
		const auto port = backend.port();
		CHECK(request(port, "/health").find("HTTP/1.1 200 ") == 0);
		for(unsigned fault = 1; fault <= 2; ++fault)
		{
			transport.fault.store(fault);
			const auto deadline = Clock::now() + std::chrono::seconds(3);
			TcpDiagnostics recovered;
			do
			{
				recovered = backend.diagnostics();
				if(recovered.recoveries == fault && recovered.state == TcpRuntimeState::Listening) break;

				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while(Clock::now() < deadline);
			CHECK(recovered.state == TcpRuntimeState::Listening && recovered.recoveries == fault);
			CHECK(recovered.port == port && recovered.retries == fault && recovered.errors == fault);
			CHECK(recovered.last_failure == (fault == 1 ? TcpFailure::Wait : TcpFailure::Listener));
			CHECK(recovered.native_error == (fault == 1 ? 8101 : 8102));
			CHECK(request(port, "/health").find("HTTP/1.1 200 ") == 0);
		}
		CHECK(server.end() == HttpError::Ok && transport.native.ownedHandles() == 0);
		CHECK(server.begin() == HttpError::Ok && backend.diagnostics().errors == 0);
		CHECK(request(backend.port(), "/health").find("HTTP/1.1 200 ") == 0);
		CHECK(server.end() == HttpError::Ok && transport.native.ownedHandles() == 0);
		sampling.store(false);
		sampler.join();
		CHECK(samples > 0 && endpoint.calls == 4);
		CHECK(transport.native.stop().status == Status::Ok);
		std::printf("TCP_REAL_LISTENER_RECOVERY managed=%u samples=%u\n", managed ? 1 : 0, samples.load());
	}

	void boundedStopAndRetry()
	{
		sockets::TcpTransport transport;
		CHECK(transport.start().status == Status::Ok);
		TcpOptions options;
		options.port = 0;
		options.stop_timeout_ms = 20;
		options.wait_ceiling_ms = 1;
		TcpHttpBackend backend(transport, options);
		BlockingEndpoint endpoint;
		Route route("/blocked", HttpMethod::Get, EndpointRef(endpoint));
		HandlerBase *const routes[]{&route};
		HttpServer server(backend, routes);
		CHECK(server.begin() == HttpError::Ok);
		const auto old_port = backend.port();
		const sockets::Handle client = connectClient(old_port);
		sendRequest(client, "/blocked");
		endpoint.awaitEntry();
		// The second request is queued in the OS backlog. Stop must not dispatch
		// it after the current controlled callback is released.
		const sockets::Handle queued_client = connectClient(old_port);
		sendRequest(queued_client, "/blocked");
		const auto started = Clock::now();
		CHECK(server.end() == HttpError::CleanupRequired);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
		CHECK(elapsed < 1000);
		CHECK(endpoint.calls.load() == 1 && endpoint.returned.load() == 0);
		// Preserve the existing HttpServer state contract on an unsuccessful end.
		CHECK(server.running() && !server.cleanupRequired() && backend.port() == old_port);
		CHECK(transport.ownedHandles() == 2 && transport.stop().status == Status::InvalidState);
		endpoint.release();
		CHECK(receiveResponse(client, true).empty());
		CHECK(receiveResponse(queued_client, true).empty());
		CHECK(server.end() == HttpError::Ok);
		CHECK(!server.running() && !server.cleanupRequired() && backend.port() == 0);
		CHECK(endpoint.calls.load() == 1 && endpoint.returned.load() == 1);
		CHECK(transport.ownedHandles() == 0);
		refusedConnection(old_port);
		CHECK(endpoint.calls.load() == 1);
		CHECK(server.begin() == HttpError::Ok);
		CHECK(request(backend.port(), "/blocked").find("released") != std::string::npos);
		CHECK(endpoint.calls.load() == 2 && endpoint.returned.load() == 2);
		CHECK(server.end() == HttpError::Ok && transport.ownedHandles() == 0);
		CHECK(transport.stop().status == Status::Ok);
		std::printf("TCP_BACKEND_LIFECYCLE stop_timeout_ms=20 observed_ms=%lld\n", static_cast<long long>(elapsed));
	}

} // namespace

int main()
{
	// Test peers retain their own reference; backend transports remain independent.
	CHECK(sockets::initialize() == 0);
	beginEndRestart();
	invalidOptionsAndLimits();
	bindConflictAndAddressCopy();
	unsupportedWebSocketBeforeListener();
	invalidWebSocketStorageBeforeListener();
	failedStartupCleanup();
	stopFromCallback();
	boundedStopAndRetry();
	realListenerRecovery(false);
	realListenerRecovery(true);
	CHECK(sockets::cleanup() == 0);
	std::printf("TCP_BACKEND_LIFECYCLE_PASS checks=%u restart_cycles=10\n", checks.load());
	return 0;
}
