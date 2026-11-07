# Library SSE example (ESP32-C3)

Uses `sse.h` / `NaWServer::sse::EventSource` from the component source list.
No tools runtime dependency or implementation copy. The application owns Wi-Fi/NVS
and explicit SSE startup. Optional ignored wifi_secret.hpp is searched in this main
directory and examples/minimal/main; compilation does not require credentials.

```powershell
pio run -d examples/sse -e c3
curl.exe -N -u test:test-only http://DEVICE/events
curl.exe -u test:test-only -X POST --data-binary "hello" http://DEVICE/publish
```

GET /health is public. /events and /publish use existing auth/policy and public
fixture credentials, not production secrets. Publish reports queued/refused,
not delivery. UTF-8 is bounded by encoded Frame capacity; no replay/history.
Shutdown order: stop publishers/admission, drain events.stop, then server.end.
Never stop from a URI callback. The common TCP engine serves plain HTTP; NVS is never erased as a fallback.
