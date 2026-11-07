# yyjson 0.13.0

Unmodified upstream reader/writer sources, vendored for reproducible optional
JSON-backend builds. This does not install or update an SDK/package environment.

- Upstream: https://github.com/ibireme/yyjson
- Tag: `0.13.0`
- Commit: `6447536015f3d600f3d65323b10976103b337ca7`
- License: MIT, preserved in `LICENSE` and source headers.
- `src/yyjson.h` SHA256: `ef803cda5c06b8962face6dfa39c3284b3bcf3e73f4d7317664690cc779679ca`
- `src/yyjson.c` SHA256: `d2d58ef0a3b2267862dc363832c4f3185fd375aea933d5d3419d1a940d6ece2e`

Only the selected yyjson backend links these sources. Applications can supply an
existing compatible CMake target with `NaWServer_YYJSON_TARGET` instead of building
this copy. The application owns that target's version and compile options; the
shared JSON contract tests still apply. No global yyjson allocator is replaced.
