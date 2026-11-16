#pragma once
#include <stdint.h>
namespace tlspool {
// Takes the block for big TLS records once; call early, while the heap is still in one piece
void reserve();
// Whether the block exists, how many big requests it served, how many went to the heap, the biggest request seen
int state(uint32_t *hits, uint32_t *misses, uint32_t *biggest);
}
