// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "../../examples/common/demo_mcp_experiments.hpp"
#include "../../examples/common/demo_mcp_tools.hpp"
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <string>

namespace
{
	struct Command
	{
		bool stop = false, pulse = false;
		int duration = 5000, on = 200, period = 1000, core = 0, priority = 2;
	};
	struct Experiment : Command
	{
		bool running = false, active = false;
		std::int64_t elapsed = 0;
		double pi = 0;
		std::uint64_t iterations = 0;
	};
	struct State { Experiment experiment; } state;
	unsigned calls = 0, checks = 0;
	State read_state()
	{
		return state;
	}

	bool set_experiment(const Command &command)
	{
		++calls;
		if(command.stop)
		{
			state.experiment.running = false;
			return true;
		}

		if(state.experiment.running || command.core != 0 || command.priority < 1 || command.priority > 5
		    || command.duration < 1000 || command.duration > 30000 || command.period < 20
		    || command.period > 5000 || command.on < 1 || command.on > command.period) return false;

		static_cast<Command &>(state.experiment) = command;
		state.experiment.running = true;
		return true;
	}

	void check(bool condition, const char *name)
	{
		++checks;
		if(!condition)
		{
			std::cerr << "FAIL " << name << '\n';
			std::exit(1);
		}
	}

	using Binding = nawserver_demo_mcp::ExperimentTools<Command, read_state, set_experiment, 0>;
	NaWServer::mcp::ToolResult information(void *, const NaWServer::json::Document &,
	    NaWServer::json::Value, NaWServer::json::Writer &writer)
	{
		writer.string("test");
		return NaWServer::mcp::ToolResult::Success;
	}

}

int main()
{
	alignas(std::max_align_t) unsigned char storage[NAWSERVER_JSON_WORKSPACE_BYTES];
	NaWServer::json::Document document(storage, sizeof(storage));
	char output[2048];
	auto set = [&](std::string_view input)
		{
			check(document.parse(input) == NaWServer::json::Error::None, "arguments parse");
			NaWServer::json::Writer writer(output, sizeof(output));
			return Binding::set(nullptr, document, document.root(), writer);
		};
	using NaWServer::mcp::ToolResult;
	check(set("{\"action\":\"start\"}") == ToolResult::Success, "start uses runtime defaults");
	check(state.experiment.duration == 5000 && state.experiment.on == 200 && state.experiment.period == 1000
	    && state.experiment.core == 0 && state.experiment.priority == 2 && !state.experiment.pulse,
	    "defaults retained");
	check(set("{\"action\":\"start\"}") == ToolResult::InvalidArguments, "runtime rejects running start");
	check(set("{\"action\":\"stop\",\"duration_ms\":0}") == ToolResult::Success, "runtime owns stop validation");
	const unsigned before = calls;
	check(set("{\"action\":\"start\",\"core\":18446744073709551615}") == ToolResult::InvalidArguments,
	    "uint64 does not truncate into command");
	check(set("{\"action\":\"start\",\"period_ms\":1.5}") == ToolResult::InvalidArguments, "fraction rejected");
	check(set("{\"action\":\"start\",\"unknown\":true}") == ToolResult::InvalidArguments, "unknown field rejected");
	check(calls == before, "malformed arguments never invoke application");
	check(set("{\"action\":\"start\",\"core\":1}") == ToolResult::InvalidArguments, "runtime owns core limit");
	check(set("{\"action\":\"start\",\"on_ms\":201,\"period_ms\":200}") == ToolResult::InvalidArguments,
	    "runtime owns cross-field validation");
	check(set("{\"action\":\"start\",\"kind\":\"pulse\",\"duration_ms\":1000,\"on_ms\":5,\"period_ms\":20}")
	    == ToolResult::Success, "pulse command forwarded");
	check(state.experiment.pulse && state.experiment.on == 5 && state.experiment.period == 20, "parameters retained");
	state.experiment.iterations = UINT64_MAX;
	check(document.parse("{}") == NaWServer::json::Error::None, "status arguments parse");
	NaWServer::json::Writer writer(output, sizeof(output));
	check(Binding::status(nullptr, document, document.root(), writer) == ToolResult::Success, "status success");
	check(document.parse(writer.fragment()) == NaWServer::json::Error::None, "result is JSON text string");
	char decoded[512];
	std::size_t length;
	check(document.decode(document.root(), decoded, sizeof(decoded), length), "status JSON text decode");
	check(document.parse({decoded, length}) == NaWServer::json::Error::None, "snapshot valid JSON");
	std::uint64_t iterations;
	check(document.unsignedInteger(document.member(document.root(), "iterations"), iterations)
	    && iterations == UINT64_MAX, "snapshot retains exact uint64");
	check(document.parse(Binding::input_schema) == NaWServer::json::Error::None, "schema valid bounded JSON");
	NaWServer::mcp::Workspace workspace{storage, sizeof(storage), output, sizeof(output)};
	NaWServer::mcp::Endpoint endpoint(nawserver_demo_mcp::tools<Binding, information, information>, 4, workspace);
	const auto reply =
	    endpoint.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"_meta\":{"
	        "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
	check(reply.status == 200 && !reply.body.empty(), "actual four-tool list fits 2048 bytes and 256 tokens");
	check(document.parse(reply.body) == NaWServer::json::Error::None, "actual tool list is bounded JSON");
	std::cout << "Tool list bytes=" << reply.body.size() << " tokens=" << document.count() << '\n';
	NaWServer::mcp::Endpoint read_only_endpoint(nawserver_demo_mcp::read_only_tools<information, information>,
	    2, workspace);
	const auto read_only_reply =
	    read_only_endpoint.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
	check(read_only_reply.status == 200 && document.parse(read_only_reply.body) == NaWServer::json::Error::None,
	    "minimal read-only registry valid");
	const auto minimal_tools = document.member(document.member(document.root(), "result"), "tools");
	check(minimal_tools && minimal_tools.type() == NaWServer::json::Type::Array, "minimal list contains tools");
	const auto tool_count = document.size(minimal_tools);
	check(tool_count == 2 && read_only_reply.body.find("experiment_") == std::string_view::npos,
	    "minimal profile has exactly two tools and no experiment callbacks");
	std::cout << checks << " demo experiment checks passed\n";
}
