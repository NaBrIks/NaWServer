// SPDX-License-Identifier: MIT
#pragma once

// Bounded measurement fixture, separate from the production demo and frontend.
#include <NaWServer/http_server.h>
#include <NaWServer/header_request.h>
#include <NaWServer/route.h>
#include <NaWServer/response.h>
#if NAWSERVER_DEMO_README_PROFILE >= 2
#include <NaWServer/websocket.h>
#endif
#if NAWSERVER_DEMO_README_PROFILE == 3
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/sse.h>
#endif
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#if NAWSERVER_DEMO_ASYNC_PROBE
#include "async_probe.hpp"
#endif

namespace compact::readme
{

	using namespace NaWServer;
	constexpr unsigned pages = NAWSERVER_DEMO_README_PROFILE == 1 ? 0 : NAWSERVER_DEMO_README_PROFILE == 2 ? 3 : 6;
	constexpr unsigned sse_clients = NAWSERVER_DEMO_README_PROFILE == 3 ? 4 : 0;
	inline TcpOptions options()
	{
		TcpOptions value;
		value.bind_address = "0.0.0.0"; value.port = 80;
		value.max_websocket_clients = pages;
		value.http_memory.enabled = true;
		value.keep_alive.enabled = NAWSERVER_DEMO_ASYNC_PROBE;
		if(NAWSERVER_DEMO_ASYNC_PROBE) value.keep_alive.maximum_requests = 1000;
		value.http_memory.initial_requests = 1;
		value.http_memory.maximum_requests = pages ? 3 : 1;
		value.managed.ingress_slots = pages ? 3 : 1;
		value.managed.header_bytes = 1024; value.managed.response_body_bytes = 1024;
		value.managed.stream_bytes_per_client = 4096;
		value.managed.header_timeout_ms = 3000;
		value.buffer_memory.max_bytes = 128 * 1024;
		value.transmission.enabled = true;
		value.transmission.execution = NAWSERVER_DEMO_ASYNC_PROBE && NAWSERVER_DEMO_TX_EXECUTOR == 0 ? tcp::TransmissionExecution::DedicatedWorker : tcp::TransmissionExecution::Cooperative;
		value.transmission.maximum_blocks = pages ? pages * 3 + 4 : 2;
		value.transmission.maximum_bytes = value.transmission.maximum_blocks * 1024;
		value.transmission.block_bytes = 1024; value.transmission.step_bytes = 1024;
		value.transmission.reserved_important_blocks = 0;
		value.transmission.maximum_blocks_per_connection = 2;
		value.transmission.stall_timeout_ms = 3000;
		return value;
	}

	inline HttpLimits limits()
	{
		HttpLimits value;
		value.max_connections = pages ? pages + sse_clients + 3 : 2;
		value.max_header_bytes = 1024; value.max_headers = 24; value.max_body_size = 256;
		value.max_target_size = 128; value.exchange_timeout_ms = 120000;
		return value;
	}

	struct Fixture
	{
		EspPlatform &platform;
		TcpHttpBackend &backend;
		std::atomic<bool> restart{false};
		std::atomic<unsigned> echoes{0}, echo_rejected{0}, publications{0};
		std::uint64_t next = 0;
#if NAWSERVER_DEMO_README_PROFILE >= 2
		WebSocketEndpoint websocket{"/ws", WebSocketLimits{256, 2, pages, 1024}};
#endif
#if NAWSERVER_DEMO_README_PROFILE == 3
		static sse::Limits streamLimits()
		{
			sse::Limits value;
			value.clients = sse_clients; value.pending = 1; value.bytes_per_client = 128;
			value.total_bytes = sse_clients * 128; value.buffer_memory.max_bytes = 16 * 1024;
			return value;
		}
		sse::TcpAdapter adapter;
		sse::EventSource stream;
		detail::Mutex mutex;
		std::array<sse::Token, sse_clients> tokens{};
		std::uint64_t publication_time = 0;
		unsigned pending_publications = 0;
		std::atomic<unsigned> sse_busy{0}, sse_rejected{0}, sse_expired{0};
#endif
		struct Access
		{
			Fixture &self;
			Result operator()(HeaderContext &context, HeaderNext &next) const
			{
				if(context.request.path() != "/api/state") return next();
				char secret[65];
				const auto read = context.request.readHeader("X-Client-Key", secret, sizeof(secret));
				const bool allowed = read.status == HeaderReadStatus::Ok && self.platform.authorized({secret, read.size});
				std::memset(secret, 0, sizeof(secret));
				return allowed ? next() : responseResult(context.response.text(401, "API key required"));
			}
		} access{*this};
		decltype(makeHeaderChain(access)) headers{access};
#if NAWSERVER_DEMO_ASYNC_PROBE
		std::function<Result(Context &)> probe_state_callback, probe_config_callback, probe_file_callback;
		Route probe_state, probe_config, probe_file;
#endif
		std::function<Result(Context &)> state_callback, home_callback, ota_callback;
#if NAWSERVER_DEMO_README_PROFILE == 3
		std::function<Result(Context &)> events_callback;
#endif
		Route api, state, home, firmware;
#if NAWSERVER_DEMO_README_PROFILE == 3
		Route events;
#endif
		HandlerBase *handlers[4 + (pages ? 1 : 0) + (NAWSERVER_DEMO_README_PROFILE == 3 ? 1 : 0) + (NAWSERVER_DEMO_ASYNC_PROBE ? 3 : 0)];
		HttpServer server;

