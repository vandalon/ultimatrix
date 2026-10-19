// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <WebServer.h>
class MatrixPanel_I2S_DMA;
namespace dlna {
void attach(WebServer &server, void (*postRoute)(const char *, WebServer::THandlerFunction));
void poll(uint32_t now, bool ready);
bool active();
void draw(MatrixPanel_I2S_DMA &gfx);
void hidden();
}
