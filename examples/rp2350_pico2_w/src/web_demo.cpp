#include "web_demo.hpp"
#include "storage.hpp"
#include "api_keys.hpp"
#include "pico_transport.hpp"
#include "demo_assets.hpp"
#include "showcase_api.hpp"
#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/http_server.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/route.h>
#include <NaWServer/request.h>
#include <NaWServer/websocket.h>
#include <NaWServer/sse.h>
#include "../../common/sse_test.hpp"
#include <FreeRTOS.h>
#include <task.h>
#include "pico/time.h"
#include "lwip/opt.h"
#include <array>
#include <atomic>
#ifndef NAWSERVER_DEMO_FILE_READERS
#define NAWSERVER_DEMO_FILE_READERS 2
#endif
#include <cstdio>
#include <cstring>

namespace rp_demo
{
	using namespace NaWServer;
	namespace
	{
		tcp::PicoTransport transport;
		TcpOptions makeOptions()
		{
			TcpOptions options;
			options.bind_address = "0.0.0.0";
			options.port = http_port;
			options.backlog = 6;
			options.http_memory.enabled = true;
			options.http_memory.initial_requests = NAWSERVER_DEMO_HTTP_INITIAL_REQUESTS;
            options.http_memory.reserved_requests = NAWSERVER_DEMO_HTTP_RESERVED_REQUESTS;
			options.http_memory.maximum_requests = NAWSERVER_DEMO_HTTP_MAXIMUM_REQUESTS;
			options.managed.header_bytes = 2048;
			options.managed.response_body_bytes = 8192; // Match the demo CBOR writer capacity.
			options.keep_alive.enabled = true;
			options.keep_alive.maximum_idle = 2;
			options.max_websocket_clients = NAWSERVER_DEMO_SHARED_CLIENTS;
			options.websocket_tx_block_size = 4096;
			options.websocket_tx_blocks = 4;
			// HTTP workspaces plus up to four lazy 16 KiB WS arenas. TX has its own 32 KiB budget.
			options.buffer_memory.max_bytes = 160 * 1024;
			options.transmission.enabled = true;
			options.transmission.block_bytes = 4 * TCP_MSS;
			options.transmission.step_bytes = TCP_MSS;
			options.transmission.maximum_bytes = 32 * 1024;
			options.transmission.maximum_blocks = 12;
			return options;
		}
		TcpHttpBackend backend(transport, makeOptions());
		sse::TcpAdapter event_backend(backend);
		sse::EventSource events(event_backend);
		FixedBasicAuthenticator users("demo", "showcase-only", 1);
		AuthMiddleware authentication(users);
		AccessMiddleware authorization;
		auto private_chain = makeHeaderChain(authentication, authorization);
		examples::SseTest event_test(events, private_chain);
		WebSocketLimits makeWebSocketLimits()
		{
			WebSocketLimits limits;
			limits.max_message_size = 4096;
			limits.max_pending_messages = 2;
			limits.max_clients = websocket_client_limit;
			limits.max_pending_bytes = 8192;
			return limits;
		}
		WebSocketEndpoint websocket("/ws", makeWebSocketLimits());

		struct AssetReader
		{
			lfs_file_t file{};
			std::atomic<bool> active{false};
		};
		static_assert(NAWSERVER_DEMO_FILE_READERS > 0);
		std::array<AssetReader, NAWSERVER_DEMO_FILE_READERS> asset_readers;

		class AssetHandler final : public HandlerBase
		{
			static BodyReadResult readAsset(void *context, std::size_t offset, std::uint8_t *bytes, std::size_t capacity)
			{
				std::size_t size = 0;
				return storage().readPart(static_cast<AssetReader *>(context)->file, offset, bytes, capacity, size) ?
				    BodyReadResult{size ? BodyReadStatus::Data : BodyReadStatus::End, size} :
				    BodyReadResult{BodyReadStatus::TransportError, 0};
			}
			static void releaseAsset(void *context)
			{
				auto &reader = *static_cast<AssetReader *>(context);
				storage().closeRead(reader.file);
				reader.active.store(false, std::memory_order_release);
			}
		public:
			RouteDescriptors routes() const override
			{
				static const auto descriptors = []
				{
					constexpr std::size_t count = sizeof(seed_assets) / sizeof(seed_assets[0]);
					std::array<RouteDescriptor, count * 2 + 2> values{};
					for(std::size_t i = 0; i < count; ++i)
					{
						values[i * 2] = {seed_assets[i].path, HttpMethod::Get};
						values[i * 2 + 1] = {seed_assets[i].path, HttpMethod::Head};
					}
					values[count * 2] = {"/", HttpMethod::Get};
					values[count * 2 + 1] = {"/", HttpMethod::Head};
					return values;
				}();
				return {descriptors.data(), descriptors.size()};
			}

