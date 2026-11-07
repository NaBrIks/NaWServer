// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/tcp.h>
#include <NaWServer/detail/sse/queued_adapter.hpp>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/sse.h>
#include <NaWServer/websocket.h>
#include "backends/tcp/connection_registry.h"
#include "backends/tcp/platform/worker.h"
#include <NaWServer/detail/worker_signal.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#if defined(_WIN32)
#include <windows.h>
#else
#include <ucontext.h>
#endif

namespace
{

	unsigned checks = 0;
	std::size_t live_allocations = 0;
	struct AllocationPhase
	{
		std::size_t explicit_attempts = 0;
		std::size_t ordinary_allocations = 0;
		std::size_t ordinary_requested_bytes = 0;
		std::size_t requested_bytes = 0;
		std::array<std::size_t, 64> sizes{};
		std::ptrdiff_t fail_at = -1;
		bool injected = false;
		bool measuring = false;
	} phase;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_STORAGE_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

	void *allocate(std::size_t size)
	{
		if(void *storage = std::malloc(size ? size : 1))
		{
			++live_allocations;
			return storage;
		}

		std::abort();
	}

} // namespace

void *operator new(std::size_t size)
{
	if(phase.measuring)
	{
		++phase.ordinary_allocations;
		phase.ordinary_requested_bytes += size;
	}

	return allocate(size);
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	if(phase.measuring)
	{
		const auto index = phase.explicit_attempts++;
		if(index < phase.sizes.size()) phase.sizes[index] = size;

		phase.requested_bytes += size;
		if(phase.fail_at >= 0 && index == static_cast<std::size_t>(phase.fail_at))
		{
			phase.injected = true;
			return nullptr;
		}
	}

	return allocate(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept
{
	return ::operator new(size, tag);
}

void operator delete(void *storage) noexcept
{
	if(storage) --live_allocations;

	std::free(storage);
}

void operator delete[](void *storage) noexcept
{
	::operator delete(storage);
}

void operator delete(void *storage, std::size_t) noexcept
{
	::operator delete(storage);
}

void operator delete[](void *storage, std::size_t) noexcept
{
	::operator delete(storage);
}

void operator delete(void *storage, const std::nothrow_t &) noexcept
{
	::operator delete(storage);
}

void operator delete[](void *storage, const std::nothrow_t &) noexcept
{
	::operator delete(storage);
}

namespace NaWServer::tcp::detail
{

	// A paused scheduler lets the test enter the real runtime deterministically.
	// These are private test definitions, not a production platform abstraction.
	struct Worker
	{
		void (*entry)(void *) = nullptr;
		void *context = nullptr;
		bool owned = false, completed = false;
#if defined(_WIN32)
		void *fiber = nullptr;
#else
		ucontext_t fiber{};
		std::array<unsigned char, 64 * 1024> stack{};
#endif
	};
	// Linux test stacks are static; Windows fibers own OS-managed stacks. Neither
	// changes measured C++ payload allocations or races the failure injector.
	std::array<Worker, 4> test_workers;
	Worker *current_worker = nullptr;
	void *current_context = nullptr;
	std::uint64_t test_milliseconds = 0;
	std::size_t worker_starts = 0;
#if defined(_WIN32)
	void *scheduler_context = nullptr;
#else
	ucontext_t scheduler_context{};
#endif

	void yieldWorker()
	{
		if(!current_worker) return;

		auto *worker = current_worker;
		current_worker = nullptr;
		current_context = nullptr;
#if defined(_WIN32)
		SwitchToFiber(scheduler_context);
#else
		check(swapcontext(&worker->fiber, &scheduler_context) == 0, __LINE__);
#endif
		current_worker = worker;
		current_context = worker->context;
	}

#if defined(_WIN32)
	void CALLBACK enterWorker(void *argument)
	{
		auto *worker = static_cast<Worker *>(argument);
#else
	void enterWorker(unsigned index)
	{
		auto *worker = &test_workers[index];
#endif
		current_worker = worker;
		current_context = worker->context;
		worker->entry(worker->context);
		worker->completed = true;
		current_worker = nullptr;
		current_context = nullptr;
#if defined(_WIN32)
		SwitchToFiber(scheduler_context);
#endif
	}

	Worker *startTcpWorker(void (*entry)(void *), void *context, WorkerRole) noexcept
	{
		for(std::size_t index = 0; index < test_workers.size(); ++index)
		{
			auto &worker = test_workers[index];
			if(worker.owned) continue;

			worker.entry = entry; worker.context = context;
			worker.owned = true; worker.completed = false;
#if defined(_WIN32)
			if(!scheduler_context)
			{
				scheduler_context = ConvertThreadToFiber(nullptr);
				check(scheduler_context != nullptr, __LINE__);
			}
			worker.fiber = CreateFiber(64 * 1024, enterWorker, &worker);
			check(worker.fiber != nullptr, __LINE__);
#else
			check(getcontext(&worker.fiber) == 0, __LINE__);
			worker.fiber.uc_stack.ss_sp = worker.stack.data();
			worker.fiber.uc_stack.ss_size = worker.stack.size();
			worker.fiber.uc_link = &scheduler_context;
			makecontext(&worker.fiber, reinterpret_cast<void (*)()>(enterWorker), 1, static_cast<unsigned>(index));
#endif
			++worker_starts;
			return &worker;
		}
		check(false, __LINE__);
		return nullptr;
	}

	void runTestWorker()
	{
		check(!current_worker, __LINE__);
		for(unsigned rounds = 0;; ++rounds)
		{
			check(rounds < 100000, __LINE__);
			bool progressed = false;
			for(auto &worker : test_workers)
			{
				if(!worker.owned || worker.completed) continue;

				progressed = true;
#if defined(_WIN32)
				SwitchToFiber(worker.fiber);
#else
				check(swapcontext(&scheduler_context, &worker.fiber) == 0, __LINE__);
#endif
			}
			if(!progressed) return;
		}
	}

	bool workersOwned()
	{
		for(const auto &worker : test_workers) if(worker.owned) return true;

		return false;
	}

	bool isTcpWorkerThread(const Worker *worker) noexcept
	{
		return worker && worker == current_worker;
	}

	bool isTcpWorkerContext(const void *context) noexcept
	{
		return current_worker && current_context == context;
	}

	bool joinTcpWorker(Worker *worker, std::uint32_t) noexcept
	{
		if(!worker || current_worker) return false;

		if(!worker->completed) runTestWorker();

#if defined(_WIN32)
		DeleteFiber(worker->fiber);
#endif
		worker->owned = false;
		return true;
	}

	std::uint64_t tcpMilliseconds() noexcept { return test_milliseconds; }

	void sleepTcpWorker(std::uint32_t milliseconds) noexcept
	{
		test_milliseconds += milliseconds;
		yieldWorker();
	}

	// The fixture's workers are cooperative fibers, not native threads. A native
	// condition wait would block its sole scheduler; retain the same latched state
	// while yielding through the fixture's clock instead of sleeping the OS thread.
	bool WorkerSignal::initialize() noexcept
	{
		initialized_ = true;
		return true;
	}

	WorkerSignal::~WorkerSignal() = default;

	void WorkerSignal::notify() noexcept
	{
		storage_[0] = 1;
	}

	bool WorkerSignal::wait(std::uint32_t timeout_ms) noexcept
	{
		if(!storage_[0] && timeout_ms) sleepTcpWorker(std::min<std::uint32_t>(timeout_ms, 20));

		const bool signaled = storage_[0] != 0;
		storage_[0] = 0;
		return signaled;
	}

} // namespace NaWServer::tcp::detail

namespace
{

	using namespace NaWServer;
	using namespace NaWServer::tcp;
	using namespace NaWServer::tcp::detail;

#define CHECK(value) check((value), __LINE__)

	void measure(std::ptrdiff_t fail_at = -1)
	{
		phase = {};
		phase.fail_at = fail_at;
		phase.measuring = true;
	}

	AllocationPhase measured()
	{
		phase.measuring = false;
		return phase;
	}

	void report(const char *name, const AllocationPhase &result)
	{
		std::printf("TCP_STORAGE_PHASE %s explicit=%zu ordinary=%zu ordinary_bytes=%zu requested=%zu sizes=",
		    name, result.explicit_attempts, result.ordinary_allocations, result.ordinary_requested_bytes,
		    result.requested_bytes);
		for(std::size_t index = 0; index < result.explicit_attempts; ++index)
			std::printf("%s%zu", index ? "," : "", result.sizes[index]);
		std::puts("");
	}

	// Bounded one-client transport. It runs the actual runtime/handshake/writer,
	// but models readiness and peer EOF without sockets, threads, sleeps or heap.
	class StorageTransport final : public Transport
	{
		bool listener_ = false;
		bool client_ = false;
		bool accepted_ = false;
		std::size_t input_offset_ = 0;
		unsigned waits_ = 0;

	public:
		std::array<std::uint8_t, 12288> input{};
		std::array<std::uint8_t, 24576> output{};
		std::size_t input_size = 0;
		std::size_t output_size = 0;
		std::size_t first_read_limit = 0;
		std::size_t expected_reply_bytes = 0;
		const std::uint8_t *direct_payload_destination = nullptr;
		std::size_t listener_opens = 0;
		std::size_t client_closes = 0;

		void reset()
		{
			CHECK(!listener_ && !client_);
			accepted_ = false;
			input_offset_ = output_size = input_size = first_read_limit = expected_reply_bytes = 0;
			listener_opens = client_closes = waits_ = 0;
			direct_payload_destination = nullptr;
		}

		void append(const void *bytes, std::size_t size)
		{
			CHECK(size <= input.size() - input_size);
			std::memcpy(input.data() + input_size, bytes, size);
			input_size += size;
		}

		void request(std::string_view head)
		{
			reset();
			append(head.data(), head.size());
			first_read_limit = head.size();
		}

		std::string_view sent() const
		{
			return {reinterpret_cast<const char *>(output.data()), output_size};
		}

		std::size_t owned() const
		{
			return static_cast<std::size_t>(listener_) + client_;
		}

		ListenerResult openListener(const char *, std::uint16_t, int) override
		{
			CHECK(!listener_);
			listener_ = true;
			++listener_opens;
			return {Status::Ok, 0, {0, true}, 31000};
		}

		AcceptResult accept(Handle listener) override
		{
			CHECK(listener.valid && listener.value == 0 && listener_ && !accepted_);
			accepted_ = client_ = true;
			return {Status::Ok, 0, {1, true}};
		}

		IoResult readSome(Handle handle, std::uint8_t *destination, std::size_t capacity) override
		{
			CHECK(handle.valid && handle.value == 1 && client_ && destination && capacity);
			if(input_offset_ == input_size) return {IoStatus::PeerClosed, 0, 0};

			auto size = std::min(capacity, input_size - input_offset_);
			if(!input_offset_ && first_read_limit) size = std::min(size, first_read_limit);

			if(input_offset_ >= first_read_limit && size >= 1024 && !direct_payload_destination)
				direct_payload_destination = destination;

			std::memcpy(destination, input.data() + input_offset_, size);
			input_offset_ += size;
			return {IoStatus::Progress, size, 0};
		}

		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override
		{
			CHECK(handle.valid && handle.value == 1 && client_ && bytes && size);
			CHECK(size <= output.size() - output_size);
			std::memcpy(output.data() + output_size, bytes, size);
			output_size += size;
			return {IoStatus::Progress, size, 0};
		}

		WaitResult wait(const Interest *interests, Ready *ready, std::size_t count, std::uint32_t timeout) override
		{
			yieldWorker();
			CHECK(interests && ready && count && count <= 5 && ++waits_ < 100);
			std::fill_n(ready, count, Ready{});
			std::size_t ready_count = 0;
			const auto header_end = sent().find("\r\n\r\n");
			const bool reply_done = header_end != std::string_view::npos &&
			    output_size >= header_end + 4 + expected_reply_bytes;
			for(std::size_t index = 0; index < count; ++index)
			{
				const auto &interest = interests[index];
				CHECK(interest.handle.valid);
				if(interest.handle.value == 0)
					ready[index].readable = interest.read && !accepted_ && input_size;
				else
				{
					CHECK(interest.handle.value == 1 && client_);
					ready[index].readable = interest.read &&
					    (input_offset_ < input_size || !expected_reply_bytes || reply_done);
					ready[index].writable = interest.write;
				}

				if(ready[index].readable || ready[index].writable) ++ready_count;
			}
			if(ready_count) return {WaitStatus::Ready, ready_count, 0};

			if(!client_) return {WaitStatus::Fatal, 0, 901};

			test_milliseconds += timeout;
			return {WaitStatus::Timeout, 0, 0};
		}

		OperationResult shutdownWrite(Handle handle) override
		{
			CHECK(handle.valid && handle.value == 1 && client_);
			return {};
		}

		CloseResult close(Handle &handle) override
		{
			CHECK(handle.valid);
			if(handle.value == 0)
			{
				CHECK(listener_);
				listener_ = false;
			}
			else
			{
				CHECK(handle.value == 1 && client_);
				client_ = false;
				++client_closes;
			}

			handle = {};
			return {Status::Ok, 0, true};
		}

	};

	constexpr std::string_view handshake = "GET /ws HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\n"
	    "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
	    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";

	SendResult health(Response &response)
	{
		return response.text(200, "ready");
	}

	std::size_t startupFailures(bool with_websocket)
	{
		StorageTransport transport;
		TcpOptions options;
		options.recovery.enabled = false; // This fixture uses fatal wait to end its paused worker.
		TcpHttpBackend backend(transport, options);
		Route route("/health", HttpMethod::Get, health);
		WebSocketEndpoint websocket("/ws");
		HandlerBase *const routes[]{with_websocket ? static_cast<HandlerBase *>(&websocket) : &route};
		HttpServer server(backend, routes);
		const auto baseline = live_allocations;
		measure();
		CHECK(server.begin() == HttpError::Ok);
		const auto success = measured();
		CHECK(success.explicit_attempts > 0 && success.explicit_attempts < success.sizes.size());
		CHECK(server.end() == HttpError::Ok && transport.owned() == 0 && !workersOwned());
		CHECK(live_allocations == baseline);
		report(with_websocket ? "START_WS4" : "START_HTTP", success);
		for(std::size_t index = 0; index < success.explicit_attempts; ++index)
		{
			transport.reset();
			const auto starts = worker_starts;
			measure(static_cast<std::ptrdiff_t>(index));
			CHECK(server.begin() == HttpError::TransportError);
			const auto failed = measured();
			CHECK(failed.injected && transport.listener_opens == 0 && transport.owned() == 0);
			CHECK(worker_starts == starts && live_allocations == baseline && !server.running());
			CHECK(server.end() == HttpError::Ok);
			CHECK(server.begin() == HttpError::Ok);
			CHECK(server.end() == HttpError::Ok && transport.owned() == 0);
			CHECK(live_allocations == baseline);
		}
		return success.explicit_attempts;
	}

	std::size_t admissionAttempt(std::ptrdiff_t failure)
	{
		StorageTransport transport;
		TcpOptions options;
		options.recovery.enabled = false;
		TcpHttpBackend backend(transport, options);
		WebSocketEndpoint endpoint("/ws");
		unsigned connected = 0;
		endpoint.onConnect([&](WebSocketClient *) {
			++connected;
		});
		HandlerBase *const routes[]{&endpoint};
		HttpServer server(backend, routes);
		const auto baseline = live_allocations;
		transport.request(handshake);
		CHECK(server.begin() == HttpError::Ok);
		measure(failure);
		runTestWorker();
		const auto result = measured();
		CHECK(server.end() == HttpError::Ok && transport.owned() == 0);
		CHECK(transport.client_closes == 1 && endpoint.count() == 0 && live_allocations == baseline);
		if(failure < 0)
		{
			CHECK(connected == 1 && transport.sent().find("HTTP/1.1 101 ") == 0);
			report("WS_ADMISSION", result);
		}
		else
		{
			CHECK(result.injected && connected == 0);
			CHECK(transport.sent().find("HTTP/1.1 503 ") == 0);
			CHECK(transport.sent().find("101 Switching") == std::string_view::npos);
		}

		return result.explicit_attempts;
	}

	struct EchoBody
	{
		StorageTransport &transport;
		unsigned calls = 0;
		SendResult operator()(Request &request, Response &response)
		{
			++calls;
			const auto body = request.body();
			CHECK(body.size() == request.contentLength());
			if(!body.empty())
			{
				CHECK(body.size() == 4096 && body.data() == transport.direct_payload_destination);
				for(std::size_t index = 0; index < body.size(); ++index)
					CHECK(body.data()[index] == 0xa7);
			}

			return response.bytes(200, body);
		}

	};

	void httpSteadyState(bool post)
	{
		StorageTransport transport;
		TcpOptions options;
		options.recovery.enabled = false;
		TcpHttpBackend backend(transport, options);
		EchoBody endpoint{transport};
		Route route("/payload", HttpMethod::Any, EndpointRef(endpoint));
		HandlerBase *const routes[]{&route};
		HttpServer server(backend, routes);
		if(post)
		{
			transport.request("POST /payload HTTP/1.1\r\nHost: test\r\nContent-Length: 4096\r\n\r\n");
			std::array<std::uint8_t, 4096> body;
			body.fill(0xa7);
			transport.append(body.data(), body.size());
		}
		else transport.request("GET /payload HTTP/1.1\r\nHost: test\r\n\r\n");

		const auto baseline = live_allocations;
		CHECK(server.begin() == HttpError::Ok);
		measure();
		runTestWorker();
		const auto result = measured();
		CHECK(endpoint.calls == 1 && transport.sent().find("HTTP/1.1 200 ") == 0);
		CHECK(result.explicit_attempts == 0 && result.ordinary_allocations == 0);
		CHECK(server.end() == HttpError::Ok && live_allocations == baseline && transport.owned() == 0);
		report(post ? "POST4096_DIRECT_BODY" : "GET", result);
	}

	void appendFragment(StorageTransport &transport, std::uint8_t opcode, bool final, std::size_t size)
	{
		const std::uint8_t header[]{static_cast<std::uint8_t>((final ? 0x80 : 0) | opcode), 0xfe,
			                        static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size), 1, 2, 3, 4};
		transport.append(header, sizeof(header));
		for(std::size_t index = 0; index < size; ++index)
		{
			const auto byte = static_cast<std::uint8_t>(0xa7 ^ (1 + (index & 3)));
			transport.append(&byte, 1);
		}
	}

	void websocketSteadyState()
	{
		StorageTransport transport;
		TcpOptions options;
		options.recovery.enabled = false;
		TcpHttpBackend backend(transport, options);
		WebSocketEndpoint endpoint("/ws");
		unsigned messages = 0;
		endpoint.onConnect([](WebSocketClient *) {
			measure();
		});
		endpoint.onMessage([&](WebSocketClient *client, const std::uint8_t *bytes, std::size_t size, bool binary)
		{
			++messages;
			// The network decoder reads directly into its reserved RX buffer;
			// serialized callbacks now own a separate pre-admission mailbox.
			CHECK(binary && size == 4096 && transport.direct_payload_destination &&
			    bytes != transport.direct_payload_destination);
			for(std::size_t index = 0; index < size; ++index)
				CHECK(bytes[index] == 0xa7);
			CHECK(client->send_binary(bytes, size) == WebSocketSendResult::Queued);
		});
		HandlerBase *const routes[]{&endpoint};
		HttpServer server(backend, routes);
		transport.request(handshake);
		appendFragment(transport, 2, false, 2048);
		appendFragment(transport, 0, true, 2048);
		transport.expected_reply_bytes = 4100;
		const auto baseline = live_allocations;
		CHECK(server.begin() == HttpError::Ok);
		runTestWorker();
		const auto result = measured();
		CHECK(messages == 1 && result.explicit_attempts == 0 && result.ordinary_allocations == 0);
		const auto response_end = transport.sent().find("\r\n\r\n") + 4;
		CHECK(response_end >= 4 && transport.output_size == response_end + 4100);
		CHECK(transport.output[response_end] == 0x82 && transport.output[response_end + 1] == 126);
		for(std::size_t index = response_end + 4; index < transport.output_size; ++index)
			CHECK(transport.output[index] == 0xa7);
		CHECK(server.end() == HttpError::Ok && endpoint.count() == 0);
		CHECK(transport.owned() == 0 && live_allocations == baseline);
		report("WS4096_FRAGMENT_RX_AND_ECHO_TX", result);
	}

	class ManualSseAdapter final : public sse::QueuedAdapter
	{
	public:
		bool start() override
		{
			return beginRuntime();
		}

		bool stop(std::uint32_t) override
		{
			requestStop();
			for(unsigned index = 0; index < 16; ++index)
			{
				poll(++test_milliseconds);
				if(finishRuntime()) return true;
			}
			return false;
		}

		void tick()
		{
			poll(++test_milliseconds);
		}

	};

	class SseHttpExchange final : public HttpExchange
	{
	public:
		TcpSseExchange binding;
		SseHttpExchange(ConnectionRegistry &registry, Token token)
		  : binding(registry, token)
		{
		}

		HttpRequestHead head() const override
		{
			return {HttpMethod::Get, "/events", 0};
		}

		HeaderReadResult readHeader(const char *, char *, std::size_t) const override
		{
			return {HeaderReadStatus::NotFound, 0};
		}

		BodyReadResult readBody(std::uint8_t *, std::size_t) override
		{
			return {BodyReadStatus::End, 0};
		}

		ResponseTransport responseTransport() override
		{
			return {};
		}

		void *extension(const void *key) const override
		{
			return key == &sse::exchange_key ? const_cast<TcpSseExchange *>(&binding) : nullptr;
		}

	};

	void sseSteadyState()
	{
		const auto baseline = live_allocations;
		StorageTransport transport;
		auto listener = transport.openListener("127.0.0.1", 0, 1).handle;
		auto handle = transport.accept(listener).handle;
		ConnectionSlot slots[1]{};
		CloseState close_states[1]{};
		int identity = 0;
		ConnectionRegistry registry(transport, &identity, slots, close_states, 1);
		const auto token = registry.claim(handle, Owner::Http);
		CHECK(token.valid() && !handle.valid);
		ManualSseAdapter adapter;
		sse::EventSource events(adapter);
		CHECK(events.start());
		{
			SseHttpExchange exchange(registry, token);
			Request request(exchange);
			Response response(exchange.responseTransport());
			measure();
			const auto opened = events.open(request, response);
			const auto admission = measured();
			CHECK(opened.active && opened.result == SendResult::Ok && transport.output_size == 0);
			adapter.tick();
			CHECK(transport.output_size == 0); // Actual lease gates native I/O until callback release.
			CHECK(exchange.binding.callbackComplete());
			report("SSE_ADMISSION", admission);
			measure();
			for(unsigned index = 0; index < 4; ++index)
				adapter.tick();
			const auto preface = measured();
			CHECK(transport.sent().find("text/event-stream") != std::string_view::npos);
			CHECK(preface.explicit_attempts == 0 && preface.ordinary_allocations == 0);
			report("SSE_PREFACE_SEND", preface);
			const auto preface_size = transport.output_size;
			std::array<char, 512> payload;
			payload.fill('s');
			sse::Frame frame;
			measure();
			CHECK(sse::encode({{payload.data(), payload.size()}, std::nullopt, std::nullopt, std::nullopt}, frame) ==
			    sse::EncodeResult::Ok);
			CHECK(events.publish(opened.token, frame) == sse::Publish::Queued);
			for(unsigned index = 0; index < 4; ++index)
				adapter.tick();
			const auto publication = measured();
			CHECK(transport.output_size == preface_size + frame.size);
			CHECK(std::memcmp(transport.output.data() + preface_size, frame.bytes.data(), frame.size) == 0);
			CHECK(publication.explicit_attempts == 0 && publication.ordinary_allocations == 0);
			report("SSE_ENCODE_PUBLISH_SEND512", publication);
			CHECK(events.stop(0) && events.active() == 0 && events.completions() == 1);
			CHECK(registry.active() == 0 && transport.client_closes == 1);
		}
		CHECK(registry.beginStop()); // The exchange released its registration after native close.
		CHECK(transport.close(listener).released && transport.owned() == 0);
		CHECK(live_allocations == baseline);
	}

} // namespace

int main()
{
	const auto baseline = live_allocations;
	{
		StorageTransport transport;
		TcpOptions options;
		options.buffer_memory.max_bytes = 1024;
		TcpHttpBackend backend(transport, options);
		Route route("/health", HttpMethod::Get, health);
		HandlerBase *const routes[]{&route};
		HttpServer server(backend, routes);
		CHECK(server.begin() == HttpError::TransportError);
		CHECK(transport.listener_opens == 0 && transport.owned() == 0);
		CHECK(backend.bufferMemoryStats().budget_rejections > 0);
		CHECK(backend.bufferMemoryStats().used_bytes == 0);
		CHECK(server.end() == HttpError::Ok);
	}
	CHECK(live_allocations == baseline);
	const auto http_startup = startupFailures(false);
	const auto websocket_startup = startupFailures(true);
	CHECK(websocket_startup > http_startup);
	const auto admission = admissionAttempt(-1);
	for(std::size_t index = 0; index < admission; ++index)
	{
		(void)admissionAttempt(static_cast<std::ptrdiff_t>(index));
		(void)admissionAttempt(-1); // Fresh handshake after each failed admission.
	}
	httpSteadyState(false);
	httpSteadyState(true);
	websocketSteadyState();
	sseSteadyState();
	CHECK(live_allocations == baseline && !workersOwned());
	std::printf("TCP_STORAGE_PASS checks=%u startup_failures=%zu/%zu admission_failures=%zu\n",
	    checks, http_startup, websocket_startup, admission);
}
