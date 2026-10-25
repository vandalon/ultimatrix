// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "PlaySocket.h"
#include "AdminPin.h"
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

// Same handshake and framing as the WLED socket (lib/wled/WledWs.cpp): the connection is taken over from the web
// server, frames are read in the main loop, nothing waits. The frames here are tiny, so the buffer is fixed.
namespace playsocket {
namespace {
constexpr size_t kBuffer = 64;   // the largest frame: "t" and a 32 character token, plus the header and mask
WebServer *srv = nullptr;
void (*steer)(int) = nullptr;
WiFiClient client;
bool open = false, authed = false;
uint8_t buf[kBuffer];
size_t have = 0;

void handshake() {
  const String key = srv->header("Sec-WebSocket-Key");
  if (key.isEmpty()) { srv->send(400, "text/plain", "WebSocket only"); return; }
  const String origin = srv->header("Origin");   // a page on another site must not steer the game
  if (origin.length()) {
    const int scheme = origin.indexOf("://");
    if (scheme < 0 || !origin.substring(scheme + 3).equalsIgnoreCase(srv->hostHeader())) { srv->send(403, "text/plain", "Cross-site request refused."); return; }
  }
  const String text = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20];
  mbedtls_sha1((const uint8_t *)text.c_str(), text.length(), digest);
  unsigned char accept[32];
  size_t length = 0;
  mbedtls_base64_encode(accept, sizeof(accept), &length, digest, sizeof(digest));
  close();
  client = srv->client();
  client.print("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ");
  client.write(accept, length);
  client.print("\r\n\r\n");
  client.setNoDelay(true);
  open = true;
  authed = !adminpin::isSet();
  have = 0;
}

void sendClose() {
  const uint8_t frame[2] = {0x88, 0};
  client.write(frame, 2);
}

void readFrames() {
  int n = client.available();
  if (n > 0) {
    if ((size_t)n > kBuffer - have) n = kBuffer - have;
    if (n > 0) { const int got = client.read(buf + have, n); if (got > 0) have += got; }
  }
  for (;;) {
    if (have < 2) return;
    const uint8_t opcode = buf[0] & 0x0F;
    const bool masked = buf[1] & 0x80;
    const size_t length = buf[1] & 0x7F;
    if (length >= 126 || !masked || length + 6 > kBuffer) { close(); return; }   // browsers always mask; only small frames are ours
    if (have < 6 + length) return;
    uint8_t *mask = buf + 2, *payload = buf + 6;
    char text[40];
    const size_t size = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
    for (size_t i = 0; i < size; ++i) text[i] = payload[i] ^ mask[i & 3];
    text[size] = 0;
    if (opcode == 1) {
      if (!authed) {
        if (text[0] == 't' && adminpin::tokenOk(String(text + 1))) authed = true;
        else { sendClose(); close(); return; }
      } else if (text[0] != 't' && steer) {
        steer(atoi(text));
      }
    } else if (opcode == 8) { sendClose(); close(); return; }
    else if (opcode == 9) {   // ping: answer with the same bytes
      uint8_t pong[2 + 4] = {0x8A, (uint8_t)size};
      for (size_t i = 0; i < size && i < 4; ++i) pong[2 + i] = text[i];
      client.write(pong, 2 + (size < 4 ? size : 4));
      pong[1] = 0;
    }
    const size_t used = 6 + length;
    memmove(buf, buf + used, have - used);
    have -= used;
  }
}
}  // namespace

void attach(WebServer &server, void (*onSteer)(int dir)) {
  srv = &server;
  steer = onSteer;
  server.on("/play", HTTP_GET, handshake);   // main.cpp collects the Origin and Sec-WebSocket-Key headers
}

void close() {
  if (open) client.stop();
  open = false;
  have = 0;
}

void service() {
  if (!open) return;
  if (!client.connected()) { close(); return; }
  readFrames();
}

}  // namespace playsocket
