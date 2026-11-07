// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/tcp.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include "backends/tcp/platform/worker.h"
#include "support/tcp_scripted_transport.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

using namespace NaWServer;
using namespace NaWServer::tcp;
using namespace NaWServer::tcp::test;

namespace
{
	unsigned checks = 0;
	std::uint64_t milliseconds = 0, stop_at = UINT64_MAX;
	std::uint32_t largest_sleep = 0;
	bool executing = false;
	void *current_context = nullptr;
	TcpHttpBackend *current_backend = nullptr;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_LISTENER_RECOVERY_FAIL line=%d time=%llu\n", line,
			    static_cast<unsigned long long>(milliseconds));
			std::fflush(stdout);
			std::_Exit(1);
		}
	}

#define CHECK(value) check((value), __LINE__)

	void requestStop()
	{
		// Model the owner requesting stop while the paused worker is still live.
		// join refuses until entry returns; it cannot destroy an executing runtime.
		auto *saved_context = current_context;
		current_context = nullptr;
		CHECK(current_backend->stop() == HttpError::CleanupRequired);
		current_context = saved_context;
	}

}

namespace NaWServer::tcp::detail
{
	struct Worker { void (*entry)(void *); void *context; bool owned; };
	Worker test_worker{};

	Worker *startTcpWorker(void (*entry)(void *), void *context, WorkerRole) noexcept
	{
		CHECK(!test_worker.owned);
		test_worker = {entry, context, true};
		return &test_worker;
	}

	bool isTcpWorkerThread(const Worker *worker) noexcept
	{
		return worker && current_context == worker->context;
	}

	bool isTcpWorkerContext(const void *context) noexcept
	{
		return context == current_context;
	}

	bool joinTcpWorker(Worker *worker, std::uint32_t) noexcept
	{
		if(executing) return false;

		worker->owned = false;
		return true;
	}

	std::uint64_t tcpMilliseconds() noexcept
	{
		return milliseconds;
	}

	void sleepTcpWorker(std::uint32_t duration) noexcept
	{
		CHECK(duration > 0 && duration <= 20);
		largest_sleep = std::max(largest_sleep, duration);
		milliseconds += duration;
		if(milliseconds >= stop_at) requestStop();
	}

}

namespace
{
	using namespace NaWServer::tcp::detail;

	struct ControlledTransport final : Transport
	{
		ScriptedTransport script;
		unsigned stop_on_wait = 0;
		Handle stop_on_close;
		std::array<std::uint64_t, 8> open_times{}, close_times{};
		std::array<std::uint16_t, 8> ports{};
		TcpDiagnostics before_stop;

		ListenerResult openListener(const char *address, std::uint16_t port, int backlog) override
		{
			const auto index = script.calls(Operation::OpenListener);
			CHECK(index < open_times.size());
			open_times[index] = milliseconds;
			ports[index] = port;
			return script.openListener(address, port, backlog);
		}

		AcceptResult accept(Handle handle) override
		{
			return script.accept(handle);
		}

		IoResult readSome(Handle handle, std::uint8_t *bytes, std::size_t size) override
		{
			return script.readSome(handle, bytes, size);
		}

		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override
		{
			return script.writeSome(handle, bytes, size);
		}

		WaitResult wait(const Interest *interests, Ready *ready, std::size_t count, std::uint32_t timeout) override
		{
			const auto result = script.wait(interests, ready, count, timeout);
			if(stop_on_wait && script.calls(Operation::Wait) == stop_on_wait)
			{
				before_stop = current_backend->diagnostics();
				requestStop();
			}

			return result;
		}

		OperationResult shutdownWrite(Handle handle) override
		{
			return script.shutdownWrite(handle);
		}

		CloseResult close(Handle &handle) override
		{
			const auto index = script.calls(Operation::Close);
			CHECK(index < close_times.size());
			close_times[index] = milliseconds;
			const bool stop = stop_on_close.valid && stop_on_close.value == handle.value;
			const auto result = script.close(handle);
			if(stop)
			{
				before_stop = current_backend->diagnostics();
				requestStop();
			}

			return result;
		}

