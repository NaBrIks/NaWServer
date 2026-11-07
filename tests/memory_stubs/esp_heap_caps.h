// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstddef>
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_SPIRAM 2U
#define MALLOC_CAP_8BIT 4U
void *heap_caps_malloc(std::size_t, unsigned);
void *heap_caps_malloc_prefer(std::size_t, std::size_t, ...);
void heap_caps_free(void *);
std::size_t heap_caps_get_free_size(unsigned);
std::size_t heap_caps_get_largest_free_block(unsigned);
