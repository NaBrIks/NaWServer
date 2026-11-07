// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/mcp.h>
#if NAWSERVER_MCP
#include <limits>

namespace nawserver_demo_mcp
{

	// Compile-time application binding: the MCP module never owns a runtime task
	// or duplicates its locking, start/stop and board-specific validation rules.
	template <class Command, auto ReadState, auto SetExperiment, int MaximumCore>
	struct ExperimentTools
	{
		static constexpr const char *input_schema = MaximumCore == 0
		    ? "{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"start\",\"stop\"]},"
		    "\"kind\":{\"enum\":[\"pi\",\"pulse\"]},\"duration_ms\":{\"type\":\"integer\",\"minimum\":1000,\"maximum\":30000},"
		    "\"on_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000},\"period_ms\":{\"type\":\"integer\",\"minimum\":20,\"maximum\":5000},"
		    "\"core\":{\"enum\":[0]},\"priority\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5}},"
		    "\"required\":[\"action\"],\"additionalProperties\":false}"
		    : "{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"start\",\"stop\"]},"
		    "\"kind\":{\"enum\":[\"pi\",\"pulse\"]},\"duration_ms\":{\"type\":\"integer\",\"minimum\":1000,\"maximum\":30000},"
		    "\"on_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000},\"period_ms\":{\"type\":\"integer\",\"minimum\":20,\"maximum\":5000},"
		    "\"core\":{\"enum\":[0,1]},\"priority\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5}},"
		    "\"required\":[\"action\"],\"additionalProperties\":false}";

		static bool integer(const NaWServer::json::Document &document, NaWServer::json::Value arguments,
		    const char *name, int &value)
		{
			const auto token = document.member(arguments, name);
			if(!token) return true;

			std::int64_t number;
			if(!document.signedInteger(token, number) || number < std::numeric_limits<int>::min()
			    || number > std::numeric_limits<int>::max()) return false;

			value = static_cast<int>(number);
			return true;
		}

		static NaWServer::mcp::ToolResult snapshot(NaWServer::json::Writer &output)
		{
			const auto runtime_snapshot = ReadState();
			const auto &experiment = runtime_snapshot.experiment;
			// Fixed application snapshot text, independent of the parser arena size.
			constexpr std::size_t experiment_snapshot_bytes = 512;
			char bytes[experiment_snapshot_bytes];
			NaWServer::json::Writer writer(bytes, sizeof(bytes));
			writer.append("{\"running\":"); writer.append(experiment.running ? "true" : "false");
			writer.append(",\"kind\":"); writer.string(experiment.pulse ? "pulse" : "pi");
			writer.append(",\"duration_ms\":"); writer.integer(static_cast<std::int64_t>(experiment.duration));
			writer.append(",\"on_ms\":"); writer.integer(static_cast<std::int64_t>(experiment.on));
			writer.append(",\"period_ms\":"); writer.integer(static_cast<std::int64_t>(experiment.period));
			writer.append(",\"core\":"); writer.integer(static_cast<std::int64_t>(experiment.core));
			writer.append(",\"priority\":"); writer.integer(static_cast<std::int64_t>(experiment.priority));
			writer.append(",\"elapsed_ms\":"); writer.integer(static_cast<std::int64_t>(experiment.elapsed));
			writer.append(",\"progress_pct\":");
			writer.real(experiment.duration > 0 ? 100.0 * experiment.elapsed / experiment.duration : 0.0);
			writer.append(",\"pi\":"); writer.real(experiment.pi);
			writer.append(",\"iterations\":"); writer.integer(static_cast<std::uint64_t>(experiment.iterations));
			writer.append(",\"active\":"); writer.append(experiment.active ? "true" : "false");
			writer.append(",\"sample_ms\":100,\"enabled\":"); writer.append(experiment.running ? "true" : "false");
			writer.append("}");
			// This fixed-shape inner JSON becomes text in the outer MCP response;
			// the outer writer performs the final bounded document validation.
			if(!writer.valid() || !output.string(writer.fragment())) return NaWServer::mcp::ToolResult::Failed;

			return NaWServer::mcp::ToolResult::Success;
		}

		static NaWServer::mcp::ToolResult status(void *, const NaWServer::json::Document &document,
		    NaWServer::json::Value arguments, NaWServer::json::Writer &output)
		{
			if(arguments && document.size(arguments) != 0)
				return NaWServer::mcp::ToolResult::InvalidArguments;

			return snapshot(output);
		}

		static NaWServer::mcp::ToolResult set(void *, const NaWServer::json::Document &document,
		    NaWServer::json::Value arguments, NaWServer::json::Writer &output)
		{
			using NaWServer::mcp::ToolResult;
			if(!arguments || arguments.type() != NaWServer::json::Type::Object) return ToolResult::InvalidArguments;

			for(std::size_t index = 0; index < document.size(arguments); ++index)
			{
				const auto key = document.memberAt(arguments, index).key;
				if(!document.equals(key, "action") && !document.equals(key, "kind")
				    && !document.equals(key, "duration_ms") && !document.equals(key, "on_ms")
				    && !document.equals(key, "period_ms") && !document.equals(key, "core")
				    && !document.equals(key, "priority")) return ToolResult::InvalidArguments;
			}
			const auto action = document.member(arguments, "action");
			const auto kind = document.member(arguments, "kind");
			if(!action || (!document.equals(action, "start") && !document.equals(action, "stop"))
			    || (kind && !document.equals(kind, "pi") && !document.equals(kind, "pulse")))
				return ToolResult::InvalidArguments;

			Command command;
			command.stop = document.equals(action, "stop");
			command.pulse = kind && document.equals(kind, "pulse");
			if(!integer(document, arguments, "duration_ms", command.duration)
			    || !integer(document, arguments, "on_ms", command.on)
			    || !integer(document, arguments, "period_ms", command.period)
			    || !integer(document, arguments, "core", command.core)
			    || !integer(document, arguments, "priority", command.priority) || !SetExperiment(command))
				return ToolResult::InvalidArguments;

			return snapshot(output);
		}

	};

} // namespace nawserver_demo_mcp
#endif
