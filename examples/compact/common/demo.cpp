// SPDX-License-Identifier: MIT
#include "demo.hpp"
#include "compact_web.hpp"
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/json.h>
#include <NaWServer/detail/synchronization.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include <memory>
#ifndef NAWSERVER_DEMO_TX_MEMORY_MODE
#define NAWSERVER_DEMO_TX_MEMORY_MODE 0
#endif
#ifndef NAWSERVER_DEMO_TX_EXECUTOR
#define NAWSERVER_DEMO_TX_EXECUTOR 0
#endif
#if NAWSERVER_DEMO_TX_EXECUTOR < 0 || NAWSERVER_DEMO_TX_EXECUTOR > 1
#error Invalid NAWSERVER_DEMO_TX_EXECUTOR
#endif
#ifndef NAWSERVER_DEMO_TX_BLOCKS
#define NAWSERVER_DEMO_TX_BLOCKS 4
#endif
#ifndef NAWSERVER_DEMO_TX_BLOCK_BYTES
#define NAWSERVER_DEMO_TX_BLOCK_BYTES 1024
#endif
#ifndef NAWSERVER_DEMO_TX_METADATA_MODE
#define NAWSERVER_DEMO_TX_METADATA_MODE 0
#endif
#ifndef NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES
#define NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES 2048
#endif
#ifndef NAWSERVER_DEMO_TX_PAYLOAD_PSRAM
#define NAWSERVER_DEMO_TX_PAYLOAD_PSRAM 0
#endif
#ifndef NAWSERVER_DEMO_TX_INTERNAL_ONLY
#define NAWSERVER_DEMO_TX_INTERNAL_ONLY 0
#endif
#ifndef NAWSERVER_DEMO_TX_BULK_PROBE
#define NAWSERVER_DEMO_TX_BULK_PROBE 0
#endif
#if NAWSERVER_DEMO_TX_BULK_PROBE != 0 && NAWSERVER_DEMO_TX_BULK_PROBE != 1
#error NAWSERVER_DEMO_TX_BULK_PROBE must be 0 or 1
#endif
#if NAWSERVER_DEMO_TX_BULK_PROBE && !defined(ESP_PLATFORM)
#error The compact TX bulk probe is an ESP-only diagnostic
#endif
#if NAWSERVER_DEMO_TX_METADATA_MODE < 0 || NAWSERVER_DEMO_TX_METADATA_MODE > 2 || NAWSERVER_DEMO_TX_PAYLOAD_PSRAM < 0 || NAWSERVER_DEMO_TX_PAYLOAD_PSRAM > 1
#error Invalid compact TX placement
#endif
#if (NAWSERVER_DEMO_TX_METADATA_MODE == 2 || NAWSERVER_DEMO_TX_PAYLOAD_PSRAM || NAWSERVER_DEMO_TX_INTERNAL_ONLY) && !defined(ESP_PLATFORM)
#error PSRAM placement requires ESP-IDF
#endif
#if NAWSERVER_DEMO_TX_PAYLOAD_PSRAM && NAWSERVER_DEMO_TX_MEMORY_MODE == 2
#error Static TX payload and PSRAM payload cannot be selected together
#endif
#if NAWSERVER_DEMO_TX_METADATA_MODE == 1 && NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES < 256
#error Compact TX metadata arena is too small
#endif
#if NAWSERVER_DEMO_TX_METADATA_MODE == 2 || NAWSERVER_DEMO_TX_PAYLOAD_PSRAM || NAWSERVER_DEMO_TX_INTERNAL_ONLY
#include <esp_heap_caps.h>
#endif
#ifndef NAWSERVER_DEMO_HTTP_JOBS
#define NAWSERVER_DEMO_HTTP_JOBS 2
#endif
#ifndef NAWSERVER_DEMO_CONNECTIONS
#define NAWSERVER_DEMO_CONNECTIONS 4
#endif
#ifndef NAWSERVER_DEMO_INGRESS_SLOTS
#define NAWSERVER_DEMO_INGRESS_SLOTS 2
#endif
#if NAWSERVER_DEMO_INGRESS_SLOTS < 1 || NAWSERVER_DEMO_INGRESS_SLOTS > 12
#error Invalid compact ingress capacity
#endif
#if NAWSERVER_DEMO_HTTP_JOBS < 2 || NAWSERVER_DEMO_HTTP_JOBS > 8 || NAWSERVER_DEMO_CONNECTIONS < NAWSERVER_DEMO_HTTP_JOBS + 2 || NAWSERVER_DEMO_CONNECTIONS > 12
#error Invalid compact HTTP capacity
#endif
#if NAWSERVER_DEMO_TX_MEMORY_MODE < 0 || NAWSERVER_DEMO_TX_MEMORY_MODE > 2
#error Invalid NAWSERVER_DEMO_TX_MEMORY_MODE
#endif
#if NAWSERVER_DEMO_TX_BLOCKS < 2 || NAWSERVER_DEMO_TX_BLOCKS > 12 || NAWSERVER_DEMO_TX_BLOCK_BYTES < 768 || NAWSERVER_DEMO_TX_BLOCK_BYTES > 4096 || (NAWSERVER_DEMO_TX_BLOCK_BYTES % 16) != 0
#error Invalid compact TX block configuration
#endif
#ifndef NAWSERVER_DEMO_UPLOAD_TIMEOUT_MS
#define NAWSERVER_DEMO_UPLOAD_TIMEOUT_MS 120000
#endif
#if NAWSERVER_DEMO_STATUS_WS
#include <NaWServer/websocket.h>
#else
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/sse.h>
#endif

