// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <arpa/inet.h>
#define lwip_inet_pton inet_pton
#define lwip_htons htons
#define lwip_ntohs ntohs
#define lwip_ntohl ntohl
