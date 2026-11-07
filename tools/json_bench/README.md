# JSON reader comparison

This fixture compares the same immutable inputs and common output writer with
the selected bounded, yyjson or RapidJSON reader. It is a bounded acceptance measurement,
not a general library benchmark or worst-case memory guarantee.

Six fixtures cover a small MCP request, tool schemas, Unicode/embedded NUL,
exact integer boundaries/double, depth eight and a 512-byte string. There are
200 iterations (1200 parses). The corpus reaches 127 logical values; configured
limits permit larger documents. `reader_parse_peak_bytes` is the furthest
observed allocator payload end, including the arena prefix and preceding pool
bookkeeping; it is not all stack, HTTP, writer-validation or application RAM.

## Host

With an existing C/C++17 toolchain supporting floating-point `from_chars`:

```text
cmake -S tools/json_bench -B build/json-bounded -DNAWSERVER_JSON_BACKEND=bounded
cmake --build build/json-bounded
cmake -S tools/json_bench -B build/json-yyjson -DNAWSERVER_JSON_BACKEND=yyjson
cmake --build build/json-yyjson
```

Run `json_bench` in each build directory. Add
`-DNAWSERVER_BENCH_EXISTING_YYJSON=ON` to both profiles for the reuse comparison.
That profile really calls yyjson from the application consumer before our reader,
consumes its result and frees its document. The application uses yyjson's normal
allocator; that heap cost is separate from the MCP reader's fixed arena. Both
consumers link the same target. No second yyjson implementation is compiled.

For an independent RapidJSON application consumer, configure both backend profiles
with `-DNAWSERVER_BENCH_EXISTING_RAPIDJSON=ON`:

```text
cmake -S tools/json_bench -B build/json-bounded-rapidjson-app -DNAWSERVER_JSON_BACKEND=bounded -DNAWSERVER_BENCH_EXISTING_RAPIDJSON=ON
cmake --build build/json-bounded-rapidjson-app
cmake -S tools/json_bench -B build/json-rapidjson-rapidjson-app -DNAWSERVER_JSON_BACKEND=rapidjson -DNAWSERVER_BENCH_EXISTING_RAPIDJSON=ON
cmake --build build/json-rapidjson-rapidjson-app
```

In each profile the same application consumer parses every fixture with a
RapidJSON DOM and UTF-8 validation, traverses the result into the reported
checksum, then releases the document at function return. Its default allocator
is independent from our fixed arena. This compares ordinary application use of
RapidJSON against the bounded reader and against our RapidJSON adapter. Sharing
the same pinned headers does not guarantee template SAX and DOM code instantiate
the same implementation or collapse to one object copy. The RapidJSON consumer
flag and yyjson consumer flag are mutually exclusive; CMake rejects selecting
both.

## ESP-IDF fixture

`device/` is a separate no-Wi-Fi/no-LCD C6 application using the same `bench.cpp`.
Select `CONFIG_NAWSERVER_JSON_BOUNDED=y` or `CONFIG_NAWSERVER_JSON_YYJSON=y`, keeping
SDK, CPU clock, compiler flags and all other settings identical. The latter
uses the repository's pinned optional yyjson source. The same CMake reuse flag
adds the independent application consumer to either firmware.
`-DNAWSERVER_BENCH_EXISTING_RAPIDJSON=ON` selects the independent RapidJSON DOM
consumer instead; it uses the pinned `third_party/rapidjson/include` headers with
a normal application allocator in either bounded or RapidJSON reader builds.

The fixture waits five seconds after boot for the serial monitor, reports heap
and stack, runs the short corpus, then prints `JSON_BENCH_DONE result=0` on
success. Hardware identity and flash write ranges must be checked before each
deployment. Restore the ordinary demo after measurements. This fixture does not
configure Wi-Fi or change saved credentials/NVS.

Use linker maps and BIN sizes for the ESP Flash comparison; host executable size
is not ESP Flash consumption. Full-demo images are a separate comparison from
this isolated program. Record source/configuration and exact toolchain together
with results in `docs/readme_build_memory_log_ru.md`.
