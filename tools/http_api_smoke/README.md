# Synchronous HTTP API acceptance

Run only against current examples/minimal on the authorized ESP32-C3:

```powershell
go -C tools/smoke test ./...
go -C tools/smoke run -buildvcs=false . DEVICE_IP
go -C tools/http_api_smoke run -buildvcs=false . http://DEVICE_IP
```

The first network tool retains the earlier HTTP/form/Basic/DELETE/body/WS coverage.
The second verifies Header[]/ResponseOptions precedence, both endpoint signatures,
binary/NUL echo, empty response, redirect, bounded text builder, bounded header read,
response-only authentication, second-send rejection, gzip/static assets and 2/4
concurrent clients. It does not mutate device state, NVS or flash.

The default runs both concurrency profiles (2 and 4 clients), preserving the old
single-URL command. When other applications already hold device connections, select
an explicit smaller stand profile before the URL:

```powershell
go -C tools/http_api_smoke run -buildvcs=false . --max-concurrent-clients=2 http://DEVICE_IP
```

Only 2 or 4 is accepted. The 2-client mode runs only that parallel profile and
reports it as such (78 checks instead of the default 198); it is not evidence for
4 concurrent clients. Count existing browser/WS/SSE connections against the firmware's
TCP connection limit before choosing a profile. For example, four existing browser
connections plus four probe clients exceed a seven-connection HTTPD limit; the
explicit 2-client profile fits that budget. The tool does not close other clients,
change server limits or automatically downgrade after a failure.

Authorized app0-only flash and boot capture (after exclusive hardware coordination):

```powershell
& tools/http_api_smoke/Run-C3.ps1 -ExpectedMac VERIFIED_MAC -Port COM10
```

This uses vendor esptool, verifies chip/MAC and image size/hash, writes only app0
0x10000 within the already verified partition layout, captures boot/property/unit/
loopback/network-ready evidence and leaves all identity/raw logs under ignored local/.
No flash backups, partition-table/NVS/data writes or erase-all commands.

Host and device source tests: tests/response_test.cpp. Generic deferred HTTP APIs
are intentionally absent. API contracts and validation limits are recorded
in docs/readme_http_api_ru.md and docs/readme_http_api_verification_ru.md.
