// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/json.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#ifndef NAWSERVER_BENCH_EXISTING_YYJSON
#define NAWSERVER_BENCH_EXISTING_YYJSON 0
#endif
#ifndef NAWSERVER_BENCH_EXISTING_RAPIDJSON
#define NAWSERVER_BENCH_EXISTING_RAPIDJSON 0
#endif
#ifndef NAWSERVER_JSON_BENCH_HOST
#define NAWSERVER_JSON_BENCH_HOST 0
#endif
#if NAWSERVER_BENCH_EXISTING_YYJSON
#include <yyjson.h>
#endif
#if NAWSERVER_BENCH_EXISTING_RAPIDJSON
#include <rapidjson/document.h>
#endif
#if NAWSERVER_JSON_BENCH_HOST
#include <chrono>
#endif

namespace
{
	using namespace NaWServer;
	alignas(std::max_align_t) unsigned char workspace[NAWSERVER_JSON_WORKSPACE_BYTES];
	char output[2048];
	volatile std::uint64_t checksum = 0;
	struct Fixture { const char *name; std::string_view source; };
	// Identical immutable inputs for both reader implementations. The tools
	// response models the four demo tools without borrowing any ESP headers.
	static constexpr Fixture fixtures[]{
		{"tiny_mcp",
		 R"JSON({"jsonrpc":"2.0","id":1,"method":"ping","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{},"io.modelcontextprotocol/clientInfo":{"name":"benchmark","version":"1"}}}})JSON"},
		{"four_tool_schemas",
		 R"JSON({"jsonrpc":"2.0","id":2,"result":{"resultType":"complete","tools":[{"name":"device_info","description":"Read device information","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"readOnlyHint":true}},{"name":"echo","description":"Echo UTF-8 text","inputSchema":{"type":"object","properties":{"text":{"type":"string","maxLength":256}},"required":["text"],"additionalProperties":false},"annotations":{"readOnlyHint":true}},{"name":"experiment_status","description":"Read experiment state","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"readOnlyHint":true}},{"name":"experiment_set","description":"Start or stop the application experiment","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["start","stop"]},"kind":{"type":"string","enum":["pi","pulse"]},"duration_ms":{"type":"integer"},"on_ms":{"type":"integer"},"period_ms":{"type":"integer"},"core":{"type":"integer"},"priority":{"type":"integer"}},"required":["action"],"additionalProperties":false},"annotations":{"readOnlyHint":false}}]}})JSON"},
		{"unicode_escapes",
		 R"JSON({"text":"\u041f\u0440\u0438\u0432\u0435\u0442 \ud83d\ude80","nul":"left\u0000right","escapes":"\"\\\/\b\f\n\r\t"})JSON"},
		{"exact_numbers",
		 R"JSON({"unsigned":18446744073709551615,"signed":-9223372036854775808,"real":3.141592653589793,"exponent":1.25e-30,"zero":-0})JSON"},
		{"depth_eight", R"JSON({"a":[{"b":[{"c":[{"d":[0]}]}]}]})JSON"},
		{"string_512", "{\"text\":\""
		 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		 "\"}"}
	};

#if NAWSERVER_BENCH_EXISTING_RAPIDJSON
	std::uint64_t rapidJsonChecksum(const rapidjson::Value &value);
#endif

	bool existingApplicationRead(std::string_view source)
	{
#if NAWSERVER_BENCH_EXISTING_YYJSON
		// Deliberately exercise the application's independent reader dependency
		// before our abstraction in BOTH backend builds. Its normal allocator is
		// application-owned; no mutable yyjson writer is pulled in by this test.
		yyjson_doc *document = yyjson_read(source.data(), source.size(), 0);
		if(!document) return false;

		checksum = checksum + yyjson_doc_get_read_size(document) + yyjson_doc_get_val_count(document)
		    + yyjson_get_len(yyjson_doc_get_root(document));
		yyjson_doc_free(document);
#elif NAWSERVER_BENCH_EXISTING_RAPIDJSON
		// This is a normal application-owned RapidJSON DOM allocation. It is
		// independent from the selected NaWServer reader and its fixed arena.
		rapidjson::Document document;
		document.Parse<rapidjson::kParseValidateEncodingFlag>(source.data(), source.size());
		if(document.HasParseError()) return false;

		checksum = checksum + rapidJsonChecksum(document);
#else
		(void)source;
#endif
		return true;
	}

