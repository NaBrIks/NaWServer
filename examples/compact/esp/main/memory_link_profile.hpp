// SPDX-License-Identifier: MIT
#pragma once

// These images are for linker accounting only. They deliberately omit OTA and
// storage management and must never replace a device's working application.
#include <NaWServer/handler.h>
#include <NaWServer/response.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdlib>
#if NAWSERVER_DEMO_MEMORY_LINK_PROFILE == 2
#include <esp_http_server.h>
#endif

namespace compact::measurement
{

	inline void linkProfile()
	{
#if NAWSERVER_DEMO_MEMORY_LINK_PROFILE == 2
		httpd_handle_t handle = nullptr;
		httpd_config_t config = HTTPD_DEFAULT_CONFIG();
		config.stack_size = 8192; config.max_open_sockets = 4;
		ESP_ERROR_CHECK(httpd_start(&handle, &config));
		httpd_uri_t route{}; route.uri = "/benchmark"; route.method = HTTP_GET;
		route.handler = [](httpd_req_t *request) { return httpd_resp_send(request, "OK", 2); };
		ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &route));
#elif NAWSERVER_DEMO_MEMORY_LINK_PROFILE >= 3
		class Handler final : public NaWServer::HttpRequestHandler
		{
		public:
			NaWServer::ExchangeDisposition handle(NaWServer::HttpExchange &exchange) override
			{
				NaWServer::Response response(exchange.responseTransport());
				return response.text(200, "OK") == NaWServer::SendResult::Ok ? NaWServer::ExchangeDisposition::Complete : NaWServer::ExchangeDisposition::Drop;
			}
		};
		static NaWServer::tcp::IdfTransport transport;
		if(transport.start().status != NaWServer::tcp::Status::Ok) std::abort();
		NaWServer::TcpOptions options;
		options.bind_address = "0.0.0.0"; options.port = 80;
		options.keep_alive.enabled = false;
		options.http_memory.enabled = NAWSERVER_DEMO_MEMORY_LINK_PROFILE == 4;
		options.http_memory.initial_requests = 1; options.http_memory.maximum_requests = 3;
		options.managed.ingress_slots = 2; options.managed.header_bytes = 1024;
		options.managed.response_body_bytes = 1024; options.buffer_memory.max_bytes = 24 * 1024;
		options.transmission.enabled = NAWSERVER_DEMO_MEMORY_LINK_PROFILE == 4;
		options.transmission.maximum_blocks = 3; options.transmission.maximum_bytes = 3072;
		options.transmission.block_bytes = 1024; options.transmission.reserved_important_blocks = 0;
		static NaWServer::TcpHttpBackend backend(transport, options);
		static Handler handler;
		NaWServer::HttpLimits limits;
		limits.max_connections = 4; limits.max_headers = 24; limits.max_header_bytes = 1024;
		limits.max_body_size = 256; limits.max_target_size = 96;
		if(backend.start(handler, limits) != NaWServer::HttpError::Ok) std::abort();
#endif
		for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
	}

}
