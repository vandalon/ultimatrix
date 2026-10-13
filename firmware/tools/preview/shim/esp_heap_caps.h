#pragma once
#include <stddef.h>

#define MALLOC_CAP_8BIT 1
inline size_t heap_caps_get_free_size(unsigned) { return 256 * 1024; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return 256 * 1024; }