		void append(Step step)
		{
			CHECK(script.append(step));
		}

		void open(Status status = Status::Ok, Handle handle = {0, true},
		    std::optional<CloseResult> cleanup = {})
		{
			Step step;
			step.operation = Operation::OpenListener;
			step.status = status;
			step.native_error = status == Status::Ok ? 0 : 701;
			step.handle = handle;
			step.port = 31234;
			step.cleanup = cleanup;
			append(step);
		}

		void closeStep(Handle handle = {0, true}, Status status = Status::Ok, bool released = true)
		{
			Step step;
			step.operation = Operation::Close;
			step.expected_handle = handle;
			step.status = status;
			step.native_error = status == Status::Ok ? 0 : 702;
			step.released = released;
			append(step);
		}

		void waitStep(WaitStatus status, Ready ready = {}, int error = 0)
		{
			Step step;
			step.operation = Operation::Wait;
			step.wait_status = status;
			step.readiness[0] = ready;
			step.native_error = error;
			append(step);
		}

		void fault(TcpFailure failure)
		{
			if(failure == TcpFailure::Wait) waitStep(WaitStatus::Fatal, {}, 703);
			else if(failure == TcpFailure::Listener) waitStep(WaitStatus::Ready, {false, false, true, 704});
			else
			{
				waitStep(WaitStatus::Ready, {true, false, false});
				Step step;
				step.operation = Operation::Accept;
				step.status = Status::Fatal;
				step.native_error = 705;
				append(step);
			}
		}

	};

	SendResult health(Response &response)
	{
		return response.text(200, "ready");
	}

	constexpr std::string_view request = "GET /health HTTP/1.1\r\nHost: test\r\n\r\n";
	constexpr std::string_view reply = "HTTP/1.1 200 Response\r\nConnection: close\r\nContent-Length: 5\r\n"
	    "Content-Type: text/plain; charset=utf-8\r\n\r\nready";

	void appendHttp(ControlledTransport &transport)
	{
		transport.waitStep(WaitStatus::Ready, {true, false, false});
		Step step;
		step.operation = Operation::Accept;
		step.handle = {1, true};
		transport.append(step);
		step = {};
		step.operation = Operation::Read;
		step.expected_handle = {1, true};
		step.bytes = reinterpret_cast<const std::uint8_t *>(request.data());
		step.size = request.size();
		transport.append(step);
		step = {};
		step.operation = Operation::Write;
		step.expected_handle = {1, true};
		step.bytes = reinterpret_cast<const std::uint8_t *>(reply.data());
		step.size = reply.size();
		transport.append(step);
		step = {};
		step.operation = Operation::Read;
		step.expected_handle = {1, true};
		step.io_status = IoStatus::PeerClosed;
		transport.append(step);
		transport.closeStep({1, true});
		transport.stop_on_close = {1, true};
	}

	struct Application
	{
		ControlledTransport transport;
		TcpOptions options;
		TcpHttpBackend backend;
		Route route{"/health", HttpMethod::Get, health};
		HandlerBase *routes[1]{&route};
		HttpServer server{backend, routes};

		explicit Application(bool managed = false, bool enabled = true, std::uint32_t delay = 13)
		  : options(makeOptions(managed, enabled, delay)), backend(transport, options)
		{
			milliseconds = 0;
			stop_at = UINT64_MAX;
			largest_sleep = 0;
			current_backend = &backend;
		}

		static TcpOptions makeOptions(bool managed, bool enabled, std::uint32_t delay)
		{
			TcpOptions options;
			options.port = 0;
			options.wait_ceiling_ms = 3;
			options.recovery = {enabled, delay};
			options.http_memory.enabled = managed;
			return options;
		}

		void run()
		{
			CHECK(server.begin() == HttpError::Ok);
			CHECK(backend.diagnostics().state == TcpRuntimeState::Listening);
			executing = true;
			current_context = test_worker.context;
			test_worker.entry(test_worker.context);
			current_context = nullptr;
			executing = false;
		}

