// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include "detail/configuration_names.h"
// Selection does not enable MCP. CMake/Kconfig selects bounded by default;
// explicit compiler users select exactly one reader implementation.
#ifndef NAWSERVER_JSON_BOUNDED
#define NAWSERVER_JSON_BOUNDED 0
#endif
#ifndef NAWSERVER_JSON_YYJSON
#define NAWSERVER_JSON_YYJSON 0
#endif
#ifndef NAWSERVER_JSON_RAPIDJSON
#define NAWSERVER_JSON_RAPIDJSON 0
#endif
#if (NAWSERVER_JSON_BOUNDED != 0 && NAWSERVER_JSON_BOUNDED != 1) || (NAWSERVER_JSON_YYJSON != 0 && \
	NAWSERVER_JSON_YYJSON != 1) || (NAWSERVER_JSON_RAPIDJSON != 0 && NAWSERVER_JSON_RAPIDJSON != 1)
#error "NaWServer JSON backend flags must have value 0 or 1"
#endif
#if NAWSERVER_JSON_BOUNDED + NAWSERVER_JSON_YYJSON + NAWSERVER_JSON_RAPIDJSON != 1
#error "NaWServer JSON requires exactly one reader backend"
#endif
// Strings count decoded UTF-8 bytes; values include object keys. The arena
// capacity is independent and never grows or falls back to the process heap.
#ifndef NAWSERVER_JSON_INPUT_BYTES
// Maximum JSON document passed to the reader. This neither reserves input RAM
// nor changes the HTTP backend's independent body/header receive storage.
#define NAWSERVER_JSON_INPUT_BYTES 4096
#endif
#ifndef NAWSERVER_JSON_STRING_BYTES
// Maximum decoded UTF-8 bytes per key/string, including embedded NUL bytes.
// Unicode escapes count after decoding; this is not a Unicode character count.
#define NAWSERVER_JSON_STRING_BYTES 512
#endif
#ifndef NAWSERVER_JSON_CONTAINER_ITEMS
// Maximum members per object or elements per array, independently at each level.
#define NAWSERVER_JSON_CONTAINER_ITEMS 32
#endif
#ifndef NAWSERVER_JSON_DEPTH
// Maximum nested containers; root container is level one. Raising this also
// raises bounded parser/validation and value serialization recursion stack use.
#define NAWSERVER_JSON_DEPTH 8
#endif
#ifndef NAWSERVER_JSON_VALUE_CAPACITY
// Logical values admitted in one document: containers, scalars and object keys.
// This count is independent of arena bytes and is enforced by both readers.
#define NAWSERVER_JSON_VALUE_CAPACITY 256
#endif
#ifndef NAWSERVER_JSON_WORKSPACE_BYTES
// Caller-owned reader arena reserved once by demos, never grown implicitly.
// Bounded needs eight bytes per value; yyjson additionally owns its native DOM,
// decoded input copy and pool bookkeeping. The yyjson default is a separate
// resource profile, not a promise that every allowed input fits this arena.
#if NAWSERVER_JSON_YYJSON
#define NAWSERVER_JSON_WORKSPACE_BYTES 16384
#elif NAWSERVER_JSON_RAPIDJSON
// Immutable caller input is copied once for in-situ decoding by the SAX reader;
// compact indexed values share this fixed arena. No growing native DOM is used.
#define NAWSERVER_JSON_WORKSPACE_BYTES 8192
#else
#define NAWSERVER_JSON_WORKSPACE_BYTES (NAWSERVER_JSON_VALUE_CAPACITY * 8)
#endif
#endif
#if NAWSERVER_JSON_INPUT_BYTES < 1 || NAWSERVER_JSON_INPUT_BYTES > 65535 || NAWSERVER_JSON_STRING_BYTES < 1 || \
	NAWSERVER_JSON_STRING_BYTES > 65535
#error "NaWServer JSON byte limits must be in 1..65535"
#endif
#if NAWSERVER_JSON_CONTAINER_ITEMS < 1 || NAWSERVER_JSON_CONTAINER_ITEMS > 65535 || NAWSERVER_JSON_VALUE_CAPACITY < 1 || \
	NAWSERVER_JSON_VALUE_CAPACITY > 65535
#error "NaWServer JSON item/value limits must be in 1..65535"
#endif
#if NAWSERVER_JSON_DEPTH < 1 || NAWSERVER_JSON_DEPTH > 32 || NAWSERVER_JSON_WORKSPACE_BYTES < 1
#error "NaWServer JSON depth must be in 1..32 and workspace bytes must be positive"
#endif
