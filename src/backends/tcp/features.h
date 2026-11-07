// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

// Private source selection only. Public HTTP headers remain SDK-independent.
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if defined(CONFIG_NAWSERVER_TCP_MANAGED) && CONFIG_NAWSERVER_TCP_MANAGED
#define NAWSERVER_TCP_MANAGED 1
#else
#define NAWSERVER_TCP_MANAGED 0
#endif
#if defined(CONFIG_NAWSERVER_WEBSOCKET) && CONFIG_NAWSERVER_WEBSOCKET
#define NAWSERVER_TCP_WEBSOCKET 1
#else
#define NAWSERVER_TCP_WEBSOCKET 0
#endif
#if defined(CONFIG_NAWSERVER_SSE) && CONFIG_NAWSERVER_SSE
#define NAWSERVER_TCP_SSE 1
#else
#define NAWSERVER_TCP_SSE 0
#endif
#else
// Host suites retain their full default corpus. Focused host builds may
// override a private flag explicitly without defining SDK configuration.
#ifndef NAWSERVER_TCP_MANAGED
#define NAWSERVER_TCP_MANAGED 1
#endif
#ifndef NAWSERVER_TCP_WEBSOCKET
#define NAWSERVER_TCP_WEBSOCKET 1
#endif
#ifndef NAWSERVER_TCP_SSE
#define NAWSERVER_TCP_SSE 1
#endif
#endif