		static BodyReceiver upload(void *context, HttpExchange &exchange)
		{
			char hash[65]; const auto read = exchange.readHeader("X-Firmware-SHA256", hash, sizeof(hash));
			if(read.status != HeaderReadStatus::Ok || read.size != 64) return {};
			return static_cast<EspPlatform *>(context)->beginFirmware(exchange.head().content_length, {hash, read.size});
		}

		Result snapshot(Context &context)
		{
			const auto value = platform.status();
			char bytes[768];
			const auto tx = backend.transmissionStats();
			const auto size = std::snprintf(bytes, sizeof(bytes),
			    "{\"profile\":%u,\"firmware\":\"%s\",\"platform\":\"%s\",\"running_slot\":\"%s\",\"uptime_ms\":%llu,"
			    "\"free_heap\":%u,\"largest\":%u,\"physical\":%u,\"cpu_busy_us\":%llu,\"cpu_sample_us\":%llu,"
			    "\"ws\":%u,\"sse\":%u,\"echoes\":%u,\"echo_rejected\":%u,\"publications\":%u,\"tx_failed\":%u,"
			    "\"sse_busy\":%u,\"sse_rejected\":%u,\"sse_expired\":%u}",
			    unsigned(NAWSERVER_DEMO_README_PROFILE), value.firmware, value.platform, value.running_slot,
			    static_cast<unsigned long long>(value.uptime_ms), unsigned(value.free_heap), unsigned(value.largest_free_block),
			    unsigned(value.physical_ram_bytes), static_cast<unsigned long long>(value.cpu_busy_us),
			    static_cast<unsigned long long>(value.cpu_sample_us),
#if NAWSERVER_DEMO_README_PROFILE >= 2
			    unsigned(websocket.count()),
#else
			    0u,
#endif
#if NAWSERVER_DEMO_README_PROFILE == 3
			    unsigned(stream.active()),
#else
			    0u,
#endif
			    echoes.load(), echo_rejected.load(), publications.load(), unsigned(tx.failed_jobs),
#if NAWSERVER_DEMO_README_PROFILE == 3
			    sse_busy.load(), sse_rejected.load(), sse_expired.load()
#else
			    0u, 0u, 0u
#endif
			    );
			if(size <= 0 || std::size_t(size) >= sizeof(bytes)) return Result::Error;
			return responseResult(context.response.send(200, "application/json", {reinterpret_cast<const std::uint8_t *>(bytes), std::size_t(size)}));
		}

		Fixture(EspPlatform &device, TcpHttpBackend &tcp) : platform(device), backend(tcp),
#if NAWSERVER_DEMO_README_PROFILE == 3
		    adapter(tcp, streamLimits()), stream(adapter),
#endif
#if NAWSERVER_DEMO_ASYNC_PROBE
		    probe_state_callback([](Context &context) { return applicationProbe().snapshot(context); }),
		    probe_config_callback([](Context &context)
		    {
			const auto query = context.request.rawQuery();
			if(query.size() != 6 || query.substr(0, 5) != "mode=" || query[5] < '0' || query[5] > '4' ||
			    !applicationProbe().configure(unsigned(query[5] - '0'))) return responseResult(context.response.text(400, "mode=0..4"));
			return responseResult(context.response.text(202, "configured"));
		    }),
		    probe_file_callback([](Context &context)
		    {
			static constexpr std::array<std::uint8_t, 64 * 1024> bytes{};
			return responseResult(context.response.sendImmutable(200, "application/octet-stream", {bytes.data(), bytes.size()}));
		    }),
		    probe_state("/probe/state", HttpMethod::Get, EndpointRef(probe_state_callback)),
		    probe_config("/probe/config", HttpMethod::Post, EndpointRef(probe_config_callback)),
		    probe_file("/probe/file", HttpMethod::Get, EndpointRef(probe_file_callback)),
#endif
		    state_callback([this](Context &context) { return snapshot(context); }),
		    home_callback([](Context &context)
		    {
			static constexpr char html[] = "<!doctype html><html lang=en><meta charset=utf-8><title>NaWServer profile</title>"
			    "<style>body{font:14px system-ui;background:#faf8f3;color:#302b24;margin:20px}pre{font:inherit}</style>"
			    "<b>NaWServer</b><pre id=s>Connecting</pre><script>let u=new URL(location);"
			    "let w=new WebSocket('ws://'+u.host+'/ws');w.onmessage=e=>s.textContent=e.data;"
			    "setInterval(()=>w.readyState===1&&w.send('status'),1000);</script></html>";
			return responseResult(context.response.sendImmutable(200, "text/html", {reinterpret_cast<const std::uint8_t *>(html), sizeof(html) - 1}));
		    }),
		    ota_callback([this](Context &context)
		    {
			const auto result = context.response.text(202, "{\"ok\":true}");
			if(result == SendResult::Ok) restart.store(true);
			return responseResult(result);
		    }),
#if NAWSERVER_DEMO_README_PROFILE == 3
		    events_callback([this](Context &context)
		    {
			const auto opened = stream.open(context.request, context.response);
			if(!opened.active) return responseResult(context.response.text(503, "SSE full"));
			detail::LockGuard<detail::Mutex> lock(mutex);
			tokens[opened.token.slot] = opened.token;
			return Result::Complete;
		    }),
#endif
		    api("/api/state", HttpMethod::Get, EndpointRef(state_callback)),
		    state("/ui/status", HttpMethod::Get, EndpointRef(state_callback)),
		    home("/", HttpMethod::Get, EndpointRef(home_callback)),
		    firmware("/ui/firmware", HttpMethod::Post, EndpointRef(ota_callback), {}, {}, {}, {},
		        {BodyMode::Stream, device.maximumFirmwareBytes(), 1024, &device, upload}),
#if NAWSERVER_DEMO_README_PROFILE == 3
		    events("/events", HttpMethod::Get, EndpointRef(events_callback)),
#endif
		    handlers{&api, &state, &home, &firmware
#if NAWSERVER_DEMO_ASYNC_PROBE
		        , &probe_state, &probe_config, &probe_file
#endif
#if NAWSERVER_DEMO_README_PROFILE >= 2
		        , &websocket
#endif
#if NAWSERVER_DEMO_README_PROFILE == 3
		        , &events
#endif
		    }, server(tcp, handlers, limits(), {}, {}, headers)
		{
#if NAWSERVER_DEMO_README_PROFILE >= 2
			websocket.onMessage([this](WebSocketClient *client, const std::uint8_t *data, std::size_t size, bool binary)
			{
				(void)binary;
				if(client->send_binary(data, size) == WebSocketSendResult::Queued) ++echoes; else ++echo_rejected;
			});
#endif
		}

