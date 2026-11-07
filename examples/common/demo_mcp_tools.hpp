// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/mcp.h>
#if NAWSERVER_MCP
namespace nawserver_demo_mcp
{
	template <NaWServer::mcp::ToolCallback Information, NaWServer::mcp::ToolCallback Echo>
	inline constexpr NaWServer::mcp::Tool read_only_tools[] = {
		{"device_info", "Read device information",
		 "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}", Information},
		{"echo", "Echo at most 256 UTF-8 bytes",
		 "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"maxLength\":256,"
		 "\"description\":\"At most 256 UTF-8 bytes\"}},"
		 "\"required\":[\"text\"],\"additionalProperties\":false}", Echo}};

	template <class Experiments, NaWServer::mcp::ToolCallback Information, NaWServer::mcp::ToolCallback Echo>
	inline const NaWServer::mcp::Tool tools[] = {
		read_only_tools<Information, Echo>[0],
		read_only_tools<Information, Echo>[1],
		{"experiment_status", "Read experiment state",
		 "{\"type\":\"object\",\"additionalProperties\":false}", Experiments::status},
		{"experiment_set", "Start or stop experiment; defaults pi/5000/200/1000/0/2",
		 Experiments::input_schema, Experiments::set, nullptr, false}};

}
#endif
