# Public snapshot audit — 2026-11-07

Target: [NaBrIks/NaWServer](https://github.com/NaBrIks/NaWServer), `main`.
The owner authorized publication, commit, push and searchable GitHub topics.
The public branch starts from one audited snapshot. Earlier local branches,
stash and recovery refs remain private; their complete Git bundle was verified
before editing. The original working files were also saved locally.
This history boundary does not change [source provenance](2026-10-30_provenance.md)
or the separate [third-party licenses](../../THIRD_PARTY_NOTICES.md).

## Included and excluded

The snapshot includes the portable library, CMake/Kconfig source registries,
optional modules, Windows/Linux/ESP-IDF/Pico adapters, tests, five demo families,
shared frontend and its dependency lock, packed web assets, benchmark summaries,
architecture diagrams and their editable SeMaps source.

Device API keys, Wi-Fi settings, host-specific SeMaps authentication, raw local
measurements, backups, SDKs, build output and dependency caches are excluded.
Published device addresses use documentation placeholders; machine paths and
actual board identifiers were removed. Pico tools require an explicit target or
Wi-Fi profile rather than selecting the author's board/network by default.
Author attribution and vendor copyright/license notices are retained.

## Checks on a separate source-only copy

| Check | Result |
|---|---|
| Windows native, MSVC, Debug | 65 passed; 3 libstdc++ allocation-count probes remain explicitly disabled on MSVC |
| Compact host, Release, SSE and WS builds | Both built; 2 tests passed, including live HTTP/SSE, API-key rotation and graceful shutdown |
| Shared Vue frontend | All 4 production profiles built; 37 tests passed after the builds |
| PowerShell | All included scripts parse; Pico tools reject missing explicit targets before device/network access |
| Local Markdown links | Targets and filename case checked against the source manifest, without relying on sibling projects |
| Gitleaks 8.30.1 | Current source, locally reachable history and unpacked committed gzip web assets scanned with default detectors and recursive decoding |
| Known local credentials | Exact-value search found no matches in the publication files |
| Local identifiers | No actual LAN addresses, board identifiers or machine-specific paths found in the publication files or unpacked web assets |

Gitleaks exceptions are limited to exact public WebSocket handshake nonces in
test paths and hexadecimal SHA-256 evidence in named historical report files.
Default detectors remain enabled. Synthetic test addresses/identifiers and
upstream author contacts are intentional. No unreviewed detector findings remain.
These checks reduce publication risk; they are not proof that every possible
secret or provenance issue can be detected automatically.

The GitHub repository had no issues/PRs, releases/assets, Actions runs/artifacts,
wiki or Pages content before publication. Only the audited `main` is pushed;
private recovery refs and old local branches are not uploaded.

## Validation boundaries

This publication check does not flash boards or repeat earlier hardware and
long-duration benchmarks. Those remain separately dated evidence. SDKs and
toolchains are external prerequisites, not missing vendored source. Frontend
builds/tests used a fresh dependency install from the lockfile and the local
package cache. Fragment anchors and every external historical URL are outside
the relative-file link check.
