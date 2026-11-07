# Library SSE implementation

Public entry: `<NaWServer/sse.h>`, `NaWServer::sse::EventSource`. One canonical implementation;
tools/sse contains only harnesses/fixtures. Component CMake lists these sources.
Implementation headers required by static ownership are in
`include/NaWServer/detail/sse`; applications should not depend on these headers.
EventSource borrows an explicit SSE Backend; public sse.h includes no SDK.
Memory/Windows backends use the shared bounded QueuedAdapter and owned Streams;
IDF keeps its native async-request/FreeRTOS runtime. The encoder and Sessions engine
are canonical for all three. No task/socket starts from construction or inclusion.
See [usage](../../docs/readme_sse_ru.md) for limits, ownership and native restrictions.

Implementation references are WHATWG SSE and official ESP-IDF 5.4 APIs.
