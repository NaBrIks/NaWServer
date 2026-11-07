# URL percent decoder: reproducible C3 experiment

Contract: [CONTRACT.md](CONTRACT.md). Russian interpretation/results:
[docs/readme_url_decode_benchmark_ru.md](../../docs/readme_url_decode_benchmark_ru.md).
This directory is an experiment; it is not part of the NaWServer component's source
list. It retains four candidates: Direct, Lookup, from_chars and Spans.

Subsequent owner-requested production integration lives in src/http/url_decode.* and
src/http/form_parameters.*; see [integration guide](../../docs/readme_url_decode_integration_ru.md).
The component API statement above describes the benchmark stage. These four
experimental candidates/results are retained; they are not the production build.

## Build / correctness

From the repository root, Windows PowerShell, with existing PlatformIO:

```powershell
& tools/url_decode/Build.ps1
& tools/url_decode/Test-Host.ps1 -Cxx 'PATH/TO/zig.exe' -CompilerPrefix @('c++')
# Or use an ordinary GCC/Clang executable, without CompilerPrefix.
# Also available as url_decode_test in tests/CMakeLists.txt.
```

PlatformIO Core is isolated under ignored `local/pio-core`. Platform 6.10.0,
ESP-IDF 3.50400.0 (=5.4.0), RISC-V GCC 14.2.0+20241119 are pinned in
device/platformio.ini. A preinstalled identical toolchain may be reused by a
local junction; do not replace it with another version or edit vendor sources.
The first build downloads the SDK/build dependencies. Vendor PlatformIO/IDF tools
internally use Python; no agent-authored Python tools are provided or required.

PlatformIO's SDK default `gnu++2b` must be removed with `build_unflags`.
CMake compile options alone do not guarantee the flags of SCons-compiled sources.
Both decoder.cpp and app_main contain a C++17 static_assert; runtime META must say
`cplusplus=201703`. Optimization `-Os`, no LTO, 160 MHz, tick 1000 Hz.

## Hardware (identified C3 only)

Do not select another device or assume an old COM number. Inspect current ports,
read chip-id and obtain the expected MAC from that identified test C3. Real
identifiers belong in ignored local records, not this README. Flash.ps1 requires
that identity, finds the current unique Espressif USB Serial/JTAG port and checks
chip/MAC before any write. Multiple matching ports cause refusal. Current owner
instruction (2026-10-19): **do not create flash backups**.

```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Name,PNPDeviceID
# Substitute the verified device MAC; redirect hardware logs into ignored local/.
& tools/url_decode/Flash.ps1 -ExpectedMac 'VERIFIED-MAC' *> local/flash-url-decode.log
# Immediately use the port returned by Flash.ps1 (boot waits seven seconds).
& tools/url_decode/Capture.ps1 -Port 'CURRENT-PORT' -OutputPath (Join-Path (Get-Location) 'local/url-decode-unicode-serial.log') -TimeoutSeconds 480
go -C tools/url_decode/analyze run -buildvcs=false . ../../../local/url-decode-unicode-serial.log ../results/unicode
```

Flash writes **app0 at 0x10000 only** (max 0x280000 bytes), with esptool data hash
verification; no erase-all, bootloader, partition table or data partition writes.
This assumes the already verified project partition layout with app0 at 0x10000.
Do not run it against a board with another/unknown layout. The benchmark replaces
the C3 application and does not start Wi-Fi, HTTP or the showcase. After DONE it
idles; each reset repeats the experiment. It does not write NVS or other data.
Esptool 5.1.0 uses `--no-progress` plus UTF-8 environment to avoid redirected
Windows console encoding failures. These are vendor CLI settings, not Python glue.

## Measurement / extraction

Three rounds, rotating variant order; 9 batches of 32 calls per warm/owning cell,
15 single calls per cache-pressure cell. There are 19 workloads: the original 11
plus eight Unicode cases (Russian/Chinese/Japanese/emoji, raw and percent).
Each Unicode output is exactly 1024 bytes; input is 1024 raw or 3072 percent bytes.
utf8_fixtures.hpp repeats complete original phrases/emoji sequences, fills with
complete code points and at most three ASCII spaces. Emoji include ZWJ sequences,
variation selectors and a skin tone modifier. Fixture generation is outside timing.
The four candidates get exact-capacity/byte/canary checks in correctness.cpp;
all four candidates pass eight device checks (32 comparisons). Measured output is consumed with a
checksum and compiler memory barriers, no serial printing occurs during timing.
The batch loop includes copying input to the common fixed in-place buffer; owning
is a separate experiment, allocating an output std::string, then decoding from
the immutable source. Its different ownership is not a pure allocation-only delta.

Hooks count allocations, frees and requested bytes; separate untimed MEMORY rows
measure live allocator cost including block overhead and retained heap. Global
minimum heap includes correctness fixtures and is not an algorithm heap peak.
Decoder stack figures use -fstack-usage; they are own frames, not full call chains.

The Go analyzer refuses missing DONE/correctness/C++17/160MHz, watchdog diagnostics,
incomplete counts and unbalanced allocations. Current extended run expects
**10044 numeric samples, 298 groups, 76 memory profiles, eight UTF8_CHECK records**.
The original capture without UTF8_CHECK remains supported (5724/170/44).
Original results stay in results/; the extension is saved in results/unicode/.
It extracts numeric-only CSV, discards
all raw serial/boot text, and writes tables with median/min/max. Raw serial logs,
firmware, ELF/map, downloads and identity records remain ignored. `summary.csv`
subtracts median matched overhead for warm/pressure, then reports cycles per
input byte and ns/op at 160 MHz; owning has no overhead subtraction. This is an
estimate, not a claim that every instruction in the subtraction is identical.

Use nm -S -C on the actual firmware ELF for linked symbol sizes, size -A for
firmware sections, and decoder.cpp.su for own stack frames. Helpers are shared;
do not add shared helper costs repeatedly and call that the firmware's total size.
Host correctness and showcase simulation are not hardware performance evidence.