#if NAWSERVER_BENCH_EXISTING_RAPIDJSON
	std::uint64_t hashBytes(const char *bytes, std::size_t size)
	{
		std::uint64_t hash = UINT64_C(14695981039346656037);
		for(std::size_t index = 0; index < size; ++index)
		{
			hash = (hash ^ static_cast<unsigned char>(bytes[index])) * UINT64_C(1099511628211);
		}
		return hash;
	}

	std::uint64_t rapidJsonChecksum(const rapidjson::Value &value)
	{
		std::uint64_t result = static_cast<std::uint64_t>(value.GetType()) + UINT64_C(1);
		if(value.IsObject())
		{
			result += value.MemberCount();
			for(auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
			{
				result += hashBytes(member->name.GetString(), member->name.GetStringLength());
				result += rapidJsonChecksum(member->value);
			}
		}
		else if(value.IsArray())
		{
			result += value.Size();
			for(auto element = value.Begin(); element != value.End(); ++element)
				result += rapidJsonChecksum(*element);
		}
		else if(value.IsString())
		{
			result += hashBytes(value.GetString(), value.GetStringLength());
		}
		else if(value.IsInt64())
		{
			result += static_cast<std::uint64_t>(value.GetInt64());
		}
		else if(value.IsUint64())
		{
			result += value.GetUint64();
		}
		else if(value.IsNumber())
		{
			const double number = value.GetDouble();
			std::uint64_t bits = 0;
			const auto *number_bytes = reinterpret_cast<const unsigned char *>(&number);
			for(std::size_t index = 0; index < sizeof(number); ++index)
				bits |= static_cast<std::uint64_t>(number_bytes[index]) << (index * 8);
			result += bits;
		}
		else if(value.IsBool())
		{
			result += value.GetBool() ? 1 : 0;
		}

		return result;
	}

#endif

	const char *applicationFailureName()
	{
#if NAWSERVER_BENCH_EXISTING_YYJSON
		return "yyjson";
#elif NAWSERVER_BENCH_EXISTING_RAPIDJSON
		return "rapidjson";
#else
		return "yyjson";
#endif
	}

	bool consume(const json::Document &document)
	{
		const auto root = document.root();
		checksum = checksum + document.count() + document.size(root);
		const auto text = document.member(root, "text");
		if(text)
		{
			char decoded[512]; std::size_t size = 0;
			if(!document.decode(text, decoded, sizeof(decoded), size)) return false;

			checksum = checksum + size + (size ? static_cast<unsigned char>(decoded[0]) : 0);
		}

		const auto integer = document.member(root, "unsigned");
		if(integer)
		{
			std::uint64_t value = 0;
			if(!document.unsignedInteger(integer, value) || value != UINT64_MAX) return false;

			checksum = checksum + (value & 255);
		}

		return true;
	}

	int failure(const char *phase, const Fixture &fixture, unsigned error, std::size_t peak)
	{
		std::printf("{\"backend\":\"%s\",\"ok\":false,\"phase\":\"%s\",\"fixture\":\"%s\","
		    "\"error\":%u,\"reserved_arena_bytes\":%zu,\"reader_parse_peak_bytes\":%zu}\n",
		    json::backendName(), phase, fixture.name, error, sizeof(workspace), peak);
		return 1;
	}

}

// Caller supplies a monotonic clock in microseconds. Static benchmark buffers
// make this a synchronous, single-caller diagnostic, not a request handler.
// The MCU wrapper may run it before starting concurrent server application work.
extern "C" int run_json_benchmark(std::uint64_t (*now_microseconds)(), std::size_t iterations)
{
	using namespace NaWServer;
	if(!now_microseconds || !iterations || iterations > 10000) return 2;

	std::size_t observed_peak = 0, maximum_values = 0, maximum_output = 0;
	// Untimed writer correctness gate. Destroy the source Document before
	// finish reuses its arena; yyjson's native DOM must not outlive that reuse.
	for(const auto &fixture : fixtures)
	{
		json::Writer writer(output, sizeof(output));
		{
			json::Document document(workspace, sizeof(workspace));
			const auto error = document.parse(fixture.source);
			if(error != json::Error::None)
				return failure("writer_input", fixture, static_cast<unsigned>(error),
				           document.workspaceHighWaterBytes());

			if(!writer.value(document, document.root()))
				return failure("writer_encode", fixture, 0, document.workspaceHighWaterBytes());
		}
		if(!writer.finish(workspace, sizeof(workspace))) return failure("writer_finish", fixture, 0, 0);

		if(writer.view().size() > maximum_output) maximum_output = writer.view().size();
	}
	const auto begin = now_microseconds();
	for(std::size_t iteration = 0; iteration < iterations; ++iteration)
	{
		for(const auto &fixture : fixtures)
		{
			if(!existingApplicationRead(fixture.source))
				return failure(applicationFailureName(), fixture, 0, observed_peak);

			json::Document document(workspace, sizeof(workspace));
			const auto error = document.parse(fixture.source);
			const auto peak = document.workspaceHighWaterBytes();
			if(peak > observed_peak) observed_peak = peak;

			if(document.count() > maximum_values) maximum_values = document.count();

			if(error != json::Error::None)
				return failure("reader_parse", fixture, static_cast<unsigned>(error), observed_peak);

			if(!consume(document)) return failure("reader_consume", fixture, 0, observed_peak);
		}
	}
	const auto end = now_microseconds();
	if(end < begin) return 2;

	std::printf("{\"backend\":\"%s\",\"ok\":true,\"reserved_arena_bytes\":%zu,"
	    "\"reader_parse_peak_bytes\":%zu,\"maximum_value_count\":%zu,\"output_reserved_bytes\":%zu,"
	    "\"maximum_encoded_output_bytes\":%zu,\"fixtures\":%zu,\"iterations\":%zu,\"parses\":%zu,"
	    "\"elapsed_us\":%llu,%s,\"checksum\":%llu}\n",
	    json::backendName(), sizeof(workspace), observed_peak, maximum_values, sizeof(output), maximum_output,
	    sizeof(fixtures) / sizeof(fixtures[0]), iterations, iterations * (sizeof(fixtures) / sizeof(fixtures[0])),
	    static_cast<unsigned long long>(end - begin),
#if NAWSERVER_BENCH_EXISTING_RAPIDJSON
	    "\"existing_application_rapidjson\":true",
#else
	    NAWSERVER_BENCH_EXISTING_YYJSON ? "\"existing_application_yyjson\":true" :
	    "\"existing_application_yyjson\":false",
#endif
	    static_cast<unsigned long long>(checksum));
	return 0;
}

#if NAWSERVER_JSON_BENCH_HOST
namespace
{
	std::uint64_t nowMicroseconds()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				   std::chrono::steady_clock::now().time_since_epoch()).count());
	}

}
int main()
{
	return run_json_benchmark(nowMicroseconds, 200);
}

#endif
