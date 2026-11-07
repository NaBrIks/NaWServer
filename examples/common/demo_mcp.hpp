// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/mcp.h>
#include "demo_mcp_experiments.hpp"
#include "demo_mcp_tools.hpp"
#if NAWSERVER_MCP
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cstddef>
#if CONFIG_IDF_TARGET_ESP32S3
#include "esp_heap_caps.h"
#endif

// Application-owned demo tools and storage. No product/hardware knowledge enters
// the generic registry. The application protects the complete MCP endpoint with
// the same HTTP Basic route policy as its existing command API.
namespace nawserver_demo_mcp
{
	inline NaWServer::mcp::Workspace workspace;
#if !CONFIG_IDF_TARGET_ESP32S3
	// One fixed parser arena per application endpoint; no per-client allocation.
	alignas(std::max_align_t) inline unsigned char reader_storage[NAWSERVER_JSON_WORKSPACE_BYTES];
	inline char output[NAWSERVER_MCP_OUTPUT_BYTES];
#endif
	inline NaWServer::mcp::ToolResult echo(void *, const NaWServer::json::Document &document,
	    NaWServer::json::Value arguments, NaWServer::json::Writer &writer)
	{
		const auto text = arguments ? document.member(arguments, "text") : NaWServer::json::Value{};
		// This is the echo tool contract, not the parser arena or request budget.
		constexpr std::size_t echo_text_bytes = 256;
		char decoded[echo_text_bytes]; std::size_t size;
		if(!text || !document.decode(text, decoded, sizeof(decoded), size))
			return NaWServer::mcp::ToolResult::InvalidArguments;

		// additionalProperties:false is enforced by the application, not a
		// general-purpose JSON Schema engine in the protocol component.
		if(document.size(arguments) != 1)
			return NaWServer::mcp::ToolResult::InvalidArguments;

		writer.string({decoded, size});
		return NaWServer::mcp::ToolResult::Success;
	}

	inline NaWServer::mcp::ToolResult device_info(void *, const NaWServer::json::Document &document,
	    NaWServer::json::Value arguments, NaWServer::json::Writer &writer)
	{
		if(arguments && document.size(arguments) != 0)
			return NaWServer::mcp::ToolResult::InvalidArguments;

		const auto *application = esp_app_get_description();
		constexpr std::size_t device_summary_bytes = 256;
		char summary[device_summary_bytes];
		int size = std::snprintf(summary, sizeof(summary),
		        "{\"target\":\"%s\",\"sdk\":\"%s\",\"uptime_ms\":%llu,\"free_heap_bytes\":%lu,\"read_only\":true}",
		        CONFIG_IDF_TARGET, application->idf_ver,
		        static_cast<unsigned long long>(esp_timer_get_time() / 1000),
		        static_cast<unsigned long>(esp_get_free_heap_size()));
		if(size < 0 || static_cast<std::size_t>(size) >= sizeof(summary)) return NaWServer::mcp::ToolResult::Failed;

		writer.string({summary, static_cast<std::size_t>(size)});
		return NaWServer::mcp::ToolResult::Success;
	}

	inline bool trusted_origin(void *, std::string_view origin)
	{
		// Compare against live local interface addresses, never the request Host.
		for(const char *key : {"WIFI_STA_DEF", "WIFI_AP_DEF"})
		{
			esp_netif_t *network = esp_netif_get_handle_from_ifkey(key);
			esp_netif_ip_info_t address{};
			if(!network || esp_netif_get_ip_info(network, &address) != ESP_OK || !address.ip.addr) continue;

			char expected[64];
			int length = std::snprintf(expected, sizeof(expected), "http://" IPSTR, IP2STR(&address.ip));
			if(length > 0 && origin == std::string_view(expected, static_cast<std::size_t>(length))) return true;

			if(length > 0 && static_cast<std::size_t>(length) + 3 < sizeof(expected))
			{
				std::memcpy(expected + length, ":80", 3);
				if(origin == std::string_view(expected, static_cast<std::size_t>(length) + 3)) return true;
			}
		}
		return false;
	}

	template <class Experiments>
	inline NaWServer::mcp::Endpoint endpoint(tools<Experiments, device_info, echo>, 4, workspace,
	    "/mcp", trusted_origin);
	// Minimal has no experiment runtime. Instantiate this separate static profile
	// only when used, so full demos do not acquire an unused second endpoint.
	template <class = void>
	inline NaWServer::mcp::Endpoint read_only_endpoint(read_only_tools<device_info, echo>, 2, workspace,
	    "/mcp", trusted_origin);
	inline bool initialize()
	{
#if CONFIG_IDF_TARGET_ESP32S3
		// Shared request storage uses external RAM; the endpoint's atomic guard
		// stays in ordinary internal static memory. No fallback consumes the
		// scarce internal Wi-Fi/TCP reserve if external RAM is unavailable.
		if(workspace.reader_storage && workspace.output) return true;

		void *storage = heap_caps_aligned_alloc(alignof(std::max_align_t),
		        NAWSERVER_JSON_WORKSPACE_BYTES + NAWSERVER_MCP_OUTPUT_BYTES,
		        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if(!storage) return false;

		// Capability malloc alone guarantees only four bytes; request the JSON
		// contract's alignment explicitly. Output holds chars and
		// needs no extra alignment after the exact configured reader arena size.
		workspace = {storage, NAWSERVER_JSON_WORKSPACE_BYTES,
			         static_cast<char *>(storage) + NAWSERVER_JSON_WORKSPACE_BYTES, NAWSERVER_MCP_OUTPUT_BYTES};
#else
		workspace = {reader_storage, sizeof(reader_storage), output, sizeof(output)};
#endif
		return true;
	}

}
#endif
