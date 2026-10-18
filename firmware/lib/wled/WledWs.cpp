// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "Wled.h"
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <memory>

// The WLED app only works over a WebSocket at /ws. One client at a time, text frames only, taken over from the
// web server's connection (the server drops its own reference when the handler returns). The buffer exists while
// a client is connected. Everything runs in the main loop; nothing waits.
namespace wled {
namespace {
constexpr size_t kBuffer = 1024;      // the largest frame accepted (commands are small)
constexpr uint32_t kSendMinMs = 100;  // state changes are sent at most this often
WebServer *srv = nullptr;
WiFiClient client;
bool open = false;
std::unique_ptr<uint8_t[]> buf;
size_t have = 0;
uint32_t sentStamp = 0, sentAt = 0;

void sendFrame(uint8_t opcode, const uint8_t *data, size_t length) {
  uint8_t head[4];
  size_t n = 0;
  head[n++] = 0x80 | opcode;
  if (length < 126) head[n++] = (uint8_t)length;
  else { head[n++] = 126; head[n++] = length >> 8; head[n++] = length & 0xFF; }
  client.write(head, n);
  if (length) client.write(data, length);
}

void handshake() {
  const String key = srv->header("Sec-WebSocket-Key");
  if (key.isEmpty()) { srv->send(400, "text/plain", "WebSocket only"); return; }
  const String text = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20];
  mbedtls_sha1((const uint8_t *)text.c_str(), text.length(), digest);
  unsigned char accept[32];
  size_t length = 0;
  mbedtls_base64_encode(accept, sizeof(accept), &length, digest, sizeof(digest));
  wsClose();
  buf.reset(new (std::nothrow) uint8_t[kBuffer]);
  if (!buf) { srv->send(503, "text/plain", "No memory"); return; }
  client = srv->client();
  client.print("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ");
  client.write(accept, length);
  client.print("\r\n\r\n");
  client.setNoDelay(true);
  open = true;
  have = 0;
  sentStamp = 0;  // the state goes out on the next service()
}

void sendState() {
  String json;
  json.reserve(1400);
  apiStateInfo(json);
  sendFrame(1, (const uint8_t *)json.c_str(), json.length());
  sentStamp = changeStamp();
  sentAt = millis();
}

// Reads whatever has arrived and handles complete frames
void readFrames() {
  int n = client.available();
  if (n > 0) {
    if ((size_t)n > kBuffer - have) n = kBuffer - have;
    if (n > 0) have += client.read(buf.get() + have, n);
  }
  for (;;) {
    if (have < 2) return;
    const uint8_t opcode = buf[0] & 0x0F;
    const bool masked = buf[1] & 0x80;
    size_t length = buf[1] & 0x7F, at = 2;
    if (length == 126) { if (have < 4) return; length = (size_t)buf[2] << 8 | buf[3]; at = 4; }
    else if (length == 127) { wsClose(); return; }
    if (!masked || length + at + 4 >= kBuffer) { wsClose(); return; }  // browsers and the app always mask; bigger frames are not ours
    if (have < at + 4 + length) return;
    uint8_t *mask = buf.get() + at, *payload = mask + 4;
    for (size_t i = 0; i < length; ++i) payload[i] ^= mask[i & 3];
    if (opcode == 1) {
      const uint8_t after = payload[length];  // the end of the text goes where the next frame may start: put that byte back afterwards
      payload[length] = 0;
      apiApplyText((const char *)payload);
      payload[length] = after;
      sentStamp = 0;  // answer with the state
    } else if (opcode == 8) { sendFrame(8, nullptr, 0); wsClose(); return; }
    else if (opcode == 9) sendFrame(10, payload, length);
    const size_t used = at + 4 + length;
    memmove(buf.get(), buf.get() + used, have - used);
    have -= used;
  }
}
}  // namespace

void wsAttach(WebServer &server) {
  srv = &server;
  static const char *keys[] = {"Sec-WebSocket-Key", "User-Agent"};
  server.collectHeaders(keys, 2);
  server.on("/ws", HTTP_GET, handshake);
}

void wsClose() {
  if (open) client.stop();
  open = false;
  buf.reset();
  have = 0;
}

void wsService(uint32_t now) {
  if (!open) return;
  if (!client.connected()) { wsClose(); return; }
  readFrames();
  if (open && (sentStamp != changeStamp() || (int32_t)(now - sentAt) > 20000) && (int32_t)(now - sentAt) >= (int32_t)kSendMinMs) sendState();
}

}  // namespace wled