		bool start()
		{
#if NAWSERVER_DEMO_README_PROFILE == 3
			if(!stream.start()) return false;
#endif
			return server.begin() == HttpError::Ok;
		}

		void tick()
		{
			platform.poll();
			if(restart.load())
			{
				const auto pending = backend.transmissionStats();
				if(!pending.queued_jobs && !pending.active_jobs && !pending.filling_jobs)
				{ restart.store(false); platform.scheduleRestart(); }
				return;
			}
			const auto now = platform.milliseconds();
			if(now >= next)
			{
				next = now + 1000;
#if NAWSERVER_DEMO_README_PROFILE >= 2
				char message[64];
				std::snprintf(message, sizeof(message), "{\"uptime\":%llu}", static_cast<unsigned long long>(now));
				(void)websocket.send(message);
#endif
#if NAWSERVER_DEMO_README_PROFILE == 3
				for(unsigned i = 0; i < sse_clients; ++i) if(pending_publications & (1u << i)) ++sse_expired;
				publication_time = now;
				pending_publications = (1u << sse_clients) - 1;
#endif
			}
#if NAWSERVER_DEMO_README_PROFILE == 3
			if(!pending_publications) return;
			// Busy is transient, not a successful publication. Retry on later ticks,
			// retaining only a timestamp and one bit per client, for at most one period.
			sse::Frame frame;
			const auto written = std::snprintf(frame.bytes.data(), frame.bytes.size(), "data: {\"uptime\":%llu}\n\n", static_cast<unsigned long long>(publication_time));
			frame.size = std::size_t(written);
			std::array<sse::Token, sse_clients> current;
			{ detail::LockGuard<detail::Mutex> lock(mutex); current = tokens; }
			for(unsigned i = 0; i < sse_clients; ++i)
			{
				if(!(pending_publications & (1u << i))) continue;
				if(current[i].slot >= sse_clients) { pending_publications &= ~(1u << i); continue; }
				const auto result = stream.publish(current[i], frame);
				if(result == sse::Publish::Busy) { ++sse_busy; continue; }
				pending_publications &= ~(1u << i);
				if(result == sse::Publish::Queued) ++publications; else ++sse_rejected;
			}
#endif
		}
	};

	[[noreturn]] inline void run(EspPlatform &platform)
	{
		vTaskDelay(pdMS_TO_TICKS(3000));
		const auto before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		static tcp::IdfTransport transport;
		if(transport.start().status != tcp::Status::Ok) std::abort();
		static TcpHttpBackend backend(transport, options());
		static Fixture fixture(platform, backend);
		if(!fixture.start()) std::abort();
#if NAWSERVER_DEMO_ASYNC_PROBE
		if(!applicationProbe().start()) std::abort();
#endif
		for(unsigned i = 0; i < 100; ++i) { fixture.tick(); vTaskDelay(pdMS_TO_TICKS(10)); }
		const auto after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		printf("README_PROFILE profile=%u before=%u after=%u cost=%u\n", unsigned(NAWSERVER_DEMO_README_PROFILE), unsigned(before), unsigned(after), unsigned(before - after));
		for(;;) { fixture.tick(); vTaskDelay(pdMS_TO_TICKS(10)); }
	}

}
