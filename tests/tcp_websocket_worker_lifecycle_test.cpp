// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "support/host_sockets.h"
#include "backends/tcp/platform/worker.h"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/http_server.h>
#include <NaWServer/websocket.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace
{
	unsigned checks = 0, starts = 0, fail_start = 0, websocket_starts = 0, live_workers = 0;
	bool fail_join = false;
	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("WS_WORKER_LIFECYCLE_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::_Exit(1);
		}
	}
#define CHECK(value) check((value), __LINE__)
}

// This executable replaces only private worker glue, so failures can be
// injected deterministically while the real TCP backend/socket owners run.
namespace NaWServer::tcp::detail
{
	struct Worker
	{
		std::thread native;
		std::atomic<bool> completed{false};
	};
	thread_local Worker *current_worker = nullptr;
	thread_local const void *current_context = nullptr;

	Worker *startTcpWorker(void (*entry)(void *), void *context, WorkerRole role) noexcept
	{
		++starts;
		if(role == WorkerRole::WebSocket) ++websocket_starts;

		if(starts == fail_start) return nullptr;

		auto *worker = new Worker;
		++live_workers;
		worker->native = std::thread([=]
		{
			current_worker = worker;
			current_context = context;
			entry(context);
			current_context = nullptr;
			current_worker = nullptr;
			worker->completed.store(true);
		});
		return worker;
	}

	bool isTcpWorkerThread(const Worker *worker) noexcept { return current_worker == worker; }
	bool isTcpWorkerContext(const void *context) noexcept { return current_worker && current_context == context; }
	std::uint64_t tcpMilliseconds() noexcept
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now().time_since_epoch()).count());
	}
	void sleepTcpWorker(std::uint32_t milliseconds) noexcept
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
	}
	bool joinTcpWorker(Worker *worker, std::uint32_t timeout) noexcept
	{
		if(fail_join) { fail_join = false; return false; }

		const auto deadline = tcpMilliseconds() + timeout;
		while(!worker->completed.load())
		{
			if(tcpMilliseconds() >= deadline) return false;

			sleepTcpWorker(1);
		}
		worker->native.join();
		delete worker;
		--live_workers;
		return true;
	}
}

int main()
{
	using namespace NaWServer;
	namespace sockets = NaWServer::test::host;
	CHECK(sockets::initialize() == 0);
	for(unsigned failure : {1u, 2u, 3u})
	{
		starts = websocket_starts = 0;
		fail_start = failure == 3 ? 2 : failure;
		fail_join = failure == 3;
		sockets::TcpTransport transport;
		CHECK(transport.start().status == tcp::Status::Ok);
		TcpOptions options;
		options.port = 0;
		options.wait_ceiling_ms = 1;
		TcpHttpBackend backend(transport, options);
		WebSocketEndpoint endpoint("/ws");
		HandlerBase *const routes[]{&endpoint};
		HttpServer server(backend, routes);
		const auto result = server.begin();
		CHECK(websocket_starts == 1);
		if(failure == 3)
		{
			CHECK(result == HttpError::CleanupRequired && live_workers == 1);
			CHECK(transport.ownedHandles() == 1);
		}
		else CHECK(result == HttpError::TransportError && live_workers == 0 && transport.ownedHandles() == 0);

		CHECK(server.end() == HttpError::Ok && live_workers == 0 && transport.ownedHandles() == 0);
		CHECK(transport.stop().status == tcp::Status::Ok);
	}
	{
		starts = websocket_starts = fail_start = 0;
		sockets::TcpTransport transport;
		CHECK(transport.start().status == tcp::Status::Ok);
		TcpOptions options;
		options.port = 0;
		TcpHttpBackend backend(transport, options);
		HandlerStaticUriText endpoint("/", "ok");
		HandlerBase *const routes[]{&endpoint};
		HttpServer server(backend, routes);
		CHECK(server.begin() == HttpError::Ok);
		CHECK(starts == 1 && websocket_starts == 0 && live_workers == 1);
		CHECK(server.end() == HttpError::Ok && live_workers == 0 && transport.ownedHandles() == 0);
		CHECK(transport.stop().status == tcp::Status::Ok);
	}
	CHECK(sockets::cleanup() == 0);
	std::printf("WS_WORKER_LIFECYCLE_PASS checks=%u\n", checks);
}
