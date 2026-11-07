// SPDX-License-Identifier: MIT
#pragma once

// Application-owned diagnostic harness. The library and the SDK HTTPD are
// exercised through their normal APIs; neither allocator is replaced.
#include <NaWServer/handler.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <NaWServer/response.h>
#include <esp_heap_caps.h>
#include <esp_heap_trace.h>
#include <esp_http_server.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <unistd.h>

namespace compact::measurement
{

	inline void require(bool condition) { if(!condition) std::abort(); }
	inline const char *variant = "boot";
	inline EspPlatform *platform = nullptr;
#if CONFIG_HEAP_TRACING_STANDALONE
	inline heap_trace_record_t records[256]{};
#endif
	inline TaskStatus_t tasks[32]{};
	inline std::atomic<unsigned> requests{0};

	inline void snapshot(const char *phase)
	{
		multi_heap_info_t internal{}, external{}, dma{}, executable{};
		heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		heap_caps_get_info(&external, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		heap_caps_get_info(&dma, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
		heap_caps_get_info(&executable, MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC);
		printf("BRICK variant=%s phase=%s free=%u allocated=%u largest=%u minimum=%u blocks=%u psram_free=%u psram_allocated=%u dma_free=%u exec_free=%u tasks=%u requests=%u\n",
		    variant, phase, unsigned(internal.total_free_bytes), unsigned(internal.total_allocated_bytes),
		    unsigned(internal.largest_free_block), unsigned(internal.minimum_free_bytes), unsigned(internal.allocated_blocks),
		    unsigned(external.total_free_bytes), unsigned(external.total_allocated_bytes),
		    unsigned(dma.total_free_bytes), unsigned(executable.total_free_bytes), unsigned(uxTaskGetNumberOfTasks()), requests.load());
	}

	inline void taskSnapshot(const char *phase)
	{
		configRUN_TIME_COUNTER_TYPE elapsed = 0;
		const auto count = uxTaskGetSystemState(tasks, 32, &elapsed);
		for(UBaseType_t i = 0; i < count; ++i)
			printf("BRICK_TASK variant=%s phase=%s name=%s watermark=%u\n", variant, phase,
			    tasks[i].pcTaskName, unsigned(tasks[i].usStackHighWaterMark));
	}

	inline void begin(const char *phase)
	{
		ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_start());
		// Balance the SDK monitor's own bookkeeping in both phase snapshots.
		snapshot(phase);
#if CONFIG_HEAP_TRACING_STANDALONE
		ESP_ERROR_CHECK(heap_trace_start(std::strcmp(phase, "single_before") == 0 ? HEAP_TRACE_ALL : HEAP_TRACE_LEAKS));
#endif
	}

	inline void end(const char *phase)
	{
#if CONFIG_HEAP_TRACING_STANDALONE
		ESP_ERROR_CHECK(heap_trace_stop());
#endif
		snapshot(phase);
		ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_stop());
#if CONFIG_HEAP_TRACING_STANDALONE
		heap_trace_summary_t summary{};
		ESP_ERROR_CHECK(heap_trace_summary(&summary));
		printf("BRICK_TRACE variant=%s phase=%s count=%u high=%u overflow=%u allocations=%u frees=%u\n", variant, phase,
		    unsigned(summary.count), unsigned(summary.high_water_mark), unsigned(summary.has_overflowed),
		    unsigned(summary.total_allocations), unsigned(summary.total_frees));
		for(std::size_t i = 0; i < summary.count; ++i)
		{
			heap_trace_record_t record{};
			ESP_ERROR_CHECK(heap_trace_get(i, &record));
			printf("BRICK_ALLOC variant=%s phase=%s address=%p bytes=%u freed=%u external=%u pcs=", variant, phase,
			    record.address, unsigned(record.size), unsigned(record.freed),
			    unsigned(esp_ptr_external_ram(record.address)));
			for(const auto pc : record.alloced_by) printf("%p,", pc);
			printf("\n");
		}
#endif
	}