		void finish()
		{
			CHECK(server.end() == HttpError::Ok);
			CHECK(!transport.script.remaining() && !transport.script.failures() && !transport.script.owned());
			CHECK(backend.diagnostics().state == TcpRuntimeState::Stopped && backend.port() == 0);
		}

	};

	void recoverAndServe(TcpFailure failure)
	{
		Application app;
		auto &transport = app.transport;
		transport.open();
		transport.fault(failure);
		transport.closeStep();
		transport.open(Status::Fatal, {});
		transport.open();
		appendHttp(transport);
		transport.closeStep();
		app.run();
		const auto result = transport.before_stop;
		CHECK(result.state == TcpRuntimeState::Listening && result.port == 31234);
		CHECK(result.last_failure == TcpFailure::Listen && result.native_error == 701);
		CHECK(result.errors == 2 && result.retries == 2 && result.recoveries == 1);
		CHECK(transport.ports[0] == 0 && transport.ports[1] == 31234 && transport.ports[2] == 31234);
		CHECK(transport.open_times[1] >= 13 && transport.open_times[2] - transport.open_times[1] >= 13);
		CHECK(std::string_view(reinterpret_cast<const char *>(transport.script.capturedBytes()),
		    transport.script.capturedSize()) == reply);
		app.finish();
		CHECK(app.backend.diagnostics().errors == result.errors);
	}

	void retainedListener(bool rollback)
	{
		Application app(true);
		auto &transport = app.transport;
		transport.open();
		transport.fault(TcpFailure::Wait);
		if(rollback)
		{
			transport.closeStep();
			transport.open(Status::Fatal, {0, true}, CloseResult{Status::WouldBlock, 706, false});
		}
		else transport.closeStep({0, true}, Status::WouldBlock, false);

		transport.closeStep();
		transport.open();
		transport.waitStep(WaitStatus::Timeout);
		transport.stop_on_wait = 2;
		transport.closeStep();
		app.run();
		CHECK(transport.before_stop.recoveries == 1 && transport.before_stop.state == TcpRuntimeState::Listening);
		CHECK(transport.close_times[1] - transport.close_times[0] >= 13);
		if(rollback) CHECK(transport.close_times[1] - transport.open_times[1] >= 13);

		app.finish();
	}

	void stopDuringBackoff()
	{
		Application app(false, true, UINT32_MAX);
		app.transport.open();
		app.transport.fault(TcpFailure::Wait);
		app.transport.closeStep();
		stop_at = 1;
		app.run();
		CHECK(milliseconds <= 20 && largest_sleep > 0);
		CHECK(app.backend.diagnostics().retries == 0);
		app.finish();
	}

	void orphanCleanup()
	{
		Application app;
		app.transport.open();
		app.transport.waitStep(WaitStatus::Ready, {true, false, false});
		Step step;
		step.operation = Operation::Accept;
		step.status = Status::Fatal;
		step.native_error = 707;
		step.handle = {1, true};
		step.cleanup = CloseResult{Status::WouldBlock, 708, false};
		app.transport.append(step);
		app.transport.closeStep();
		app.run();
		const auto result = app.backend.diagnostics();
		CHECK(result.state == TcpRuntimeState::CleanupRequired && result.last_failure == TcpFailure::Accept);
		CHECK(result.native_error == 707 && result.retries == 0);
		CHECK(app.transport.script.owned() == 1 && app.transport.script.calls(Operation::Close) == 1);
		app.transport.closeStep({1, true});
		app.finish();
	}

	void disabledRecovery()
	{
		Application app(false, false, 0);
		app.transport.open();
		app.transport.fault(TcpFailure::Wait);
		app.transport.closeStep();
		app.run();
		CHECK(app.backend.diagnostics().state == TcpRuntimeState::Failed);
		CHECK(app.backend.diagnostics().last_failure == TcpFailure::Wait &&
		    app.backend.diagnostics().native_error == 703);
		CHECK(app.backend.diagnostics().retries == 0 && milliseconds == 0);
		app.finish();
	}

