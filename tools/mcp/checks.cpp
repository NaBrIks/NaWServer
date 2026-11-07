// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/json.h>
#include <NaWServer/mcp.h>
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <map>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <cmath>
using namespace NaWServer;
static unsigned checks;
static void check(bool condition, const char *name)
{
	++checks;
	if(!condition) { std::cerr << "FAIL " << name << '\n'; std::exit(1); }
}

static mcp::ToolResult echo(void *, const json::Document &d, json::Value args, json::Writer &w)
{
	const auto text = args ? d.member(args, "text") : json::Value{};
	char buffer[256]; std::size_t size;
	if(!text || !d.decode(text, buffer, sizeof(buffer), size)) return mcp::ToolResult::InvalidArguments;

	w.string({buffer, size}); return mcp::ToolResult::Success;
}

struct Exchange : HttpExchange
{
	HttpMethod method = HttpMethod::Post;
	std::string input;
	std::map<std::string, std::string> headers;
	int status{};
	std::string output;
	HttpRequestHead head() const override
	{
		return {method, "/mcp", input.size()};
	}

	HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
	{
		auto i = headers.find(name);
		if(i == headers.end()) return {HeaderReadStatus::NotFound, 0};

		if(capacity <= i->second.size()) return {HeaderReadStatus::BufferTooSmall, i->second.size()};

		std::memcpy(destination, i->second.c_str(), i->second.size() + 1);
		return {HeaderReadStatus::Ok, i->second.size()};
	}

	BodyReadResult readBody(std::uint8_t *, std::size_t) override
	{
		return {BodyReadStatus::End, 0};
	}

	std::optional<ByteView> preparedBody() const override
	{
		return ByteView{reinterpret_cast<const std::uint8_t *>(input.data()), input.size()};
	}

	ResponseTransport responseTransport() override
	{
		return {this, [](void *context, int status, const char *, const Header *, std::size_t, ByteView body)
				{
					auto &exchange = *static_cast<Exchange *>(context);
					exchange.status = status;
					exchange.output.assign(reinterpret_cast<const char *>(body.data()), body.size());
					return SendResult::Ok;
				}};
	}

};
static bool trusted(void *, std::string_view origin)
{
	return origin == "http://192.0.2.10";
}

