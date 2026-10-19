// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once
#include <WebServer.h>
#include <stdint.h>

// The web pages are gzip-compressed at build time (tools/gzip_asset.py) and embedded as is: the browser unpacks them,
// so the clock spends no CPU or heap on it and the flash image is about 130 KB smaller.
#define UM_GZIP_ASSET(var, base) \
  extern const uint8_t var[] asm("_binary_" base "_gz_start"); \
  extern const uint8_t var##End[] asm("_binary_" base "_gz_end");

template <typename Server>
inline void sendGzip(Server &server, const char *type, const uint8_t *start, const uint8_t *end) {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Vary", "Accept-Encoding");
  server.send_P(200, type, reinterpret_cast<const char *>(start), end - start);
}