	void invalidDelay()
	{
		Application app(false, true, 0);
		CHECK(app.server.begin() == HttpError::InvalidArgument);
		CHECK(app.transport.script.calls(Operation::OpenListener) == 0);
		app.finish();
	}

	void transientErrors()
	{
		Application app;
		app.transport.open();
		app.transport.waitStep(WaitStatus::Timeout);
		app.transport.waitStep(WaitStatus::Interrupted);
		for(const auto status : {Status::WouldBlock, Status::Interrupted})
		{
			app.transport.waitStep(WaitStatus::Ready, {true, false, false});
			Step step;
			step.operation = Operation::Accept;
			step.status = status;
			app.transport.append(step);
		}
		app.transport.waitStep(WaitStatus::Timeout);
		app.transport.stop_on_wait = 5;
		app.transport.closeStep();
		app.run();
		CHECK(app.transport.before_stop.state == TcpRuntimeState::Listening);
		CHECK(app.transport.before_stop.errors == 0 && app.transport.before_stop.retries == 0);
		app.finish();
	}

	void releasedCloseError()
	{
		Application app;
		app.transport.open();
		app.transport.fault(TcpFailure::Wait);
		app.transport.closeStep({0, true}, Status::Fatal, true);
		app.transport.open();
		app.transport.waitStep(WaitStatus::Timeout);
		app.transport.stop_on_wait = 2;
		app.transport.closeStep();
		app.run();
		CHECK(app.transport.before_stop.recoveries == 1 && app.transport.before_stop.retries == 1);
		CHECK(app.transport.before_stop.last_failure == TcpFailure::ListenerCleanup);
		CHECK(app.transport.before_stop.native_error == 702 && app.transport.before_stop.errors == 2);
		app.finish();
	}

	void initialFailure()
	{
		Application app;
		app.transport.open(Status::Fatal, {});
		CHECK(app.server.begin() == HttpError::TransportError);
		CHECK(app.backend.diagnostics().state == TcpRuntimeState::Failed);
		CHECK(app.backend.diagnostics().last_failure == TcpFailure::Listen &&
		    app.backend.diagnostics().native_error == 701);
		CHECK(app.backend.diagnostics().errors == 1);
		app.transport.open();
		app.transport.closeStep();
		CHECK(app.server.begin() == HttpError::Ok);
		CHECK(app.backend.diagnostics().errors == 0);
		app.finish();
	}

	void fatalRetained(bool rollback)
	{
		Application app;
		app.transport.open();
		app.transport.fault(TcpFailure::Wait);
		if(rollback)
		{
			app.transport.closeStep();
			app.transport.open(Status::Fatal, {0, true}, CloseResult{Status::Fatal, 710, false});
		}
		else app.transport.closeStep({0, true}, Status::Fatal, false);

		app.run();
		CHECK(app.backend.diagnostics().state == TcpRuntimeState::CleanupRequired);
		CHECK(app.server.end() == HttpError::CleanupRequired);
		CHECK(app.server.end() == HttpError::CleanupRequired);
		CHECK(!app.transport.script.remaining() && !app.transport.script.failures());
		CHECK(app.transport.script.owned() == 1 && app.transport.script.calls(Operation::Close) == 1);
		// The ambiguous retained handle intentionally cannot be destroyed safely.
		// A separate CTest process owns this terminal case, as in the SSE tests.
		std::printf("TCP_LISTENER_FATAL_RETAINED_PASS checks=%u\n", checks);
		std::fflush(stdout);
		std::_Exit(0);
	}

}

int main(int argc, char **argv)
{
	if(argc == 2) fatalRetained(std::strcmp(argv[1], "--rollback-fatal") == 0);

	for(const auto failure : {TcpFailure::Wait, TcpFailure::Listener, TcpFailure::Accept})
		recoverAndServe(failure);

	retainedListener(false);
	retainedListener(true);
	stopDuringBackoff();
	orphanCleanup();
	disabledRecovery();
	invalidDelay();
	transientErrors();
	releasedCloseError();
	initialFailure();
	std::printf("TCP_LISTENER_RECOVERY_PASS checks=%u\n", checks);
}
