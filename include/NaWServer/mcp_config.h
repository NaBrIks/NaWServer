// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include "detail/configuration_names.h"

// A JSON backend selection never enables MCP. -DNAWSERVER_MCP is value 1.
#ifndef NAWSERVER_MCP
#define NAWSERVER_MCP 0
#endif
#if NAWSERVER_MCP != 0 && NAWSERVER_MCP != 1
#error "NAWSERVER_MCP must have value 0 or 1"
#endif
#if NAWSERVER_MCP && (NAWSERVER_JSON_BOUNDED + NAWSERVER_JSON_YYJSON + NAWSERVER_JSON_RAPIDJSON != 1)
#error \
	"NAWSERVER_MCP requires exactly one JSON backend: NAWSERVER_JSON_BOUNDED, NAWSERVER_JSON_YYJSON or NAWSERVER_JSON_RAPIDJSON"
#endif
#if NAWSERVER_MCP
#include "json_config.h"
#endif
#if NAWSERVER_MCP && defined(NAWSERVER_MCP_CJSON) && NAWSERVER_MCP_CJSON
#error "cJSON does not implement the NaWServer exact-integer JSON contract"
#endif

// Application-owned complete MCP response buffer, reserved once per endpoint.
// Increasing this value reserves the same additional bytes in demo RAM/PSRAM;
// it does not change the HTTP engine body/header workspace or grow at runtime.
#ifndef NAWSERVER_MCP_OUTPUT_BYTES
#define NAWSERVER_MCP_OUTPUT_BYTES 2048
#endif
#if NAWSERVER_MCP_OUTPUT_BYTES < 256 || NAWSERVER_MCP_OUTPUT_BYTES > 65535
#error "NAWSERVER_MCP_OUTPUT_BYTES must be between 256 and 65535"
#endif