	inline void pause(unsigned milliseconds, Demo *demo = nullptr)
	{
		const auto until = esp_timer_get_time() + milliseconds * 1000ULL;
		while(std::uint64_t(esp_timer_get_time()) < until)
		{
			if(demo) demo->tick(); else platform->poll();
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}

	inline void window(const char *name, unsigned milliseconds = 5000, Demo *demo = nullptr)
	{
		begin(std::strcmp(name, "single") == 0 ? "single_before" : "exchange_before");
		printf("BRICK_READY variant=%s window=%s port=%u\n", variant, name, demo ? 80u : 81u);
		pause(milliseconds, demo);
		end(name);
		taskSnapshot(name);
	}

	inline esp_err_t nativeGet(httpd_req_t *request)
	{
		++requests;
		if(requests.load() == 1) snapshot("get_active");
		httpd_resp_set_type(request, "text/plain");
		httpd_resp_set_hdr(request, "Connection", "close");
		const auto result = httpd_resp_send(request, "OK", 2);
		if(requests.load() == 1) snapshot("get_sent");
		return result;
	}

	class GetHandler final : public NaWServer::HttpRequestHandler
	{
	public:
		NaWServer::ExchangeDisposition handle(NaWServer::HttpExchange &exchange) override
		{
			++requests;
			if(requests.load() == 1) snapshot("get_active");
			NaWServer::Response response(exchange.responseTransport());
			const auto result = response.text(200, "OK");
			if(requests.load() == 1) snapshot("get_sent");
			return result == NaWServer::SendResult::Ok ? NaWServer::ExchangeDisposition::Complete : NaWServer::ExchangeDisposition::Drop;
		}
	};

	inline void run(EspPlatform &native_platform)
	{
		platform = &native_platform;
#if CONFIG_HEAP_TRACING_STANDALONE
		ESP_ERROR_CHECK(heap_trace_init_standalone(records, 256));
#endif
		printf("BRICK_CONFIG tracing=%u trace_storage=%u task_storage=%u backend_size=%u demo_size=%u transport_size=%u\n",
#if CONFIG_HEAP_TRACING_STANDALONE
		    1u,
		    unsigned(sizeof(records)),
#else
		    0u, 0u,
#endif
		    unsigned(sizeof(tasks)), unsigned(sizeof(NaWServer::TcpHttpBackend)), unsigned(sizeof(Demo)),
		    unsigned(sizeof(NaWServer::tcp::IdfTransport)));
		pause(3000);
		snapshot("network_idle");
#if CONFIG_HEAP_TRACING_STANDALONE
		constexpr unsigned cycles = 1;
#else
		constexpr unsigned cycles = 2;
#endif
		for(unsigned cycle = 0; cycle < cycles; ++cycle)
		{
			printf("BRICK_CYCLE value=%u\n", cycle);
			variant = "raw_socket"; requests = 0;
			begin("before");
			const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			require(listener >= 0);
			int reuse = 1; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
			sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(81);
			require(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
			require(listen(listener, 4) == 0);
			end("listener_started");
			printf("BRICK_READY variant=%s window=single port=81\n", variant);
			fd_set set; FD_ZERO(&set); FD_SET(listener, &set); timeval timeout{8, 0};
			if(select(listener + 1, &set, nullptr, nullptr, &timeout) > 0)
			{
				begin("exchange_before");
				const auto client = accept(listener, nullptr, nullptr);
				if(client >= 0)
				{
					char head[256]; (void)recv(client, head, sizeof(head), 0);
					++requests; snapshot("get_active");
					constexpr char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
					(void)send(client, response, sizeof(response) - 1, 0);
					snapshot("get_sent"); close(client);
				}
				pause(1000); end("single");
			}
			close(listener); pause(1000); snapshot("stopped");

			variant = "httpd"; requests = 0;
			begin("before");
			httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.server_port = 81;
			config.stack_size = 8192; config.max_open_sockets = 4;
			httpd_handle_t native = nullptr;
			ESP_ERROR_CHECK(httpd_start(&native, &config));
			end("engine_started");
			begin("route_before");
			httpd_uri_t route{}; route.uri = "/benchmark"; route.method = HTTP_GET; route.handler = nativeGet;
			ESP_ERROR_CHECK(httpd_register_uri_handler(native, &route));
			pause(1000); end("route_registered");
			window("single"); window("concurrent"); window("headers");
			ESP_ERROR_CHECK(httpd_stop(native)); pause(1000); snapshot("stopped");

			for(unsigned mode = 0; mode < 4; ++mode)
			{
				variant = mode == 0 ? "naw_plain" : mode == 1 ? "naw_managed" : mode == 2 ? "naw_tx" : "naw_routes";
				requests = 0;
				begin("before");
				auto *transport = new(std::nothrow) NaWServer::tcp::IdfTransport;
				require(transport && transport->start().status == NaWServer::tcp::Status::Ok);
				end("transport_created");
				auto options = Demo::options(); options.port = 81;
				options.http_memory.enabled = mode != 0;
				options.transmission.enabled = mode >= 2;
				auto limits = Demo::limits(); limits.max_connections = 4;
				begin("backend_before");
				auto *backend = new(std::nothrow) NaWServer::TcpHttpBackend(*transport, options);
				require(backend); end("backend_constructed");
				GetHandler handler;
				auto endpoint = [](NaWServer::Context &context) { ++requests; return NaWServer::responseResult(context.response.text(200, "OK")); };
				begin("route_before");
				auto *naw_route = new(std::nothrow) NaWServer::Route("/benchmark", NaWServer::HttpMethod::Get, NaWServer::EndpointRef(endpoint));
				NaWServer::HandlerBase *handlers[]{naw_route};
				auto *server = new(std::nothrow) NaWServer::HttpServer(*backend, handlers, limits);
				require(naw_route && server); end("route_constructed");
				begin("start_before");
				require((mode == 3 ? server->begin() : backend->start(handler, limits)) == NaWServer::HttpError::Ok);
				pause(1000); end("engine_started");
				window("single"); window("concurrent"); window("headers");
				const auto stats = backend->transmissionStats();
				printf("BRICK_TX variant=%s metadata=%u payload=%u\n", variant, unsigned(stats.metadata_bytes), unsigned(stats.allocated_bytes));
				require((mode == 3 ? server->end() : backend->stop()) == NaWServer::HttpError::Ok);
				delete server; delete naw_route; delete backend; delete transport;
				pause(1000); snapshot("stopped");
			}
		}
		variant = "full_demo"; requests = 0;
		begin("before");
		auto *transport = new(std::nothrow) NaWServer::tcp::IdfTransport;
		require(transport && transport->start().status == NaWServer::tcp::Status::Ok);
		end("transport_created");
		begin("backend_before");
		auto *backend = new(std::nothrow) NaWServer::TcpHttpBackend(*transport, Demo::options());
		require(backend); end("backend_constructed");
		begin("demo_before");
		auto *demo = new(std::nothrow) Demo(*backend, native_platform);
		require(demo); end("demo_constructed");
		begin("start_before"); require(demo->start()); pause(1000, demo); end("engine_started");
		window("single", 5000, demo); window("streams", 15000, demo);
		window("page_api", 15000, demo);
		printf("BRICK_OTA_READY\n");
		begin("ota_before");
		for(unsigned tick = 0; tick < 900; ++tick)
		{
			pause(100, demo);
			snapshot("ota_tick");
		}
		end("ota_timeout");
		require(demo->stop()); delete demo; delete backend; delete transport;
		pause(1000); snapshot("stopped");
		printf("BRICK_COMPLETE\n");
	}

}