			void handleRequest(Request *request, Response &response) override
			{
				const auto path = request->path() == "/" ? std::string_view("/index.html") : request->path();
				for(const auto &asset : seed_assets)
				{
					if(path != asset.path) continue;

					AssetReader *reader = nullptr;
					for(auto &candidate : asset_readers)
					{
						bool free = false;
						if(candidate.active.compare_exchange_strong(free, true, std::memory_order_acquire))
						{
							reader = &candidate;
							break;
						}
					}
					if(!reader)
					{
						(void)response.text(503, "File readers busy");
						return;
					}
					if(response.header("Content-Encoding", "gzip") != SendResult::Ok ||
						response.header("Vary", "Accept-Encoding") != SendResult::Ok)
					{
						reader->active.store(false, std::memory_order_release);
						return;
					}
					reader->file = {};
					std::size_t size = 0;
					if(storage().openRead(asset.storage_path, reader->file, size))
						(void)response.sendOwnedSource(200, asset.mime, {{reader, size, readAsset}, releaseAsset});
					else
					{
						reader->active.store(false, std::memory_order_release);
						// Recovery UI remains available when FS is not initialized.
						(void)response.sendImmutable(200, asset.mime, {asset.bytes, asset.size});
					}
					return;
				}
			}
		} asset_handler;

		SendResult status(Response &response)
		{
			char report[768];
			const auto settings = storage().settings();
			const int size = std::snprintf(report, sizeof(report),
				"{\"ok\":true,\"board\":\"Pico 2 W\",\"rtos\":\"FreeRTOS\","
				"\"uptime_ms\":%llu,\"rtos_free_heap\":%u,\"rtos_min_heap\":%u,"
				"\"fs_mounted\":%s,\"fs_bytes\":%u,\"fs_used_bytes\":%u,"
				"\"nvs_bytes\":%u,\"nvs_generation\":%llu,\"boot_count\":%u,\"save_count\":%u,\"name\":\"%s\"}",
				static_cast<unsigned long long>(time_us_64() / 1000),
				unsigned(xPortGetFreeHeapSize()), unsigned(xPortGetMinimumEverFreeHeapSize()),
				storage().filesystemMounted() ? "true" : "false", unsigned(filesystem_bytes),
				unsigned(storage().filesystemUsedBytes()), unsigned(settings_bytes),
				static_cast<unsigned long long>(storage().settingsGeneration()),
				unsigned(settings.boot_count), unsigned(settings.save_count), settings.name);
			if(size < 0 || std::size_t(size) >= sizeof(report)) return response.text(500, "Status capacity exceeded");
			return response.send(200, "application/json", {reinterpret_cast<const std::uint8_t *>(report), std::size_t(size)});
		}