int main()
{
	alignas(std::max_align_t) unsigned char storage[NAWSERVER_JSON_WORKSPACE_BYTES];
	json::Document d(storage, sizeof(storage));
	const char *valid[] = {"null", "true", "false", "0", "-0", "-9223372036854775808", "18446744073709551615",
		                   "1.25e-20", "[{},[],true,null]", "\"\\u0000x\"", "\"\\uD83D\\uDE80\"",
		                   "{\"a\":1,\"b\":[2]}"};
	for(auto s : valid)
		check(d.parse(s) == json::Error::None, s);
	// Raw-number adapters must delegate range checks to the common conversion.
	// A large exponent is not overflow by itself: zero remains exactly zero.
	const char *zero_exponents[] = {"0e10000", "-0e10000", "0e-10000"};
	for(auto text : zero_exponents)
	{
		double value;
		check(d.parse(text) == json::Error::None && d.real(d.root(), value) && value == 0.0,
		    text);
	}
	// The complete lexeme, not an overflowing intermediate integer mantissa,
	// determines the finite result. There are 400 zero digits before e-400.
	const auto scaled_integer = std::string("1") + std::string(400, '0') + "e-400";
	double scaled_value;
	check(d.parse(scaled_integer) == json::Error::None && d.real(d.root(), scaled_value) && scaled_value == 1.0,
	    "large mantissa and negative exponent balance exactly");
	const char *invalid[] = {"", "01", "-01", "+1", ".2", "1.", "1e", "1e+", "1e9999", "1e-9999",
		                     "18446744073709551616", "-9223372036854775809", "NaN", "Infinity", "[1,]", "{\"a\":1,}",
		                     "{a:1}", "{\"a\":1 \"b\":2}", "[1 2]", "0x12", "true false", "\"\\uD800\"", "\"\\uDC00\"",
		                     "\"\\uD800\\u0041\"", "\"\\x20\"", "\"\n\"", "{\"a\":1,\"\\u0061\":2}", "\"\xc0\xaf\"",
		                     "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\"", "\"\x80\"", "\"\xe2\x82\""};
	for(auto s : invalid)
		check(d.parse(s) != json::Error::None && !d.root(), s);
	check(d.parse("18446744073709551615") == json::Error::None, "uint parse");
	std::uint64_t ui; check(d.unsignedInteger(d.root(), ui) && ui == UINT64_MAX, "uint exact");
	std::int64_t si; check(!d.signedInteger(d.root(), si), "signed checked overflow");
	check(d.parse("-9223372036854775808") == json::Error::None, "int parse");
	check(d.signedInteger(d.root(), si) && si == INT64_MIN, "int exact");
	check(d.parse("\"\\u0000\\uD83D\\uDE80\"") == json::Error::None, "decode parse");
	char decoded[8]; std::size_t size;
	check(d.decode(d.root(), decoded, sizeof(decoded), size) && size == 5 && decoded[0] == 0, "NUL surrogate decode");
	check(d.equals(d.root(), std::string_view("\0\xf0\x9f\x9a\x80", 5)), "decoded equality");
	check(!d.decode(d.root(), decoded, 4, size) && size == 0, "decode overflow");
	json::Limits tiny_limits; tiny_limits.value_capacity = 2;
	json::Document tiny(storage, sizeof(storage), tiny_limits);
	check(tiny.parse("[1,2]") == json::Error::Capacity, "token bound");
	json::Limits limits; limits.depth = 2; limits.string_bytes = 3; limits.container_items = 2;
	json::Document bounded(storage, sizeof(storage), limits);
	check(bounded.parse("[[[0]]]") == json::Error::Depth, "depth bound");
	check(bounded.parse("\"abcd\"") == json::Error::Capacity, "string bound");
	check(bounded.parse("[1,2,3]") == json::Error::Capacity, "item bound");
	const struct { const char *input; json::Error error; } taxonomy[] = {
		{"{", json::Error::Syntax}, {"\"\\uD800\"", json::Error::Unicode},
		{"1e9999", json::Error::Number}, {"1e-9999", json::Error::Number},
		{"18446744073709551616", json::Error::Number},
		{"{\"a\":1,\"\\u0061\":2}", json::Error::DuplicateKey}};
	for(const auto &example : taxonomy)
		check(d.parse(example.input) == example.error && !d.root() && d.count() == 0, "common error taxonomy");
	struct GuardedStorage
	{
		alignas(std::max_align_t) unsigned char bytes[NAWSERVER_JSON_WORKSPACE_BYTES];
		std::uint64_t canary = UINT64_C(0x8123456789abcdef);
	} guarded;
	json::Document measured(guarded.bytes, sizeof(guarded.bytes));
	const std::string immutable = "{\"a\\u0000b\":[18446744073709551615,\"\\uD83D\\uDE80\",true]}";
	const auto unchanged = immutable;
	check(measured.parse(immutable) == json::Error::None && immutable == unchanged, "immutable borrowed input");
	check(measured.count() == 6 && measured.workspaceHighWaterBytes() > 0
	    && measured.workspaceHighWaterBytes() <= sizeof(guarded.bytes), "caller arena high-water bound");
	check(guarded.canary == UINT64_C(0x8123456789abcdef), "arena does not overwrite adjacent storage");
	check(measured.parse("null") == json::Error::None && measured.count() == 1, "arena reset reclaims former document");
	const auto one_value_bytes = measured.workspaceHighWaterBytes();
	check(measured.parse("[0,1,2,3,4,5,6,7]") == json::Error::None
	    && measured.workspaceHighWaterBytes() > one_value_bytes, "high-water tracks actual document growth");
	alignas(std::max_align_t) unsigned char insufficient[1];
	json::Document no_room(insufficient, sizeof(insufficient));
	check(no_room.parse("null") == json::Error::Capacity && !no_room.root(), "no heap fallback for undersized arena");
	check(!json::validWorkspace(insufficient, sizeof(insufficient)), "workspace apparatus minimum");
	json::Limits invalid_limits; invalid_limits.depth = 33;
	json::Document invalid_configuration(storage, sizeof(storage), invalid_limits);
	check(!json::validLimits(invalid_limits) && invalid_configuration.parse("null") == json::Error::Capacity,
	    "runtime limits reject unbounded recursion");
	char output[2048];
	json::Writer w(output, sizeof(output));
	check(w.string(std::string_view("\0\"\\\xf0\x9f\x9a\x80", 7)), "writer escapes");
	check(w.finish(storage, sizeof(storage)) && d.parse(w.view()) == json::Error::None, "writer roundtrip");
	json::Writer short_writer(output, 2);
	check(!short_writer.string("hello") && short_writer.view().empty(), "writer transactional");
	json::Writer nonfinite(output, sizeof(output)); check(!nonfinite.real(INFINITY), "writer infinity");
	json::Writer number(output, sizeof(output)); check(number.real(0.1), "double write");
	check(number.finish(storage, sizeof(storage)) && d.parse(number.view()) == json::Error::None, "double parse");
	double real; check(d.real(d.root(), real) && real == 0.1, "double roundtrip");
	json::Writer unfinished(output, sizeof(output)); unfinished.append("{");
	check(unfinished.view().empty() && !unfinished.fragment().empty(), "unfinished fragment is not ready");
	check(!unfinished.finish(storage, sizeof(storage)) && unfinished.view().empty(),
	    "incomplete document finish fails");
	json::Writer multiple(output, sizeof(output)); multiple.string("one"); multiple.string("two");
	check(!multiple.finish(storage, sizeof(storage)) && multiple.view().empty(), "multiple root values rejected");
	json::Writer finished(output, sizeof(output)); finished.append("{}");
	check(finished.finish(storage, sizeof(storage)) && finished.view() == "{}",
	    "complete validated document published");
	finished.append("[]"); check(finished.view().empty(), "new append invalidates completed view");
	check(!finished.finish(storage, sizeof(storage)), "reopened invalid document rejected");
	check(d.parse("{\"key\\u0000\":[18446744073709551615,-9223372036854775808,\"\\u0061\\u0000\",true,null]}")
	    == json::Error::None, "opaque nested values source");
	json::Writer copied(output, sizeof(output));
	check(copied.value(d, d.root()) && copied.finish(storage, sizeof(storage)), "opaque value serialization");
	check(d.parse(copied.view()) == json::Error::None && d.size(d.root()) == 1, "opaque object traversal");
	const auto copied_array = d.member(d.root(), std::string_view("key\0", 4));
	check(copied_array && d.size(copied_array) == 5 && d.equals(d.element(copied_array, 2), std::string_view("a\0", 2)),
	    "opaque array traversal and decoded NUL names");
	check(!d.memberAt(copied_array, 0).key && !d.element(copied_array, 5), "invalid traversal stays invalid");
	check(d.parse("\"\\u0061\\u0000\\uD83D\\uDE80\\\"\\\\\"") == json::Error::None,
	    "borrowed string source");
	json::Writer borrowed_string(output, sizeof(output));
	check(borrowed_string.string(d, d.root()) && borrowed_string.finish(storage, sizeof(storage)),
	    "borrowed string streaming serialization");
	check(d.parse(borrowed_string.view()) == json::Error::None
	    && d.equals(d.root(), std::string_view("a\0\xf0\x9f\x9a\x80\"\\", 8)),
	    "borrowed string preserves NUL Unicode and escapes");
	json::Writer insufficient_string(output, 3);
	check(!insufficient_string.string(d, d.root()) && insufficient_string.view().empty(),
	    "borrowed string overflow never publishes partial JSON");
	check(d.parse("null") == json::Error::None, "non-string source parse");
	json::Writer wrong_string(output, sizeof(output));
	check(!wrong_string.string(d, d.root()) && !wrong_string.valid(), "borrowed string rejects non-string value");
	mcp::Workspace workspace{storage, sizeof(storage), output, sizeof(output)};
	mcp::Tool tools[] = {{"echo", "Echo text", "{\"type\":\"object\"}", echo}};
	mcp::Endpoint endpoint(tools, 1, workspace);
	auto rpc = [&](std::string_view request, std::string_view expected, int status = 200)
		{
			auto reply = endpoint.dispatch(request);
			check(reply.status == status && (expected.empty() || reply.body.find(expected) != std::string_view::npos),
			    "RPC reply");
			if(!reply.body.empty()) check(d.parse(reply.body) == json::Error::None, "reply valid JSON");
		};
	rpc("{", "-32700"); rpc("[]", "-32600");
	rpc(R"({"jsonrpc":"2.0","id":null,"method":"ping"})", "-32600");
	rpc(R"({"jsonrpc":"2.0","id":18446744073709551615,"method":"ping"})", "18446744073709551615");
	rpc(R"({"jsonrpc":"2.0","id":"a\u0000b","method":"ping"})", "a\\u0000b");
	rpc(
		R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2099-01-01","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})",
		"2025-11-25");
	rpc(R"({"jsonrpc":"2.0","id":2,"method":"initialize"})", "-32602");
	rpc(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", "", 202);
	rpc(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})", "inputSchema");
	rpc(R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"cursor":"next"}})", "-32602");
	rpc(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}}})",
	    "hello");
	rpc(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"echo","arguments":{"text":2}}})", "-32602");
	rpc(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"absent"}})", "-32602");
	rpc(R"({"jsonrpc":"2.0","id":6,"method":"missing"})", "-32601");
	check(endpoint.dispatch("{}", "2099-01-01").status == 200, "unsupported version");
	std::string oversized(endpoint.config().limits.input_bytes + 1, ' ');
	check(endpoint.dispatch(oversized).status == 413, "configured input bound");
	workspace.output_capacity = 256;
	std::string text(256, 'x');
	std::string request =
	    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\""
	    + text + "\"}}}";
	check(endpoint.dispatch(request).status == 500, "output capacity no partial JSON");
	workspace.output_capacity = sizeof(output);
	const std::string meta =
	    R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}})";
	auto modern = [&](std::string_view method, std::string_view extra, std::string_view name, int status,
	    std::string_view expected)
		{
			std::string request = "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"" + std::string(method)
			    + "\",\"params\":{" + meta + std::string(extra) + "}}";
			auto reply = endpoint.dispatch(request, "2026-07-28", method, name, true);
			check(reply.status == status && reply.body.find(expected) != std::string_view::npos, "modern RPC");
			check(d.parse(reply.body) == json::Error::None, "modern response valid");
		};
	modern("server/discover", "", "", 200, "supportedVersions");
	modern("ping", "", "", 200, "resultType");
	modern("tools/list", "", "", 200, "resultType");
	modern("tools/call", R"(,"name":"echo","arguments":{"text":"modern"})", "echo", 200, "modern");
	modern("tools/call", R"(,"name":"echo","arguments":{"text":"modern"})", "wrong", 400, "-32020");
	modern("missing", "", "", 404, "-32601");
	check(endpoint.dispatch(R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})",
	    "2026-07-28", "server/discover", {}, true).status == 400, "modern metadata required");
	std::string modern_request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{" + meta +
	    "}}";
	check(endpoint.dispatch(modern_request, "2026-07-28", "wrong", {},
	    true).body.find("-32020") != std::string_view::npos,
	    "method mirror mismatch");
	check(endpoint.dispatch(modern_request, "2025-11-25", "server/discover", {},
	    true).body.find("-32020") != std::string_view::npos,
	    "version mirror mismatch");
	check(endpoint.dispatch(modern_request, "2099-01-01", "server/discover", {}, true).status == 400,
	    "unknown version rejected");
	json::Writer integral_real(output, sizeof(output)); integral_real.real(1.0);
	check(integral_real.finish(storage,
	    sizeof(storage)) && d.parse(integral_real.view()) == json::Error::None && d.root().type() == json::Type::Real,
	    "real type survives output");
	mcp::Endpoint http_endpoint(tools, 1, workspace, "/mcp", trusted);
	HandlerBase *handlers[] = {&http_endpoint};
	Exchange exchange;
	exchange.input = modern_request;
	exchange.headers = {{"Accept", "application/json,text/event-stream"}, {"Content-Type", "application/json"},
		{"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", "server/discover"}};
	auto run = [&](int expected)
		{
			(void)dispatchHttp(exchange, handlers, 1, {}, {}, {}, {});
			check(exchange.status == expected, "HTTP status");
		};
	run(200);
	exchange.headers["Origin"] = "http://attacker.example";
	exchange.headers["Host"] = "attacker.example"; run(403);
	exchange.headers["Origin"] = "http://192.0.2.10"; run(200);
	exchange.headers["Accept"] = "application/json-evil,text/event-stream-evil"; run(406);
	exchange.headers["Accept"] = "application/json;q=0,text/event-stream"; run(406);
	exchange.headers["Accept"] = " Application/JSON ;q=0.8 , text/event-stream;q=1.0"; run(200);
	exchange.headers["Content-Type"] = "Application/JSON; charset=UTF-8"; run(200);
	exchange.headers["Mcp-Method"] = "tools/list"; run(400);
	exchange.headers["Mcp-Method"] = "tools/call";
	exchange.input = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{" + meta
	    + ",\"name\":\"echo\",\"arguments\":{\"text\":\"encoded header\"}}}";
	exchange.headers["Mcp-Name"] = "=?base64?ZWNobw==?="; run(200);
	check(exchange.output.find("encoded header") != std::string::npos, "base64 tool header decoded");
	exchange.headers["Mcp-Name"] = "=?base64?ZWNobx==?="; run(400);
	check(exchange.output.find("-32020") != std::string::npos, "invalid base64 pad JSON error");
	exchange.headers["Mcp-Name"] = "=?base64?!!!!?="; run(400);
	check(exchange.output.find("-32020") != std::string::npos, "invalid base64 alphabet JSON error");
	exchange.headers["Mcp-Name"] = "echo";
	exchange.headers["MCP-Protocol-Version"] = std::string(80, 'x'); run(400);
	check(exchange.output.find("-32020") != std::string::npos, "oversize protocol header JSON error");
	exchange.headers.erase("Origin"); exchange.method = HttpMethod::Get; run(405);
	exchange.method = HttpMethod::Delete; run(405);
	std::cout << checks << " checks passed; reader=" << json::backendName() << " Endpoint=" << sizeof(endpoint) << '\n';
}
