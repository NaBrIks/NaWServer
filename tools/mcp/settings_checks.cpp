// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/json.h>
#include <NaWServer/mcp.h>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
	using namespace NaWServer;
	unsigned checks = 0;

	void check(bool condition, const char *description)
	{
		++checks;
		if(!condition)
		{
			std::cerr << "FAIL " << description << '\n';
			std::exit(1);
		}
	}

	// The dedicated target changes the compile-time defaults. No request or
	// runtime Config can silently enlarge these statically reserved arrays.
	alignas(std::max_align_t) unsigned char reader_storage[NAWSERVER_JSON_WORKSPACE_BYTES];
	char output[NAWSERVER_MCP_OUTPUT_BYTES];

	mcp::ToolResult echo(void *, const json::Document &document, json::Value arguments, json::Writer &writer)
	{
		const auto text = document.member(arguments, "text");
		char decoded[1024];
		std::size_t size{};
		if(!text || document.size(arguments) != 1 || !document.decode(text, decoded, sizeof(decoded), size))
			return mcp::ToolResult::InvalidArguments;

		writer.string({decoded, size});
		return mcp::ToolResult::Success;
	}

	json::Error parse(std::string_view input, json::Limits limits)
	{
		json::Document document(reader_storage, sizeof(reader_storage), limits);
		return document.parse(input);
	}

}

int main()
{
	using namespace NaWServer;
	static_assert(NAWSERVER_JSON_INPUT_BYTES == 8192, "settings target must override input bytes");
	static_assert(NAWSERVER_JSON_STRING_BYTES == 1024, "settings target must override decoded string bytes");
	static_assert(NAWSERVER_JSON_DEPTH == 12, "settings target must override depth");
	static_assert(NAWSERVER_JSON_VALUE_CAPACITY == 128, "settings target must override logical value capacity");

	json::Limits defaults;
	check(defaults.input_bytes == 8192 && defaults.string_bytes == 1024 && defaults.depth == 12
	    && defaults.value_capacity == 128, "compile-time defaults reach public Limits");
	check(json::validWorkspace(reader_storage, sizeof(reader_storage)), "configured aligned arena admitted");
	check(!json::validWorkspace(reader_storage, 1), "one-byte arena cannot hold reader apparatus");
	check(!json::validWorkspace(reader_storage + 1, sizeof(reader_storage) - 1), "misaligned arena rejected");

	const mcp::Tool tools[] = {{"echo", "Settings corpus echo", "{\"type\":\"object\"}", echo}};
	mcp::Workspace workspace{reader_storage, sizeof(reader_storage), output, sizeof(output)};
	mcp::Config config;
	mcp::Endpoint endpoint(tools, 1, workspace, "/mcp", nullptr, nullptr, config);
	const std::string ping = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
	const std::string padded = std::string(5000, ' ') + ping;
	check(endpoint.dispatch(padded).status == 200, "configured input larger than 4096 is accepted");
	check(endpoint.config().limits.input_bytes == 8192, "endpoint retains caller/default request limit");
	check(endpoint.dispatch(std::string(8193, ' ')).status == 413, "configured large input limit is enforced");

	mcp::Config small;
	small.limits.input_bytes = 32;
	mcp::Endpoint small_input(tools, 1, workspace, "/mcp", nullptr, nullptr, small);
	check(small_input.dispatch(ping).status == 413, "per-endpoint smaller input limit enforced");
	check(endpoint.dispatch(ping).status == 200, "one endpoint Config does not modify another endpoint");

	mcp::Config short_request;
	short_request.limits.input_bytes = 16;
	mcp::Endpoint short_input(tools, 1, workspace, "/mcp", nullptr, nullptr, short_request);
	auto invalid_request = short_input.dispatch("{}");
	check(invalid_request.status == 200 && invalid_request.body.size() > short_request.limits.input_bytes
	    && invalid_request.body.find("-32600") != std::string_view::npos,
	    "small request budget does not truncate or reject a longer protocol error");

	json::Limits limits = defaults;
	limits.string_bytes = 4;
	check(parse(R"("\u0061\u0062\u0063\u0064")", limits) == json::Error::None,
	    "string budget counts decoded bytes rather than raw escapes");
	check(parse(R"("\u0061\u0062\u0063\u0064\u0065")", limits) == json::Error::Capacity,
	    "decoded string overflow is rejected");
	check(parse(std::string("\"") + std::string(800, 'a') + "\"", defaults) == json::Error::None,
	    "nondefault decoded string capacity above 512 is usable");
	limits = defaults;
	limits.depth = 2;
	check(parse("[[]]", limits) == json::Error::None, "runtime depth exact boundary accepted");
	check(parse("[[[]]]", limits) == json::Error::Depth, "runtime depth overflow rejected");
	limits = defaults;
	limits.container_items = 2;
	check(parse("[1,2]", limits) == json::Error::None, "runtime container boundary accepted");
	check(parse("[1,2,3]", limits) == json::Error::Capacity, "runtime container overflow rejected");
	limits = defaults;
	limits.value_capacity = 3;
	check(parse(R"({"x":1})", limits) == json::Error::None, "logical values include object keys at boundary");
	limits.value_capacity = 2;
	check(parse(R"({"x":1})", limits) == json::Error::Capacity, "logical value/key overflow rejected");

	const std::string echo_request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
	    "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"" + std::string(400, 'x') + "\"}}}";
	workspace.output_capacity = 256;
	auto overflow = endpoint.dispatch(echo_request);
	check(overflow.status == 500 && overflow.body.find("-32603") != std::string_view::npos,
	    "output overflow returns a complete protocol error");
	check(parse(overflow.body, defaults) == json::Error::None, "overflow error is valid complete JSON");
	workspace.output_capacity = sizeof(output);

	workspace.reader_storage_bytes = 1;
	check(endpoint.dispatch("not JSON").status == 503, "undersized arena rejected before parsing");
	workspace.reader_storage_bytes = sizeof(reader_storage) - 1;
	workspace.reader_storage = reader_storage + 1;
	check(endpoint.dispatch("not JSON").status == 503, "misaligned arena rejected before parsing");
	workspace.reader_storage = reader_storage;
	workspace.reader_storage_bytes = sizeof(reader_storage);

	for(unsigned field = 0; field < 5; ++field)
	{
		mcp::Config invalid;
		if(field == 0) invalid.limits.input_bytes = 0;

		if(field == 1) invalid.limits.string_bytes = 0;

		if(field == 2) invalid.limits.container_items = 0;

		if(field == 3) invalid.limits.depth = 0;

		if(field == 4) invalid.limits.value_capacity = 0;

		mcp::Endpoint invalid_endpoint(tools, 1, workspace, "/mcp", nullptr, nullptr, invalid);
		check(invalid_endpoint.dispatch("not JSON").status == 503,
		    "zero runtime budget rejected rather than treated as unlimited");
	}
	check(endpoint.dispatch(ping).status == 200, "valid endpoint recovers after rejected storage/config profiles");
	std::cout << checks << " settings checks passed; backend=" << json::backendName()
	          << " reader_bytes=" << sizeof(reader_storage) << " output_bytes=" << sizeof(output) << '\n';
}
