# Recommended shared ESP-IDF SDK baseline

This is the intended reproducible default, not a mandatory directory layout or
an exclusive list of allowed versions. See the username-independent
[environment layout, versions and setup guide](readme_development_environment_ru.md).
The default Core is `~/.platformio` (`%USERPROFILE%\.platformio` on Windows).
Core/packages paths may be selected through PlatformIO's environment variables.
Versioned SDK folders below are relative to the selected packages directory.

- `framework-espidf-v5.5.6`: default profiles; RISC-V GCC `14.2.0+20260121`.
- `framework-espidf-v6.1`: IDF 6 profiles; RISC-V GCC `15.2.0+20251204`.
- Current Xtensa profiles use GCC `14.2.0+20251107`.

Samples pin Espressif32 8.0.0 and use `symlink://` to reuse an installed SDK.
Alternative paths and version profiles are supported when explicitly configured
and validated. Before verification, report missing dependencies rather than
silently installing or switching tools as a side effect of a check.

Minimal and Showcase provide these independent C3 environments:

| Environment | SDK | Backend |
|---|---|---|
| `c3` | 5.5.6 | shared TCP |
| `c3_tcp` | 5.5.6 | shared TCP |
| `c3_idf6` | 6.1 | shared TCP |
| `c3_tcp_idf6` | 6.1 | shared TCP |

Every environment now uses the common TCP engine. Legacy environment names only
preserve separate build/config directories. There is no HTTPD backend selector
or HTTPD handshake dependency. Compile-check uses `IdfTransport`, `TcpHttpBackend`,
`HttpServer` and `sse::TcpAdapter`; IDF supplies BSD/lwIP sockets and FreeRTOS.
The component's direct platform requirements are `freertos`, `lwip`, `esp_timer`.
TLS is not included in this migration.

`CONFIG_NAWSERVER_AUTH` and `CONFIG_NAWSERVER_CLIENT_QUOTAS` select independent
optional middleware features. `CONFIG_NAWSERVER_TCP_MANAGED` selects physical
managed request buffers and keep-alive support. Configure `TcpOptions.http_memory`
and `TcpOptions.keep_alive` at runtime; client policy is installed through
`HttpServer::setHeaderPipeline`, never through transport accept options.

Historical IDF 5.4.0 / 6.0.3 reports remain evidence for those exact older builds.
They do not override the current SDK choice or prove compatibility with the new
versions. Current build/memory evidence is appended to
[the log](readme_build_memory_log_ru.md); hardware acceptance is recorded separately.
