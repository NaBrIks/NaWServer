// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/mcp.h>
#if NAWSERVER_MCP
#include <NaWServer/request.h>
#include <NaWServer/response.h>
#include <cstring>

namespace NaWServer::mcp
{
	namespace
	{
		// This lower bound leaves room for a complete protocol error, never a partial prefix.
		constexpr std::size_t minimum_output_bytes = 256;
		bool object(json::Value t)
		{
			return t && t.type() == json::Type::Object;
		}

		bool string(json::Value t)
		{
			return t && t.type() == json::Type::String;
		}

		void prefix(json::Writer &w, const json::Document &d, json::Value id)
		{
			w.append("{\"jsonrpc\":\"2.0\",\"id\":");
			// JSON-RPC IDs cannot be real numbers or containers. Avoid the generic
			// value writer here: its double branch makes floating-point formatting
			// reachable to the linker even in an otherwise read-only minimal build.
			if(!id) w.append("null");
			else if(id.type() == json::Type::String) w.string(d, id);
			else if(id.type() == json::Type::Signed)
			{
				std::int64_t number;
				if(d.signedInteger(id, number)) w.integer(number);
				else w.append("null");
			}
			else if(id.type() == json::Type::Unsigned)
			{
				std::uint64_t number;
				if(d.unsignedInteger(id, number)) w.integer(number);
				else w.append("null");
			}
			else w.append("null");
		}

		Reply error(json::Writer &w, const json::Document &d, json::Value id, int code, const char *message)
		{
			prefix(w, d, id); w.append(",\"error\":{\"code\":");
			w.integer(static_cast<std::int64_t>(code)); w.append(",\"message\":"); w.string(message); w.append("}}");
			return {200, w.fragment()};
		}

