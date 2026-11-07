// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/mcp.h>
#if NAWSERVER_MCP
#include <cstdio>
#include <cstdint>
#include <string_view>

namespace NaWServer::demo
{
	// Host demo owns tool semantics and workspace; the shared MCP implementation
	// knows neither operating-system identity nor trusted listener addresses.
	class McpDemo
	{
		alignas(std::max_align_t) unsigned char reader_storage_[NAWSERVER_JSON_WORKSPACE_BYTES];
		char output_[NAWSERVER_MCP_OUTPUT_BYTES];
		mcp::Workspace workspace_{reader_storage_, sizeof(reader_storage_), output_, sizeof(output_)};
		std::atomic<std::uint16_t> port_{0};
		static mcp::ToolResult echo(void *, const json::Document &document,
		    json::Value arguments, json::Writer &writer)
		{
			const auto text = arguments ? document.member(arguments, "text") : NaWServer::json::Value{};
			constexpr std::size_t echo_text_bytes = 256;
			char decoded[echo_text_bytes]; std::size_t size;
			if(!text || !document.decode(text, decoded, sizeof(decoded), size)
			    || document.size(arguments) != 1)
				return mcp::ToolResult::InvalidArguments;

			writer.string({decoded, size});
			return mcp::ToolResult::Success;
		}

		static mcp::ToolResult device_info(void *, const json::Document &document,
		    json::Value arguments, json::Writer &writer)
		{
			if(arguments && document.size(arguments) != 0)
				return mcp::ToolResult::InvalidArguments;

#if defined(_WIN32)
			writer.string("{\"target\":\"windows-host\",\"sdk\":\"native C++17\",\"read_only\":true}");
#else
			writer.string("{\"target\":\"linux-host\",\"sdk\":\"native C++17\",\"read_only\":true}");
#endif
			return mcp::ToolResult::Success;
		}

		static bool trusted_origin(void *context, std::string_view origin)
		{
			const auto &self = *static_cast<McpDemo *>(context);
			const auto port = self.port_.load();
			if(!port) return false;

			char expected[64];
			const int length = std::snprintf(expected, sizeof(expected), "http://127.0.0.1:%u",
			        static_cast<unsigned>(port));
			return length > 0 && origin == std::string_view(expected, static_cast<std::size_t>(length));
		}

		inline static const mcp::Tool tools_[]{
			{"device_info", "Read host identity without changing the application",
			 "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}", device_info},
			{"echo", "Echo at most 256 UTF-8 bytes without changing the application",
			 "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"maxLength\":256,\"description\":\"At most 256 decoded UTF-8 bytes\"}},\"required\":[\"text\"],\"additionalProperties\":false}",
			 echo}};
	public:
		mcp::Endpoint endpoint{tools_, 2, workspace_, "/mcp", trusted_origin, this};
		// The real bound port is supplied after begin(), including ephemeral binds.
		void bound_port(std::uint16_t port)
		{
			port_.store(port);
		}

	};
}
#endif
