# Library SSE tests (C++17)

The canonical runtime is compiled by the library from `src/sse`, with public
`sse.h` / `NaWServer::sse::EventSource`. This directory contains checks and fixtures;
it has no second implementation.
See the [public guide](../../docs/readme_sse_ru.md),
[library verification](../../docs/readme_sse_library_verification_ru.md), and
[`examples/sse`](../../examples/sse) for application usage.

Design and boundaries:
[Russian guide](../../docs/readme_sse_design_ru.md).
Evidence: [verification](../../docs/readme_sse_verification_ru.md).
Implemented synchronous Request/Response boundary:
[integration note](../../docs/readme_sse_integration_ru.md),
[C3/LAN verification](../../docs/readme_sse_integration_verification_ru.md).
The reserve/claimExternal/activate bridge records ownership before worker I/O.
Both device fixtures use the common TCP adapter, declarative routes and header-stage authentication.

Portable checks (native C++17 toolchain, CMake):

```powershell
cmake -S tools/sse -B local/sse-host
cmake --build local/sse-host --config Release
ctest --test-dir local/sse-host -C Release --output-on-failure
```

For Zig use Ninja, `-DCMAKE_CXX_COMPILER=PATH/zig.exe` and
`-DCMAKE_CXX_COMPILER_ARG1=c++`. All checks remain active with NDEBUG.

Pinned standalone ESP32-C3 firmware (existing vendor PlatformIO tools):

```powershell
& tools/sse/Build.ps1
# Only after verifying the other hardware chat is idle and identifying this C3.
Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Name,PNPDeviceID
& tools/sse/Run-C3.ps1 -ExpectedMac 'CURRENT-VERIFIED-C3-MAC' *> local/sse-flash.log
```

Run-C3 writes only app0 at 0x10000, bounded to 0x280000 bytes; the already verified
partition layout is required. It re-identifies C3/device, hashes the firmware and
uses vendor write verification. No flash backup or data-partition writes. Real
identity/raw serial/build logs remain ignored under local/. The app runs loopback
HTTP/SSE/WS checks without Wi-Fi/NVS, stops the server, then idles; reset repeats checks.

Integration firmware links the real core and includes the existing ignored
`examples/minimal/main/wifi_secret.hpp` without printing its values:

```powershell
& tools/sse/Build.ps1 -Integration
# Terminal A, only after board release and fresh identity/partition checks:
& tools/sse/Run-C3.ps1 -Integration -ExpectedMac 'CURRENT-VERIFIED-C3-MAC' -TimeoutSeconds 420 *> local/sse-integration-flash.log
# Terminal B, use the fresh SSE_READY address in ignored serial capture:
go -C tools/sse/lan_smoke run . -url 'http://CURRENT-C3-IP' -out '../../../local/sse-integration-lan.json'
# Open http://CURRENT-C3-IP/browser; observe SSE PASS 3; then stop with active streams:
go -C tools/sse/lan_smoke run . -url 'http://CURRENT-C3-IP' -stop-only
```

Terminal A captures through SSE_INTEGRATION_DONE and releases serial. The fixture
uses public credentials `test:test-only`; this is not production configuration.
It does not erase NVS and uses Wi-Fi storage RAM. The stopped reader requests
SO_RCVBUF=1024; effective OS receive-buffer/window size is not certified by this test.
After the library run, restore minimal and run both tools/smoke and
tools/http_api_smoke; record the current firmware in the verification report.
Do not run hardware scripts while another chat owns the C3.

Encoder and Sessions do not allocate. The common adapter owns bounded stream
storage and transport handles; successful activation transfers the request lease
to the stream until completion or cancellation. The application stops EventSource
and HttpServer before destroying them. General deferred HTTP requests and native
HTTPD async-request handles are not exposed. TLS is not provided by this fixture.
Historical verification files describe the original revisions and do not certify
the migrated firmware until a new device run is recorded.