		bool ascii_equal(std::string_view a, std::string_view b)
		{
			if(a.size() != b.size()) return false;

			for(std::size_t i = 0; i < a.size(); ++i)
			{
				char c = a[i]; if(c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');

				char d = b[i]; if(d >= 'A' && d <= 'Z') d = static_cast<char>(d + 'a' - 'A');

				if(c != d) return false;
			}
			return true;
		}

		std::string_view trim(std::string_view s)
		{
			while(!s.empty() && (s.front() == ' ' || s.front() == '\t'))
				s.remove_prefix(1);
			while(!s.empty() && (s.back() == ' ' || s.back() == '\t'))
				s.remove_suffix(1);
			return s;
		}

		bool accepts(std::string_view header, std::string_view wanted)
		{
			while(!header.empty())
			{
				std::size_t comma = header.find(',');
				auto range = trim(header.substr(0, comma));
				std::size_t semicolon = range.find(';');
				bool matching = ascii_equal(trim(range.substr(0, semicolon)), wanted);
				bool permitted = true;
				while(semicolon != std::string_view::npos)
				{
					range.remove_prefix(semicolon + 1); semicolon = range.find(';');
					auto parameter = trim(range.substr(0, semicolon));
					std::size_t equals = parameter.find('=');
					if(equals == std::string_view::npos) { permitted = false; break; }

					if(ascii_equal(trim(parameter.substr(0, equals)), "q"))
					{
						auto quality = trim(parameter.substr(equals + 1));
						bool nonzero = false;
						if(quality.empty() || (quality[0] != '0' && quality[0] != '1')) permitted = false;

						for(std::size_t i = 0; i < quality.size(); ++i)
						{
							if(i == 1 && quality[i] == '.') continue;

							if(quality[i] < '0' || quality[i] > '9' ||
							    (i > 0 && quality[0] == '1' && quality[i] != '0'))
								permitted = false;

							if(quality[i] >= '1' && quality[i] <= '9') nonzero = true;
						}
						if(quality.size() > 5 || (quality.size() > 1 && quality[1] != '.') || !nonzero)
							permitted = false;
					}
				}
				if(matching && permitted) return true;

				if(comma == std::string_view::npos) break;

				header.remove_prefix(comma + 1);
			}
			return false;
		}

		bool decode_name(std::string_view input, char *output, std::size_t capacity, std::size_t &size)
		{
			if(input.substr(0, 9) != "=?base64?") return false;

			if(input.size() < 11 || input.substr(input.size() - 2) != "?=") return false;

			input = input.substr(9, input.size() - 11);
			if(input.size() % 4 != 0) return false;

			size = 0;
			for(std::size_t i = 0; i < input.size(); i += 4)
			{
				unsigned value = 0; unsigned padding = 0;
				for(unsigned j = 0; j < 4; ++j)
				{
					char c = input[i + j]; unsigned digit;
					if(c >= 'A' && c <= 'Z') digit = static_cast<unsigned>(c - 'A');
					else if(c >= 'a' && c <= 'z') digit = static_cast<unsigned>(c - 'a' + 26);
					else if(c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0' + 52);
					else if(c == '+') digit = 62;
					else if(c == '/') digit = 63;
					else if(c == '=' && j >= 2 && i + 4 == input.size()) { digit = 0; ++padding; }
					else return false;

					if(padding && c != '=') return false;

					value = (value << 6) | digit;
				}
				if((padding == 1 && (value & 255)) || (padding == 2 && (value & 65535))) return false;

				for(unsigned j = 0; j < 3 - padding; ++j)
				{
					if(size == capacity) return false;

					output[size++] = static_cast<char>(value >> (16 - 8 * j));
				}
			}
			return true;
		}

		bool supported(std::string_view v)
		{
			return v.empty() || v == protocol_version || v == "2025-11-25";
		}

	}

	Endpoint::Endpoint(const Tool *tools, std::size_t count, Workspace &workspace, const char *path,
	    OriginPolicy origin_policy, void *origin_context, Config config)
	  : tools_(tools), tool_count_(count), workspace_(workspace), config_(config), descriptor_{path},
	    origin_policy_(origin_policy), origin_context_(origin_context)
	{
	}

	Reply Endpoint::process(std::string_view input, std::string_view version,
	    std::string_view header_method, std::string_view header_name, bool http, bool invalid_headers)
	{
		json::Document document(workspace_.reader_storage, workspace_.reader_storage_bytes, config_.limits);
		json::Writer writer(workspace_.output, workspace_.output_capacity);

		if(input.size() > config_.limits.input_bytes) return {413, {}};

		if(document.parse(input) != json::Error::None) return error(writer, document, {}, -32700, "Parse error");

		const auto root = document.root();
		if(!object(root)) return error(writer, document, {}, -32600, "Invalid Request");

		const auto id = document.member(root, "id");
		if(id && id.type() != json::Type::String && id.type() != json::Type::Signed &&
		    id.type() != json::Type::Unsigned)
			return error(writer, document, {}, -32600, "Invalid request id");

		const auto rpc = document.member(root, "jsonrpc");
		const auto method = document.member(root, "method");
		const auto params = document.member(root, "params");
		if(!rpc || !document.equals(rpc, "2.0") || !string(method) || (params && !object(params)))
			return error(writer, document, id, -32600, "Invalid Request");

		const auto metadata = params ? document.member(params, "_meta") : json::Value{};
		const auto body_version = metadata ? document.member(metadata,
		        "io.modelcontextprotocol/protocolVersion") : json::Value{};
		const bool modern = body_version || version == protocol_version || document.equals(method, "server/discover");
		auto protocol_error = [&](int code, const char *message, int status)
			{
				Reply reply = error(writer, document, id, code, message); reply.status = status; return reply;
			};
		if(invalid_headers) return protocol_error(-32020, "Invalid HTTP protocol metadata", 400);

		if(modern && http && string(body_version) && !version.empty() && !document.equals(body_version, version))
			return protocol_error(-32020, "HTTP/body protocol version mismatch", 400);

		if(!supported(version) || (modern && string(body_version) && !document.equals(body_version, protocol_version)))
		{
			prefix(writer, document, id);
			writer.append(",\"error\":{\"code\":-32022,\"message\":\"Unsupported protocol version\","
			    "\"data\":{\"supported\":[\"2026-07-28\",\"2025-11-25\"],\"requested\":");
			if(string(body_version)) writer.string(document, body_version);
			else writer.string(version);

			writer.append("}}}"); return {400, writer.fragment()};
		}

		if(modern)
		{
			if(!object(metadata) || !string(body_version)
			    || !object(document.member(metadata, "io.modelcontextprotocol/clientCapabilities")))
				return protocol_error(-32602, "Required request metadata missing", 400);

			if(http && (version != protocol_version || header_method.empty() || !document.equals(method,
			    header_method)))
				return protocol_error(-32020, "HTTP protocol/method metadata mismatch", 400);

			if(http && document.equals(method, "tools/call"))
			{
				const auto name = document.member(params, "name");
				if(!name || header_name.empty() || !document.equals(name, header_name))
					return protocol_error(-32020, "HTTP tool name metadata mismatch", 400);
			}
		}

		// This stateless profile never sends requests to a client; client responses
		// have no corresponding outstanding request and are rejected as invalid.
		if(!id)
		{
			if(document.equals(method, "notifications/initialized") || document.equals(method,
			    "notifications/cancelled"))
				return {202, {}};

			return {202, {}}; // Unknown notifications are ignored, never answered.
		}

		if(modern && document.equals(method, "server/discover"))
		{
			prefix(writer, document, id);
			writer.append(",\"result\":{\"resultType\":\"complete\","
			    "\"supportedVersions\":[\"2026-07-28\",\"2025-11-25\"],\"capabilities\":{\"tools\":{}},"
			    "\"_meta\":{\"io.modelcontextprotocol/serverInfo\":{\"name\":\"NaWServer\",\"version\":\"0.1.0\"}}}}");
		}
		else if(!modern && document.equals(method, "initialize"))
		{
			const auto v = params ? document.member(params, "protocolVersion") : json::Value{};
			const auto capabilities = params ? document.member(params, "capabilities") : json::Value{};
			const auto client = params ? document.member(params, "clientInfo") : json::Value{};
			if(!string(v) || !object(capabilities) || !object(client)
			    || !string(document.member(client, "name")) || !string(document.member(client, "version")))
				return error(writer, document, id, -32602, "Invalid initialization parameters");

			prefix(writer, document, id);
			writer.append(
				",\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
				"\"serverInfo\":{\"name\":\"NaWServer\",\"version\":\"0.1.0\"}}}");
		}
		else if(document.equals(method, "ping"))
		{
			prefix(writer, document, id);
			writer.append(modern ? ",\"result\":{\"resultType\":\"complete\"}}" : ",\"result\":{}}");
		}
		else if(document.equals(method, "tools/list"))
		{
			if(params && document.member(params, "cursor"))
				return error(writer, document, id, -32602, "Pagination unsupported");

			prefix(writer, document, id);
			writer.append(modern ? ",\"result\":{\"resultType\":\"complete\",\"tools\":[" : ",\"result\":{\"tools\":[");
			for(std::size_t i = 0; i < tool_count_; ++i)
			{
				if(i) writer.append(",");

				writer.append("{\"name\":"); writer.string(tools_[i].name);
				writer.append(",\"description\":"); writer.string(tools_[i].description);
				writer.append(",\"inputSchema\":"); writer.append(tools_[i].input_schema);
				writer.append(",\"annotations\":{\"readOnlyHint\":");
				writer.append(tools_[i].read_only ? "true" : "false");
				writer.append("}}");
			}
			writer.append("]}}");
		}
		else if(document.equals(method, "tools/call"))
		{
			const auto name = params ? document.member(params, "name") : json::Value{};
			const auto arguments = params ? document.member(params, "arguments") : json::Value{};
			if(!string(name) || (arguments && !object(arguments)))
				return error(writer, document, id, -32602, "Invalid tool parameters");

			const Tool *tool = nullptr;
			for(std::size_t i = 0; i < tool_count_; ++i)
				if(document.equals(name, tools_[i].name)) { tool = tools_ + i; break; }

			if(!tool || !tool->callback) return error(writer, document, id, -32602, "Unknown tool");

			prefix(writer, document, id);
			writer.append(modern
			? ",\"result\":{\"resultType\":\"complete\",\"content\":[{\"type\":\"text\",\"text\":"
			: ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
			auto result = tool->callback(tool->context, document, arguments, writer);
			if(result != ToolResult::Success)
			{
				// Discard all callback bytes before constructing the alternative response.
				json::Writer replacement(workspace_.output, workspace_.output_capacity);
				if(result == ToolResult::InvalidArguments)
					return error(replacement, document, id, -32602, "Invalid tool arguments");

				prefix(replacement, document, id);
				replacement.append(modern ? ",\"result\":{\"resultType\":\"complete\",\"content\":["
				: ",\"result\":{\"content\":[");
				replacement.append("{\"type\":\"text\",\"text\":\"Tool execution failed\"}],\"isError\":true}}");
				return {200, replacement.fragment()};
			}

			writer.append("}],\"isError\":false}}");
		}
		else return protocol_error(-32601, "Method not found", modern ? 404 : 200);

		return {200, writer.fragment()};
	}

	Reply Endpoint::dispatch(std::string_view input, std::string_view version,
	    std::string_view method, std::string_view name, bool http, bool invalid_headers)
	{
		if(!json::validLimits(config_.limits)
		    || !json::validWorkspace(workspace_.reader_storage, workspace_.reader_storage_bytes)
		    || !workspace_.output
		    || workspace_.output_capacity < minimum_output_bytes || workspace_.output_capacity > 65535
		    || !workspace_.reader_storage_bytes)
			return {503, {}};

		Reply reply = process(input, version, method, name, http, invalid_headers);
		if(reply.status != 202 && reply.status != 413 && reply.status != 503 && reply.body.empty())
		{
			// Output overflow never becomes a truncated success response.
			static constexpr std::string_view failure =
			    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Output capacity exceeded\"}}";
			return {500, failure};
		}

		if(!reply.body.empty())
		{
			// Request views are no longer used here. Reuse their workspace to check
			// trusted application schema/callback output before any bytes are sent.
			auto output_limits = config_.limits;
			output_limits.input_bytes = workspace_.output_capacity;
			json::Document validation(workspace_.reader_storage, workspace_.reader_storage_bytes, output_limits);
			if(validation.parse(reply.body) != json::Error::None)
			{
				static constexpr std::string_view invalid =
				    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Invalid bounded tool output\"}}";
				return {500, invalid};
			}
		}

		return reply;
	}

	void Endpoint::handleRequest(Request *request, Response &response)
	{
		// Short-lived protocol header copies are stack bounds, independent of JSON arenas.
		constexpr std::size_t origin_bytes = 192, version_bytes = 32, accept_bytes = 192;
		constexpr std::size_t content_type_bytes = 96, method_bytes = 64, name_bytes = 64;
		char origin[origin_bytes], version[version_bytes], accept[accept_bytes];
		char content_type[content_type_bytes], method[method_bytes], name[name_bytes];
		auto origin_result = request->readHeader("Origin", origin, sizeof(origin));
		if(origin_result.status != HeaderReadStatus::NotFound)
		{
			// Host is attacker controlled and cannot be the Origin trust anchor.
			// Applications provide their actual trusted addresses; absent policy fails closed.
			if(origin_result.status != HeaderReadStatus::Ok || !origin_policy_
			    || !origin_policy_(origin_context_, {origin, origin_result.size}))
			{
				(void)response.empty(403); return;
			}
		}

		if(request->method() != HttpMethod::Post)
		{
			(void)response.header("Allow", "POST"); (void)response.empty(405); return;
		}

		auto content = request->readHeader("Content-Type", content_type, sizeof(content_type));
		std::string_view mime = content.status ==
		    HeaderReadStatus::Ok ? std::string_view(content_type) : std::string_view{};
		if(!ascii_equal(trim(mime.substr(0, mime.find(';'))), "application/json"))
		{
			(void)response.empty(415); return;
		}

		auto accepted = request->readHeader("Accept", accept, sizeof(accept));
		if(accepted.status != HeaderReadStatus::Ok || !accepts(accept, "application/json")
		    || !accepts(accept, "text/event-stream"))
		{
			(void)response.empty(406); return;
		}

		auto v = request->readHeader("MCP-Protocol-Version", version, sizeof(version));
		bool invalid_headers = v.status != HeaderReadStatus::NotFound && v.status != HeaderReadStatus::Ok;
		if(busy_.test_and_set(std::memory_order_acquire)) { (void)response.empty(503); return; }

		struct Release { std::atomic_flag &flag; ~Release()
						 {
							 flag.clear(std::memory_order_release);
						 }

		} release{busy_};
		auto method_header = request->readHeader("Mcp-Method", method, sizeof(method));
		auto name_header = request->readHeader("Mcp-Name", name, sizeof(name));
		invalid_headers = invalid_headers
		    || (method_header.status != HeaderReadStatus::NotFound && method_header.status != HeaderReadStatus::Ok)
		    || (name_header.status != HeaderReadStatus::NotFound && name_header.status != HeaderReadStatus::Ok);
		char decoded_name[name_bytes]; std::size_t decoded_size{};
		std::string_view tool_name = name_header.status ==
		    HeaderReadStatus::Ok ? std::string_view(name) : std::string_view{};
		if(tool_name.substr(0, 9) == "=?base64?")
		{
			if(!decode_name(tool_name, decoded_name, sizeof(decoded_name), decoded_size)) invalid_headers = true;

			tool_name = {decoded_name, decoded_size};
		}

		auto body = request->body();
		Reply reply = dispatch({reinterpret_cast<const char *>(body.data()), body.size()},
		        v.status == HeaderReadStatus::Ok ? std::string_view(version) : std::string_view{},
		        method_header.status == HeaderReadStatus::Ok ? std::string_view(method) : std::string_view{},
		        tool_name, true, invalid_headers);
		(void)response.send(reply.status, "application/json",
		    {reinterpret_cast<const std::uint8_t *>(reply.body.data()), reply.body.size()});
	}

}
#endif