		SendResult echo(Request &request, Response &response)
		{
			if(!request.body().empty()) return response.send(200, "application/octet-stream", request.body());
			const auto value = request.parameter("value").value_or(std::string_view{});
			return response.send(200, "application/octet-stream", {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
		}

		SendResult parameters(Request &request, Response &response)
		{
			const auto value = request.parameter("value").value_or(std::string_view{});
			return response.send(200, "text/plain; charset=utf-8",
				{reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
		}

		SendResult privatePage(Response &response)
		{
			return response.text(200, "private");
		}
		SendResult redirect(Response &response)
		{
			return response.redirect("/#lab");
		}

		SendResult saveSettings(Request &request, Response &response)
		{
			auto settings = storage().settings();
			if(const auto name = request.parameter("name"))
			{
				if(name->empty() || name->size() >= sizeof(settings.name)) return response.text(400, "Invalid name length");
				for(const auto character : *name)
					if(static_cast<unsigned char>(character) < 32 || character == '"' || character == '\\')
						return response.text(400, "Invalid name character");
				std::memcpy(settings.name, name->data(), name->size());
				settings.name[name->size()] = 0;
			}
			++settings.save_count;
			if(!storage().saveSettings(settings)) return response.text(500, "NVS commit failed");
			return status(response);
		}

		        #if NAWSERVER_DEMO_API_KEYS
        auto keys_handler = [](Context &context){ return api_keys.http(context); };
        Route keys_get("/api/v2/keys", HttpMethod::Get, EndpointRef(keys_handler), {}, AccessPolicy::authenticated(), {}, private_chain);
        Route keys_post("/api/v2/keys", HttpMethod::Post, EndpointRef(keys_handler), {}, AccessPolicy::authenticated(), {}, private_chain);
        Admission<NAWSERVER_DEMO_SHARED_CLIENTS> client_policy(nawserver_demo_keys::admissionOptions());
        std::uint64_t keyClock(){return time_us_64()/1000;}
        AdmissionMiddleware key_admission(AdmissionRef(client_policy),keyClock,"X-Client-Key",nawserver_demo_keys::Service::resolve,&api_keys,true);
        auto global_chain=makeHeaderChain(key_admission);
#else
        HeaderPipeline global_chain{};
#endif
        Route health_route("/health", HttpMethod::Get, status);
		Route info_route("/api/v2/info", HttpMethod::Get, showcase::info);
		Route summary_route("/api/v2/summary", HttpMethod::Get, showcase::summary);
		Route tasks_route("/api/v2/tasks", HttpMethod::Get, showcase::tasks);
		Route api_echo_route("/api/v2/echo", HttpMethod::Any, showcase::echo);
		Route stream_route("/api/v2/stream", HttpMethod::Get, showcase::stream);
		Route redirect_route("/go", HttpMethod::Get, redirect);
		Route auth_route("/api/v2/auth/status", HttpMethod::Get, privatePage, {}, AccessPolicy::require(1), {}, private_chain);
		Route admin_route("/api/v2/auth/admin", HttpMethod::Get, privatePage, {}, AccessPolicy::require(2), {}, private_chain);
		Route led_route("/api/v2/led", HttpMethod::Post, showcase::command, {}, AccessPolicy::require(1), {}, private_chain);
		Route experiments_route("/api/v2/experiments", HttpMethod::Post, showcase::command, {}, AccessPolicy::require(1), {}, private_chain);
		Route control_route("/api/v2/control", HttpMethod::Post, showcase::command, {}, AccessPolicy::require(1), {}, private_chain);
		SendResult (*open_events)(Request &, Response &) = [](Request &request, Response &response)
		{
			return showcase::openEvents(events, request, response);
		};
		Route events_route("/api/v2/events", HttpMethod::Get, open_events);
		Route private_events_route("/api/v2/events/private", HttpMethod::Get, open_events, {}, AccessPolicy::require(1), {}, private_chain);
		Route status_route("/api/status", HttpMethod::Get, status);
		Route storage_route("/api/storage", HttpMethod::Get, status);
		Route settings_route("/api/settings", HttpMethod::Get, status);
		Route save_route("/api/settings", HttpMethod::Post, saveSettings);
		Route echo_route("/echo", HttpMethod::Post, echo);
		Route parameters_route("/parameters", HttpMethod::Any, parameters);
		Route private_route("/private", HttpMethod::Get, privatePage, {}, AccessPolicy::authenticated(), {}, private_chain);
		HandlerBase *const handlers[]{
#if NAWSERVER_DEMO_API_KEYS
            &keys_get,&keys_post,
#endif
            &health_route, &status_route, &storage_route,
			&info_route, &summary_route, &tasks_route, &api_echo_route, &stream_route, &redirect_route,
			&auth_route, &admin_route, &led_route, &experiments_route, &control_route, &events_route, &private_events_route,
			&settings_route, &save_route, &echo_route, &parameters_route, &private_route, &websocket,
			&event_test.open_route, &event_test.private_route, &event_test.publish_route,
			&event_test.close_route, &event_test.stats_route, &event_test.stop_route, &asset_handler};
		HttpLimits makeLimits()
		{
			HttpLimits limits;
			limits.max_connections = http_connection_limit;
            limits.max_body_size = NAWSERVER_DEMO_HTTP_BODY_BYTES;
			limits.max_header_bytes = 2048;
			limits.max_headers = 24;
			limits.max_target_size = 512;
			return limits;
		}
		HttpServer server(backend, handlers, makeLimits(), {}, {}, global_chain);
	}

	NaWServer::tcp::TransmissionStats transmissionSnapshot()
	{
		return backend.transmissionStats();
	}

	bool startWebServer()
	{
		#if NAWSERVER_DEMO_API_KEYS
        printf("KEY_STORE_READY=%u\n",api_keys.initialize());
#endif
        printf("WEB configure callbacks\n");
		websocket.onMessage(showcase::message);
		printf("WEB start transport\n");
		if(transport.start().status != tcp::Status::Ok) return false;
		printf("WEB start SSE\n");
		if(!events.start()) { printf("WEB SSE failed\n"); return false; }
		printf("WEB start HTTP\n");
		const auto result = server.begin();
		printf("HTTP_START result=%d port=%u\n", int(result), unsigned(http_port));
		return result == HttpError::Ok;
	}

	void pollWebDemo()
	{
		event_test.poll();
		if(event_test.stop_requested.exchange(false)) (void)events.stop(1000);
		showcase::poll(events, websocket);
	}
}