namespace compact
{

#if NAWSERVER_DEMO_TX_BULK_PROBE
	// Application-owned diagnostic route; disabled builds retain their original
	// handler table and constructor ABI without a dependency on the ESP fixture.
	NaWServer::HandlerBase &txBulkProbeHandler();
#endif

#if NAWSERVER_DEMO_TX_MEMORY_MODE == 2
	alignas(std::max_align_t) static std::uint8_t transmission_storage[NAWSERVER_DEMO_TX_BLOCKS * NAWSERVER_DEMO_TX_BLOCK_BYTES];
#endif
#if NAWSERVER_DEMO_TX_METADATA_MODE == 1
	alignas(std::max_align_t) static std::uint8_t metadata_storage[NAWSERVER_DEMO_TX_METADATA_STATIC_BYTES];
	static std::atomic<bool> metadata_used{false};
	static void *allocateMetadata(void *, std::size_t size)
	{
		bool expected = false;
		return size <= sizeof(metadata_storage) && metadata_used.compare_exchange_strong(expected, true) ? metadata_storage : nullptr;
	}
	static void releaseMetadata(void *, void *storage, std::size_t)
	{
		if(storage == metadata_storage) metadata_used.store(false);
	}
#endif
#if NAWSERVER_DEMO_TX_METADATA_MODE == 2 || NAWSERVER_DEMO_TX_PAYLOAD_PSRAM || NAWSERVER_DEMO_TX_INTERNAL_ONLY
	static void releaseEsp(void *, void *storage, std::size_t)
	{
		heap_caps_free(storage);
	}
#endif
#if NAWSERVER_DEMO_TX_METADATA_MODE == 2 || NAWSERVER_DEMO_TX_PAYLOAD_PSRAM
	static void *allocatePsram(void *, std::size_t size)
	{
		return heap_caps_aligned_alloc(alignof(std::max_align_t), size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	}

#endif
#if NAWSERVER_DEMO_TX_INTERNAL_ONLY
	static void *allocateInternal(void *, std::size_t size)
	{
		return heap_caps_aligned_alloc(alignof(std::max_align_t), size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	}
#endif

	using namespace NaWServer;
	struct Demo::Impl
	{
		Platform &platform;
		TcpHttpBackend &backend;
		std::atomic<bool> restart{false};
		std::atomic<std::uint32_t> http_requests{0}, stream_records{0};
		std::uint64_t next = 0;
		alignas(std::max_align_t) char json_workspace[1024]{};
		struct Access
		{
			Impl &self;
			Result operator()(HeaderContext &context, HeaderNext &next) const
			{
				self.http_requests.fetch_add(1);
				if(context.request.path() == "/") return next();
				const auto path = context.request.path();
				const bool management = path == "/ui/status" || path == "/ui/events" || path == "/ui/config" ||
				    path == "/ui/firmware" || path == "/ui/api-key" || path == "/ui/metrics";
				const bool firmware = path == "/api/firmware" || path == "/ui/firmware";
				char key[65];
				const auto read = context.request.readHeader("X-Client-Key", key, sizeof(key));
				bool allowed = management || (read.status == HeaderReadStatus::Ok && self.platform.authorized({key, read.size}));
#if NAWSERVER_DEMO_STATUS_WS
				if(!allowed && context.request.path() == "/api/events")
				{
					char protocol[80];
					const auto value = context.request.readHeader("Sec-WebSocket-Protocol", protocol, sizeof(protocol));
					const std::string_view text(protocol, value.status == HeaderReadStatus::Ok ? value.size : 0);
					allowed = text.substr(0, 4) == "key." && self.platform.authorized(text.substr(4));
				}
#endif
				std::memset(key, 0, sizeof(key));
				if(!allowed) return responseResult(context.response.text(401, "API key required"));
				char origin[160], host[120];
				const auto source = context.request.readHeader("Origin", origin, sizeof(origin));
				if(source.status != HeaderReadStatus::NotFound)
				{
					const auto target = context.request.readHeader("Host", host, sizeof(host));
					if(source.status != HeaderReadStatus::Ok || target.status != HeaderReadStatus::Ok ||
					    std::string_view(origin, source.size).substr(0, 7) != "http://" ||
					    std::string_view(origin + 7, source.size - 7) != std::string_view(host, target.size))
						return responseResult(context.response.text(403, "Origin rejected"));
				}
				if(context.request.method() == HttpMethod::Post)
				{
					char type[64];
					const auto field = context.request.readHeader("Content-Type", type, sizeof(type));
					const std::string_view value(type, field.status == HeaderReadStatus::Ok ? field.size : 0);
					const auto media = value.substr(0, value.find(';'));
					if(media != (firmware ? "application/octet-stream" : "application/json"))
						return responseResult(context.response.text(415, "Unsupported media type"));
				}
				if(firmware && !self.platform.maximumFirmwareBytes())
					return responseResult(context.response.text(501, "No native OTA target"));
				if(firmware)
				{
					char hash[65]; const auto field = context.request.readHeader("X-Firmware-SHA256", hash, sizeof(hash));
					if(field.status != HeaderReadStatus::Ok || field.size != 64)
						return responseResult(context.response.text(400, "SHA-256 required"));
					for(unsigned char value : std::string_view(hash, field.size))
						if(!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
							return responseResult(context.response.text(400, "Invalid SHA-256"));
				}
				return next();
			}
		} access{*this};
		decltype(makeHeaderChain(access)) protected_headers{access};
#if NAWSERVER_DEMO_STATUS_WS
		WebSocketEndpoint stream{"/api/events", WebSocketLimits{512, 1, 1, 512}};
#else
		static sse::Limits streamLimits()
		{
			sse::Limits value;
			value.clients = 2; value.pending = 1; value.bytes_per_client = 768;
			value.total_bytes = 1536; value.buffer_memory.max_bytes = 4096;
			return value;
		}
		sse::TcpAdapter stream_backend;
		sse::EventSource stream;
		detail::Mutex token_mutex;
		std::array<sse::Token, 2> tokens{};
		std::array<bool, 2> token_uses_api_key{};
		std::array<std::uint32_t, 2> token_key_revision{};
		std::size_t next_stream_slot = 0;
#endif
		std::function<Result(Context &)> home_callback, status_callback, config_callback, firmware_callback, key_callback, metrics_callback;
#if !NAWSERVER_DEMO_STATUS_WS
		std::function<Result(Context &)> events_callback;
#endif
		Route index;
		Route status_route;
		Route config_route;
		Route firmware_route;
		Route ui_status_route;
		Route ui_config_route;
		Route ui_firmware_route;
		Route ui_key_route;
		Route metrics_route;
#if !NAWSERVER_DEMO_STATUS_WS
		Route events_route;
		Route ui_events_route;
#endif
#if NAWSERVER_DEMO_STATUS_WS
		HandlerBase *handlers[10 + NAWSERVER_DEMO_TX_BULK_PROBE];
#else
		HandlerBase *handlers[11 + NAWSERVER_DEMO_TX_BULK_PROBE];
#endif
		HttpServer server;

		static BodyReceiver upload(void *context, HttpExchange &exchange)
		{
			char hash[65];
			const auto result = exchange.readHeader("X-Firmware-SHA256", hash, sizeof(hash));
			if(result.status != HeaderReadStatus::Ok || result.size != 64) return {};
			return static_cast<Platform *>(context)->beginFirmware(exchange.head().content_length, {hash, result.size});
		}

		BodyOptions firmwareBody()
		{
			return {BodyMode::Stream, std::max<std::size_t>(256, platform.maximumFirmwareBytes()), 1024, &platform, upload};
		}

		explicit Impl(TcpHttpBackend &tcp, Platform &device)
		  : platform(device), backend(tcp),
#if !NAWSERVER_DEMO_STATUS_WS
		    stream_backend(tcp, streamLimits()), stream(stream_backend),
#endif
		    home_callback([this](Context &context) { return home(context); }),
		    status_callback([this](Context &context) { return status(context); }),
		    config_callback([this](Context &context) { return configure(context); }),
		    firmware_callback([this](Context &context)
		    {
			    const auto result = context.response.text(202, "{\"ok\":true,\"rebooting\":true}");
			    if(result == SendResult::Ok) restart.store(true);
			    return responseResult(result);
		    }),
		    key_callback([this](Context &context) { return issueKey(context); }),
		    metrics_callback([this](Context &context) { return status(context, true); }),
#if !NAWSERVER_DEMO_STATUS_WS
		    events_callback([this](Context &context)
		    {
			    const bool uses_api_key = context.request.path() == "/api/events";
			    std::uint32_t revision = 0;
			    if(uses_api_key)
			    {
				    // Capture the revision before authentication. A concurrent rotation
				    // either invalidates this check or makes the admitted token stale.
				    revision = platform.keyRevision();
				    char key[65];
				    const auto read = context.request.readHeader("X-Client-Key", key, sizeof(key));
				    const bool allowed = read.status == HeaderReadStatus::Ok && platform.authorized({key, read.size});
				    std::memset(key, 0, sizeof(key));
				    if(!allowed) return responseResult(context.response.text(401, "API key required"));
			    }
			    const auto opened = stream.open(context.request, context.response);
			    if(opened.active)
			    {
				    detail::LockGuard<detail::Mutex> lock(token_mutex);
				    tokens[opened.token.slot] = opened.token;
				    token_uses_api_key[opened.token.slot] = uses_api_key;
				    token_key_revision[opened.token.slot] = revision;
				    return Result::Complete;
			    }
			    return responseResult(context.response.text(503, "Two status clients allowed"));
		    }),
#endif
		    index("/", HttpMethod::Get, EndpointRef(home_callback)),
		    status_route("/api/status", HttpMethod::Get, EndpointRef(status_callback)),
		    config_route("/api/config", HttpMethod::Post, EndpointRef(config_callback)),
		    firmware_route("/api/firmware", HttpMethod::Post, EndpointRef(firmware_callback), {}, {}, {}, {}, firmwareBody()),
		    ui_status_route("/ui/status", HttpMethod::Get, EndpointRef(status_callback)),
		    ui_config_route("/ui/config", HttpMethod::Post, EndpointRef(config_callback)),
		    ui_firmware_route("/ui/firmware", HttpMethod::Post, EndpointRef(firmware_callback), {}, {}, {}, {}, firmwareBody()),
		    ui_key_route("/ui/api-key", HttpMethod::Post, EndpointRef(key_callback)),
		    metrics_route("/ui/metrics", HttpMethod::Get, EndpointRef(metrics_callback)),
#if !NAWSERVER_DEMO_STATUS_WS
		    events_route("/api/events", HttpMethod::Get, EndpointRef(events_callback)),
		    ui_events_route("/ui/events", HttpMethod::Get, EndpointRef(events_callback)),
#endif
		    handlers{&index, &status_route, &config_route, &firmware_route,
			    &ui_status_route, &ui_config_route, &ui_firmware_route, &ui_key_route, &metrics_route,
#if NAWSERVER_DEMO_STATUS_WS
			    &stream
#else
			    &events_route, &ui_events_route
#endif
#if NAWSERVER_DEMO_TX_BULK_PROBE
			    , &txBulkProbeHandler()
#endif
		    }, server(tcp, handlers, Demo::limits(), {}, {}, protected_headers)
		{
		}

		Result home(Context &context)
		{
			char accept[256];
			const auto field = context.request.readHeader("Accept-Encoding", accept, sizeof(accept));
			if(field.status != HeaderReadStatus::Ok || std::string_view(accept, field.size).find("gzip") == std::string_view::npos)
				return responseResult(context.response.text(406, "gzip required"));
			if(context.response.header("Content-Encoding", "gzip") != SendResult::Ok ||
			    context.response.header("Cache-Control", "no-store") != SendResult::Ok) return Result::Error;
			return responseResult(context.response.sendImmutable(200, "text/html; charset=utf-8", {compact_web::gzip, compact_web::gzip_size}));
		}

		Result issueKey(Context &context)
		{
			keys::Secret secret;
			if(!platform.issueKey(secret))
			{
				secret.clear();
				return responseResult(context.response.text(503, "Key not saved"));
			}
			char bytes[80];
			const auto size = std::snprintf(bytes, sizeof(bytes), "{\"key\":\"%s\"}", secret.value.data());
			secret.clear();
			if(size <= 0 || static_cast<std::size_t>(size) >= sizeof(bytes) ||
			    context.response.header("Cache-Control", "no-store") != SendResult::Ok) return Result::Error;
			const auto result = context.response.send(201, "application/json",
			    {reinterpret_cast<const std::uint8_t *>(bytes), static_cast<std::size_t>(size)});
			std::memset(bytes, 0, sizeof(bytes));
			return responseResult(result);
		}

		std::size_t snapshot(char *bytes, std::size_t capacity, bool detailed = false)
		{
			const auto state = platform.status(); const auto settings = platform.settings();
			char name[49]; std::size_t used = 0;
			for(const unsigned char character : settings.name)
			{
				if(!character) break;
				if(character < 32 || character == 127) continue;
				if(character == '"' || character == '\\') name[used++] = '\\';
				name[used++] = static_cast<char>(character);
			}
			name[used] = 0;
			std::size_t offset = 0;
			bool valid = true;
			const auto append = [&](const char *format, auto... values)
			{
				if(!valid) return;
				const auto size = std::snprintf(bytes + offset, capacity - offset, format, values...);
				if(size < 0 || static_cast<std::size_t>(size) >= capacity - offset) { valid = false; return; }
				offset += static_cast<std::size_t>(size);
			};
			append("{\"name\":\"%s\",\"platform\":\"%s\",\"firmware\":\"%s\",\"running_slot\":\"%s\",\"uptime_ms\":%llu,\"free_heap\":%zu,\"min_free_heap\":%zu,\"rssi_dbm\":%d,\"led\":%s,\"interval_ms\":%lu,\"max_firmware_bytes\":%zu,\"firmware_format\":\"%s\"",
			    name, state.platform, state.firmware, state.running_slot, static_cast<unsigned long long>(state.uptime_ms),
			    state.free_heap, state.min_free_heap, state.rssi_dbm, settings.led ? "true" : "false",
			    static_cast<unsigned long>(settings.interval_ms), platform.maximumFirmwareBytes(), platform.firmwareFormat());
			char cpu[16] = "null";
			const bool cpu_available = state.cpu_available && state.cpu_percent >= 0 && state.cpu_percent <= 100;
			if(cpu_available)
			{
				const auto tenths = static_cast<unsigned>(state.cpu_percent * 10 + 0.5);
				// Avoid linking floating-point printf into the compact MCU image.
				std::snprintf(cpu, sizeof(cpu), "%u.%u", tenths / 10, tenths % 10);
			}
			append(",\"total_heap\":%zu,\"largest_free_block\":%zu,\"heap_scope\":\"%s\",\"cpu_percent\":%s,\"cpu_available\":%s,\"cpu_cores\":%lu,\"static_ram_bytes\":%zu,\"physical_ram_bytes\":%zu",
			    state.total_heap, state.largest_free_block, state.heap_scope, cpu, cpu_available ? "true" : "false",
			    static_cast<unsigned long>(state.cpu_cores), state.static_ram_bytes, state.physical_ram_bytes);
			if(state.secondary_heap_total)
				append(",\"secondary_heap_total\":%zu,\"secondary_heap_free\":%zu,\"secondary_heap_scope\":\"%s\"",
				    state.secondary_heap_total, state.secondary_heap_free, state.secondary_heap_scope);
			const auto tx = backend.transmissionStats();
			append(",\"stream_clients\":%zu,\"tx_bytes\":%zu,\"tx_peak_bytes\":%zu,\"tx_failures\":%zu,\"tx_memory_mode\":\"%s\",\"tx_reserved_bytes\":%zu,\"tx_limit_bytes\":%zu",
#if NAWSERVER_DEMO_STATUS_WS
			    stream.count(),
#else
			    stream.active(),
#endif
			    tx.allocated_bytes, tx.peak_bytes, tx.failed_jobs + tx.rejected_jobs,
			    NAWSERVER_DEMO_TX_MEMORY_MODE == 2 ? "static" : NAWSERVER_DEMO_TX_MEMORY_MODE == 1 ? "heap" : "lazy",
			    static_cast<std::size_t>(NAWSERVER_DEMO_TX_MEMORY_MODE ? NAWSERVER_DEMO_TX_BLOCKS * NAWSERVER_DEMO_TX_BLOCK_BYTES : 0), tx.maximum_bytes);
			if(detailed)
			{
				append(",\"tx_execution\":\"%s\"", tx.execution == tcp::TransmissionExecution::Cooperative ? "shared" : "worker");
				append(",\"tx_metadata_bytes\":%zu,\"tx_metadata_connections\":%zu,\"tx_metadata_jobs\":%zu", tx.metadata_bytes, tx.metadata_connection_capacity, tx.metadata_job_capacity);
				append(",\"tx_failed_jobs\":%zu,\"tx_rejected_jobs\":%zu", tx.failed_jobs, tx.rejected_jobs);
				append(",\"cpu_busy_us\":%llu,\"cpu_sample_us\":%llu",
				    static_cast<unsigned long long>(state.cpu_busy_us), static_cast<unsigned long long>(state.cpu_sample_us));
				append(",\"http_requests\":%lu,\"stream_records\":%lu",
				    static_cast<unsigned long>(http_requests.load()), static_cast<unsigned long>(stream_records.load()));
			}
			append("}");
			return valid ? offset : 0;
		}

		Result status(Context &context, bool detailed = false)
		{
			if(context.response.header("Cache-Control", "no-store") != SendResult::Ok) return Result::Error;
			char bytes[1024]; const auto size = snapshot(bytes, sizeof(bytes), detailed);
			if(!size) return Result::Error;
			return responseResult(context.response.send(200, "application/json", {reinterpret_cast<const std::uint8_t *>(bytes), size}));
		}

		Result configure(Context &context)
		{
			json::Limits limits{256, 128, 3, 1, 16};
			json::Document document(json_workspace, sizeof(json_workspace), limits);
			const auto body = context.request.body();
			if(document.parse({reinterpret_cast<const char *>(body.data()), body.size()}) != json::Error::None ||
			    document.root().type() != json::Type::Object || document.size(document.root()) != 3)
				return responseResult(context.response.text(400, "Invalid settings"));
			Settings settings; std::size_t size = 0; std::uint64_t interval = 0;
			if(!document.decode(document.member(document.root(), "name"), settings.name, sizeof(settings.name), size) ||
			    !size || !document.unsignedInteger(document.member(document.root(), "interval_ms"), interval) ||
			    interval < 1000 || interval > 60000 || !document.boolean(document.member(document.root(), "led"), settings.led))
				return responseResult(context.response.text(400, "Invalid settings"));
			for(unsigned char character : std::string_view(settings.name, size))
				if(character < 32 || character == 127) return responseResult(context.response.text(400, "Invalid name"));
			settings.interval_ms = static_cast<std::uint32_t>(interval);
			if(!platform.configure(settings)) return responseResult(context.response.text(503, "Settings not saved"));
			return status(context);
		}

		void tick()
		{
			platform.poll();
			const auto revision = platform.keyRevision();
			if(revision)
			{
#if !NAWSERVER_DEMO_STATUS_WS
				std::array<sse::Token, 2> revoked{};
				{
					detail::LockGuard<detail::Mutex> lock(token_mutex);
					for(std::size_t slot = 0; slot < tokens.size(); ++slot)
					{
						if(token_uses_api_key[slot] && token_key_revision[slot] != revision) revoked[slot] = tokens[slot];
					}
				}
				for(std::size_t slot = 0; slot < revoked.size(); ++slot)
				{
					if(revoked[slot].slot >= sse::client_capacity || !stream.close(revoked[slot])) continue;
					detail::LockGuard<detail::Mutex> lock(token_mutex);
					if(tokens[slot] == revoked[slot]) { tokens[slot] = {}; token_uses_api_key[slot] = false; }
				}
#endif
			}
			if(restart.load())
			{
				// Stop producing telemetry once the OTA reply is queued. Otherwise
				// a status client can keep extending the global transmit drain.
				const auto pending = backend.transmissionStats();
				if(!pending.queued_jobs && !pending.active_jobs && !pending.filling_jobs)
				{
					restart.store(false);
					platform.scheduleRestart();
				}
				return;
			}
			const auto now = platform.milliseconds();
			if(now < next) return;
			next = now + platform.settings().interval_ms;
#if NAWSERVER_DEMO_STATUS_WS
			if(!stream.count()) return;
#else
			if(!stream.active()) return;
#endif
#if NAWSERVER_DEMO_STATUS_WS
			char bytes[768]; const auto size = snapshot(bytes, sizeof(bytes));
			if(!size) return;
			(void)stream.send(bytes);
			stream_records.fetch_add(1);
#else
			std::array<sse::Token, 2> current;
			{ detail::LockGuard<detail::Mutex> lock(token_mutex); current = tokens; }
			sse::Frame frame;
			// snapshot emits a single JSON line, escaping names and rejecting controls.
			// Format directly into the owned SSE frame instead of a second 512-byte buffer.
			std::memcpy(frame.bytes.data(), "data: ", 6);
			const auto size = snapshot(frame.bytes.data() + 6, frame.bytes.size() - 8);
			if(!size) return;
			frame.bytes[6 + size] = '\n'; frame.bytes[7 + size] = '\n'; frame.size = size + 8;
			for(std::size_t offset = 0; offset < current.size(); ++offset)
			{
				const auto client = current[(next_stream_slot + offset) % current.size()];
				if(stream.publish(client, frame) == sse::Publish::Queued) stream_records.fetch_add(1);
			}
			next_stream_slot = (next_stream_slot + 1) % current.size();
#endif
		}
	};

	Demo::Demo(TcpHttpBackend &backend, Platform &platform) : impl_(nullptr)
	{
		// Some embedded default new implementations guarantee only four-byte
		// alignment; the bounded JSON workspace requires max_align_t alignment.
		std::size_t available = sizeof(Impl) + alignof(Impl) - 1;
		storage_ = ::operator new(available, std::nothrow);
		void *aligned = storage_;
		if(aligned && std::align(alignof(Impl), sizeof(Impl), aligned, available))
			impl_ = ::new(aligned) Impl(backend, platform);
	}
	Demo::~Demo()
	{
		if(impl_ && !stop()) return; // Retain owners while I/O cleanup is pending.
		if(impl_) impl_->~Impl();
		::operator delete(storage_);
	}
	bool Demo::start()
	{
		if(!impl_) return false;
#if !NAWSERVER_DEMO_STATUS_WS
		if(!impl_->stream.start()) return false;
#endif
		if(impl_->server.begin() == HttpError::Ok) return true;
#if !NAWSERVER_DEMO_STATUS_WS
		// The SSE worker was started first; don't retain it after a failed HTTP start.
		(void)impl_->stream.stop(3000);
#endif
		return false;
	}
	bool Demo::stop()
	{
		if(!impl_) return true;
#if !NAWSERVER_DEMO_STATUS_WS
		if(!impl_->stream.stop(3000)) return false;
#endif
		return impl_->server.end() == HttpError::Ok;
	}
	std::size_t Demo::activeStreams() const
	{
		if(!impl_) return 0;
#if NAWSERVER_DEMO_STATUS_WS
		return impl_->stream.count();
#else
		return impl_->stream.active();
#endif
	}
	void Demo::tick() { if(impl_) impl_->tick(); }
	TcpOptions Demo::options()
	{
		TcpOptions value;
		value.bind_address = "0.0.0.0"; value.port = 80; value.http_memory.enabled = true;
		value.http_memory.initial_requests = 1; value.http_memory.maximum_requests = NAWSERVER_DEMO_HTTP_JOBS;
		value.managed.ingress_slots = NAWSERVER_DEMO_INGRESS_SLOTS; value.managed.header_bytes = 1024;
		value.managed.header_timeout_ms = 5000; value.no_progress_timeout_ms = 5000;
		value.managed.response_body_bytes = 1024; value.managed.stream_bytes_per_client = 3072;
		value.buffer_memory.max_bytes = 24 * 1024;
#if NAWSERVER_DEMO_TX_INTERNAL_ONLY
		value.buffer_memory.allocator = {nullptr, allocateInternal, releaseEsp};
		value.transmission.allocator = {nullptr, allocateInternal, releaseEsp};
		value.transmission.metadata_allocator = {nullptr, allocateInternal, releaseEsp};
#endif
		value.max_websocket_clients = 1; value.websocket_tx_block_size = 512;
		value.websocket_tx_blocks = 1; value.keep_alive.enabled = false;
		value.transmission.enabled = true; value.transmission.maximum_bytes = NAWSERVER_DEMO_TX_BLOCKS * NAWSERVER_DEMO_TX_BLOCK_BYTES;
		value.transmission.execution = NAWSERVER_DEMO_TX_EXECUTOR ? tcp::TransmissionExecution::Cooperative : tcp::TransmissionExecution::DedicatedWorker;
		value.transmission.maximum_blocks = NAWSERVER_DEMO_TX_BLOCKS; value.transmission.block_bytes = NAWSERVER_DEMO_TX_BLOCK_BYTES;
		// This demo does not use priority TX jobs; make every block available.
		value.transmission.reserved_important_blocks = 0;
#if NAWSERVER_DEMO_TX_METADATA_MODE == 1
		value.transmission.metadata_allocator = {nullptr, allocateMetadata, releaseMetadata};
		value.transmission.metadata_max_bytes = sizeof(metadata_storage);
#elif NAWSERVER_DEMO_TX_METADATA_MODE == 2
		value.transmission.metadata_allocator = {nullptr, allocatePsram, releaseEsp};
#endif
#if NAWSERVER_DEMO_TX_PAYLOAD_PSRAM
		value.transmission.allocator = {nullptr, allocatePsram, releaseEsp};
#endif
#if NAWSERVER_DEMO_TX_MEMORY_MODE == 1
		value.transmission.initial_blocks = NAWSERVER_DEMO_TX_BLOCKS;
#elif NAWSERVER_DEMO_TX_MEMORY_MODE == 2
		value.transmission.static_payload = transmission_storage;
		value.transmission.static_payload_bytes = sizeof(transmission_storage);
#endif
		value.transmission.stall_timeout_ms = 3000; value.transmission.close_timeout_ms = 3000;
		value.transmission.step_bytes = 1024; value.transmission.maximum_blocks_per_connection = 1;
#if NAWSERVER_DEMO_TX_BULK_PROBE
		// One bounded generator fills real TX leases. Leave four queue entries
		// for two SSE sessions plus the API/page lanes in this fixture.
		value.managed.io_slice_bytes = 4096;
		value.managed.response_body_bytes = 4096;
		value.transmission.maximum_blocks_per_connection =
		    std::max<std::size_t>(1, NAWSERVER_DEMO_TX_BLOCKS > 4 ? NAWSERVER_DEMO_TX_BLOCKS - 4 : 1);
#endif
		return value;
	}
	HttpLimits Demo::limits()
	{
		HttpLimits value;
		value.max_connections = NAWSERVER_DEMO_CONNECTIONS; value.max_header_bytes = 1024;
		value.max_headers = 24; value.max_body_size = 256; value.max_target_size = 96;
		value.exchange_timeout_ms = NAWSERVER_DEMO_UPLOAD_TIMEOUT_MS;
		return value;
	}

}
