// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/mcp_config.h>
#if NAWSERVER_MCP
#include <NaWServer/handler.h>
#include <NaWServer/json.h>
#include <atomic>

namespace NaWServer::mcp
{
	inline constexpr std::string_view protocol_version = "2026-07-28";
	enum class ToolResult { Success, InvalidArguments, Failed };
	// Callback emits one JSON string with Writer::string(), not a raw JSON value.
	// It completes synchronously; borrowed arguments may never be queued as views.
	using ToolCallback = ToolResult (*)(void *, const json::Document &, json::Value, json::Writer &);
	struct Tool
	{
		const char *name;
		const char *description;
		const char *input_schema;
		ToolCallback callback;
		void *context{};
		bool read_only = true;
	};
	using OriginPolicy = bool (*)(void *, std::string_view);
	struct Workspace
	{
		// Caller owns max_align_t-aligned opaque JSON arena and complete output.
		void *reader_storage{};
		std::size_t reader_storage_bytes{};
		char *output{};
		std::size_t output_capacity{};
	};
	// Per-endpoint request budgets. Defaults come from json_config.h; changing a
	// limit never allocates memory. Storage and output capacities are supplied
	// separately by the owner, so a larger limit still fails on a smaller arena.
	struct Config
	{
		json::Limits limits{};
	};
	struct Reply
	{
		int status;
		std::string_view body;
	};
	// One bounded workspace per endpoint, not per client. Concurrent requests
	// receive 503; the application may wrap this handler in its usual Route policy.
	class Endpoint : public HandlerBase
	{
		const Tool *tools_;
		std::size_t tool_count_;
		Workspace &workspace_;
		Config config_;
		RouteDescriptor descriptor_;
		OriginPolicy origin_policy_;
		void *origin_context_;
		std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
		Reply process(std::string_view input, std::string_view version,
		    std::string_view method, std::string_view name, bool http, bool invalid_headers);

	public:
		Endpoint(const Tool *tools, std::size_t count, Workspace &workspace, const char *path = "/mcp",
		    OriginPolicy origin_policy = nullptr, void *origin_context = nullptr, Config config = {});
		const Config &config() const
		{
			return config_;
		}

		RouteDescriptors routes() const override { return {&descriptor_, 1}; }
		void handleRequest(Request *request, Response &response) override;
		// Pure wire entry for host corpus. Caller serializes access and must consume
		// the returned borrowed reply before another dispatch/HTTP request.
		Reply dispatch(std::string_view input, std::string_view version = {},
		    std::string_view method = {}, std::string_view name = {}, bool http = false, bool invalid_headers = false);
	};
}
#endif
