// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once

// Reject obsolete settings rather than silently compiling a disabled feature.
// These spellings are diagnostics only; no compatibility aliases are provided.
#if defined(NaWServer_MCP) || defined(NaWServer_MCP_OUTPUT_BYTES) || defined(NaWServer_MCP_CJSON) || \
    defined(NaWServer_JSON_BOUNDED) || defined(NaWServer_JSON_YYJSON) || defined(NaWServer_JSON_RAPIDJSON) || \
    defined(NaWServer_JSON_INPUT_BYTES) || defined(NaWServer_JSON_STRING_BYTES) || \
    defined(NaWServer_JSON_CONTAINER_ITEMS) || defined(NaWServer_JSON_DEPTH) || \
    defined(NaWServer_JSON_VALUE_CAPACITY) || defined(NaWServer_JSON_WORKSPACE_BYTES) || \
    defined(NaWServer_BENCH_EXISTING_YYJSON) || defined(NaWServer_BENCH_EXISTING_RAPIDJSON)
#error "Use the uppercase NAWSERVER_ configuration prefix; obsolete mixed-case flags are not supported"
#endif

#if defined(SHOWCASE_LED_GPIO) || defined(LCD7_WIFI_SSID) || defined(LCD7_WIFI_PASSWORD) || \
    defined(NAWSERVER_WIFI_SSID) || defined(NAWSERVER_WIFI_PASSWORD)
#error "Use the NAWSERVER_DEMO_ configuration prefix for demo settings"
#endif
