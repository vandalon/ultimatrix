// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace notice {
// Reader is bounded by the notification download's byte limit and deadline.
bool decodePng(size_t (*read)(void *, uint8_t *, size_t), void *context, uint16_t *pixels, bool fill);
}
