# Optional RapidJSON SAX reader

Headers from the official Tencent RapidJSON repository, pinned at
commit `24b5e7a8b27f42fa16b96fc70aade9106cf7102f` (2026-10-26 checkout):

- Source: https://github.com/Tencent/rapidjson/tree/24b5e7a8b27f42fa16b96fc70aade9106cf7102f
- SAX contract: https://rapidjson.org/md_doc_sax.html
- Upstream license and dependency notices: [LICENSE](LICENSE).

Only `include/` is vendored. The upstream `bin/jsonchecker/` data/code under the
JSON License is excluded. RapidJSON headers use MIT; the legacy `msinttypes`
headers retain their BSD notices and upstream license text. No SDK/package is
installed or updated by selecting this backend.

One documented local patch changes `reader.h`: when NumbersAsStrings is selected,
consume the exponent digits without applying RapidJSON's intermediate double
exponent range check. RawNumber's consumer owns checked conversion. Unmodified
upstream rejects valid `0e10000` before calling RawNumber, while the common
contract correctly represents it as zero. The patch also avoids intermediate
exponent integer overflow in raw mode; grammar checks and all non-raw reader
paths remain upstream behavior. The exact change is retained in
[`raw-number-exponent.patch`](raw-number-exponent.patch).

NaWServer uses the actual SAX reader with `Insitu`, `ValidateEncoding` and
`NumbersAsStrings`. One immutable source copy is placed in caller-owned fixed
storage. The reader decodes into that copy; compact eight-byte descriptors retain
navigation and numeric lexeme views. No RapidJSON DOM or second grammar parser
is built. The shared checked numeric conversion rejects overflow/underflow and
preserves exact signed/unsigned 64-bit values.

The selected recursive in-situ/raw-number path does not allocate RapidJSON
scratch storage. A no-scratch allocator prevents implicit use of the C runtime
allocator. Recursion consumes the caller's task stack; configured JSON depth and
the full application call chain must be tested on the real MCU. The configured
workspace reserves the copied input, alignment and descriptors; its high-water
measurement does not include task stack or the independent MCP output buffer.

The public Document/Value API and common bounded writer are unchanged. This is
an optional reader/validator adapter, not an alternative mutable writer DOM.
