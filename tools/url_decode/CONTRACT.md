# Percent decoder experiment contract (2026-10-19)

This contract precedes implementation. These are byte transforms, not URL parsers.
RFC 3986 section 2.1 defines `%HH`; the WHATWG form parser supplies the separate
plus-to-space step. Strict rejection below is a deliberate application policy,
not a claim to implement WHATWG's forgiving percent decoder.

- Input is `(pointer, length)`, destination `(pointer, capacity)`. No terminator is
  read or appended. All octets including raw/decoded NUL and 0xff are data. UTF-8
  is preserved as bytes; neither validation nor Unicode normalization occurs.
- Decode exactly once; `%25` produces `%`, `%2B` produces `+` in either mode.
  `Mode::form` maps raw `+` to space; `Mode::path` preserves it. The form option
  is appropriate for form-style query components, not every generic URI query.
- `%` requires two ASCII hex digits, case insensitive. Truncated/nonhex escapes
  return `invalid_escape` at the percent's input offset. No replacement or retry.
- `Result` gives status, consumed input bytes, and written output bytes. Successful
  consumed equals input length. On error, only the complete decoded prefix is
  meaningful; destination may contain that prefix. It is NOT transactional.
- Exact capacity succeeds. Capacity exhaustion returns `no_space` before consuming
  the next output octet. An invalid escape at that same position takes precedence.
- Empty input succeeds with null pointers and zero capacity. A null pointer with
  a nonzero corresponding extent returns `bad_argument` without writes.
- Exact in-place (`dst == src`) is supported. Other intersecting input/destination
  extents return `overlap` without writes; disjoint buffers are supported. No
  allocation occurs in the decoder. Caller must provide real accessible extents.

Sources: https://www.rfc-editor.org/rfc/rfc3986#section-2.1,
https://url.spec.whatwg.org/#application/x-www-form-urlencoded-parsing,
https://eel.is/c++draft/charconv.from.chars (integer API, C++17 supported toolchain).
No standard C++ URL decoder is assumed. The candidate implementations follow this contract.
