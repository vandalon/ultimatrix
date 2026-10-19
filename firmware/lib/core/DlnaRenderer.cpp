// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "DlnaRenderer.h"
#include "DlnaDescriptions.h"
#include "NoticeImage.h"
#include "NetworkBudget.h"
#include "Console.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <new>

namespace dlna {
namespace {
WebServer *srv = nullptr;
const IPAddress kGroup(239, 255, 255, 250);
constexpr char kRenderer[] = "urn:schemas-upnp-org:device:MediaRenderer:1";
constexpr char kAv[] = "urn:schemas-upnp-org:service:AVTransport:1";
constexpr char kCm[] = "urn:schemas-upnp-org:service:ConnectionManager:1";
constexpr char kRc[] = "urn:schemas-upnp-org:service:RenderingControl:1";
constexpr char kProtocols[] = "http-get:*:image/bmp:*,http-get:*:image/x-ms-bmp:*,http-get:*:image/jpeg:*,http-get:*:image/png:*";
// Allocated in attach(): keeps ~0.4 KB out of the static RAM budget of faces that sit close to it.
struct State {
  String uuid, uri, metadata;
  std::shared_ptr<notice::Image> image;
  WiFiUDP ssdp;
  bool listening = false, playing = false, failed = false, painted = false;
  uint32_t loadAt = 0, announceAt = 0, nextPacketAt = 0, retryAt = 0;
  IPAddress boundIp, replyIp;
  uint16_t replyPort = 0;
  int announceIndex = -1, replyIndex = -1, replyOnly = -1;
};
State *st = nullptr;

String escape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("\"", "&quot;"); s.replace("'", "&apos;"); return s;
}
// SOAP arguments contain escaped text, including signed HA URLs with '&'.
bool unescape(const String &s, String &out) {
  out = "";
  if (!out.reserve(s.length())) return false;
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] != '&') { out += s[i]; continue; }
    int end = s.indexOf(';', i + 1);
    if (end < 0 || end - (int)i > 12) return false;
    String entity = s.substring(i + 1, end);
    uint32_t cp = 0;
    if (entity == "amp") cp = '&'; else if (entity == "lt") cp = '<';
    else if (entity == "gt") cp = '>'; else if (entity == "quot") cp = '"';
    else if (entity == "apos") cp = '\'';
    else if (entity.startsWith("#")) {
      char *tail;
      const bool hex = entity.startsWith("#x");
      const char *first = entity.c_str() + (hex ? 2 : 1);
      cp = strtoul(first, &tail, hex ? 16 : 10);
      if (tail == first || *tail) return false;
    } else return false;
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
    else { out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
    i = end;
  }
  return true;
}
// Match XML local names while retaining the qualified name for the closing tag.
// No DTDs, external entities, or nested XML in argument values are interpreted.
bool element(const String &xml, const char *name, String &value) {
  int at = 0;
  while ((at = xml.indexOf('<', at)) >= 0) {
    int end = xml.indexOf('>', at + 1);
    if (end < 0) return false;
    int space = at + 1;
    while (space < end && xml[space] != ' ' && xml[space] != '\t' && xml[space] != '\r' && xml[space] != '\n' && xml[space] != '/') space++;
    String qualified = xml.substring(at + 1, space);
    int colon = qualified.indexOf(':');
    if (qualified.substring(colon + 1) == name) {
      if (xml[end - 1] == '/') { value = ""; return true; }
      int close = xml.indexOf("</" + qualified + ">", end + 1);
      if (close < 0) return false;
      value = xml.substring(end + 1, close); return true;
    }
    at = end + 1;
  }
  return false;
}
String tag(const char *name, const String &value) { return String("<") + name + ">" + escape(value) + "</" + name + ">"; }
void xml(const String &body) { srv->send(200, "text/xml; charset=\"utf-8\"", body); }
void fault(int code, const char *description) {
  srv->send(500, "text/xml; charset=\"utf-8\"", String("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">") + tag("errorCode", String(code)) + tag("errorDescription", description) + "</UPnPError></detail></s:Fault></s:Body></s:Envelope>");
}
const char *transport() {
  if (st->uri.isEmpty()) return "NO_MEDIA_PRESENT";
  if (!st->playing) return "STOPPED";
  return st->image && st->image->status == notice::Image::READY ? "PLAYING" : "TRANSITIONING";
}
void stop() { st->playing = false; st->image.reset(); st->painted = false; notice::holdFeeds(notice::HOLD_DLNA, false); }
void control(bool connectionManager) {
  String body = srv->arg("plain"), args, action;
  if (body.isEmpty() || body.length() > 8192 || body.indexOf("<!") >= 0) { fault(402, "Invalid Args"); return; }
  const char *const avActions[] = {"SetAVTransportURI", "Play", "Stop", "GetTransportInfo", "GetPositionInfo", "GetMediaInfo", "GetDeviceCapabilities", "GetTransportSettings", "GetCurrentTransportActions"};
  const char *const cmActions[] = {"GetProtocolInfo", "GetCurrentConnectionIDs", "GetCurrentConnectionInfo"};
  const char *const *actions = connectionManager ? cmActions : avActions;
  const size_t count = connectionManager ? 3 : 9;
  for (size_t i = 0; i < count; i++) if (element(body, actions[i], args)) { action = actions[i]; break; }
  if (action.isEmpty()) { fault(401, "Invalid Action"); return; }
  String instance;
  if (!connectionManager && (!element(args, "InstanceID", instance) || instance != "0")) { fault(718, "Invalid InstanceID"); return; }
  String result;
  if (action == "SetAVTransportURI") {
    String encoded, next, encodedMeta, nextMeta;
    if (!element(args, "CurrentURI", encoded) || !unescape(encoded, next) || next.length() > notice::kDownloadUrlMax ||
        (!next.isEmpty() && !next.startsWith("http://") && !next.startsWith("https://")) ||
        !element(args, "CurrentURIMetaData", encodedMeta) || !unescape(encodedMeta, nextMeta) || nextMeta.length() > 4096) {
      fault(714, "Illegal MIME-Type or URI"); return;
    }
    for (size_t i = 0; i < next.length(); i++) if ((uint8_t)next[i] <= 32 || (uint8_t)next[i] == 127) { fault(402, "Invalid Args"); return; }
    stop(); st->uri = next; st->metadata = nextMeta; st->failed = false;
  } else if (action == "Play") {
    String speed;
    if (!element(args, "Speed", speed) || speed != "1") { fault(717, "Play speed not supported"); return; }
    if (st->uri.isEmpty()) { fault(701, "Transition not available"); return; }
    if (!st->playing) { st->playing = true; st->failed = false; st->loadAt = millis(); st->painted = false; }
  } else if (action == "Stop") { stop(); st->failed = false; }
  else if (action == "GetTransportInfo") result = tag("CurrentTransportState", transport()) + tag("CurrentTransportStatus", st->failed ? "ERROR_OCCURRED" : "OK") + tag("CurrentSpeed", "1");
  else if (action == "GetPositionInfo") result = tag("Track", st->uri.isEmpty() ? "0" : "1") + tag("TrackDuration", "00:00:00") + tag("TrackMetaData", st->metadata) + tag("TrackURI", st->uri) + tag("RelTime", "00:00:00") + tag("AbsTime", "00:00:00") + tag("RelCount", "-1") + tag("AbsCount", "-1");
  else if (action == "GetMediaInfo") result = tag("NrTracks", st->uri.isEmpty() ? "0" : "1") + tag("MediaDuration", "00:00:00") + tag("CurrentURI", st->uri) + tag("CurrentURIMetaData", st->metadata) + tag("NextURI", "") + tag("NextURIMetaData", "") + tag("PlayMedium", "NETWORK") + tag("RecordMedium", "NOT_IMPLEMENTED") + tag("WriteStatus", "NOT_IMPLEMENTED");
  else if (action == "GetDeviceCapabilities") result = tag("PlayMedia", "NETWORK") + tag("RecMedia", "NOT_IMPLEMENTED") + tag("RecQualityModes", "NOT_IMPLEMENTED");
  else if (action == "GetTransportSettings") result = tag("PlayMode", "NORMAL") + tag("RecQualityMode", "NOT_IMPLEMENTED");
  else if (action == "GetCurrentTransportActions") result = tag("Actions", st->uri.isEmpty() ? "" : st->playing ? "Stop" : "Play,Stop");
  else if (action == "GetProtocolInfo") result = tag("Source", "") + tag("Sink", kProtocols);
  else if (action == "GetCurrentConnectionIDs") result = tag("ConnectionIDs", "0");
  else if (action == "GetCurrentConnectionInfo") {
    if (!element(args, "ConnectionID", instance) || instance != "0") { fault(706, "Invalid connection reference"); return; }
    result = tag("RcsID", "0") + tag("AVTransportID", "0") + tag("ProtocolInfo", "http-get:*:image/bmp:*") + tag("PeerConnectionManager", "") + tag("PeerConnectionID", "-1") + tag("Direction", "Input") + tag("Status", "OK");
  }
  const char *service = connectionManager ? kCm : kAv;
  xml(String("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:") + action + "Response xmlns:u=\"" + service + "\">" + result + "</u:" + action + "Response></s:Body></s:Envelope>");
}
void description() {
  String s = "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><device>";
  s += tag("deviceType", kRenderer) + tag("friendlyName", "Ultimatrix screen") + tag("manufacturer", "Ultimatrix") + tag("modelName", "Ultimatrix LED Matrix Clock") + tag("UDN", st->uuid);
  s += "<serviceList>";
  const char *names[] = {"AVTransport", "ConnectionManager", "RenderingControl"};
  const char *paths[] = {"av", "cm", "rc"};
  for (int i = 0; i < 3; i++) s += String("<service>") + tag("serviceType", String("urn:schemas-upnp-org:service:") + names[i] + ":1") + tag("serviceId", String("urn:upnp-org:serviceId:") + names[i]) + tag("SCPDURL", String("/dlna/") + paths[i] + ".xml") + tag("controlURL", String("/dlna/") + paths[i] + "/control") + tag("eventSubURL", String("/dlna/") + paths[i] + "/event") + "</service>";
  s += "</serviceList><presentationURL>/canvas</presentationURL></device></root>";
  xml(s);
}
String target(int index) {
  switch (index) {
    case 0: return "upnp:rootdevice"; case 1: return st->uuid; case 2: return kRenderer;
    case 3: return kAv; case 4: return kCm; default: return kRc;
  }
}
void packet(int index, bool reply, IPAddress ip, uint16_t port) {
  const String type = target(index), usn = index == 1 ? st->uuid : st->uuid + "::" + type;
  String message = reply ? "HTTP/1.1 200 OK\r\nEXT:\r\n" : "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNTS: ssdp:alive\r\n";
  message += String(reply ? "ST: " : "NT: ") + type + "\r\nUSN: " + usn + "\r\nLOCATION: http://" + WiFi.localIP().toString() + "/dlna/device.xml\r\nCACHE-CONTROL: max-age=900\r\nSERVER: ESP32/1.0 UPnP/1.0 Ultimatrix/1.0\r\n\r\n";
  const bool multicast = ip == kGroup;
  if (multicast ? st->ssdp.beginMulticastPacket() : st->ssdp.beginPacket(ip, port)) {
    st->ssdp.print(message);
    if (!st->ssdp.endPacket()) consoleLog("[DLNA] SSDP send failed (%s)\n", multicast ? "notify" : "reply");
  } else consoleLog("[DLNA] SSDP send could not start (%s)\n", multicast ? "notify" : "reply");
}
String header(const String &message, const char *key) {
  int at = message.indexOf('\n') + 1;
  while (at > 0 && at < (int)message.length()) {
    int end = message.indexOf('\n', at);
    if (end < 0) break;
    int colon = message.indexOf(':', at);
    if (colon >= at && colon < end) {
      String name = message.substring(at, colon); name.trim();
      if (name.equalsIgnoreCase(key)) { String value = message.substring(colon + 1, end); value.trim(); return value; }
    }
    at = end + 1;
  }
  return "";
}
} // namespace
void attach(WebServer &server, void (*postRoute)(const char *, WebServer::THandlerFunction)) {
  st = new (std::nothrow) State;
  if (!st) return;
  srv = &server;
  char id[48];
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(id, sizeof(id), "uuid:756c7469-6d61-7472-6978-%012llx", (unsigned long long)(mac & 0xffffffffffffULL));
  st->uuid = id;
  server.on("/dlna/device.xml", HTTP_GET, description);
  server.on("/dlna/av.xml", HTTP_GET, [] { srv->send_P(200, "text/xml", kAvScpd); });
  server.on("/dlna/cm.xml", HTTP_GET, [] { srv->send_P(200, "text/xml", kCmScpd); });
  server.on("/dlna/rc.xml", HTTP_GET, [] { srv->send_P(200, "text/xml", kRcScpd); });
  postRoute("/dlna/av/control", [] { control(false); });
  postRoute("/dlna/cm/control", [] { control(true); });
  postRoute("/dlna/rc/control", [] { fault(401, "Invalid Action"); });
  // HA explicitly falls back to polling on 412; no callback sockets or event task.
  for (const char *path : {"/dlna/av/event", "/dlna/cm/event", "/dlna/rc/event"})
    server.on(path, HTTP_ANY, [] { srv->send(412, "text/plain", "Use polling"); });
}
void poll(uint32_t now, bool ready) {
  if (!srv) return;
  if (!ready || firmwareUpdating) {
    notice::holdFeeds(notice::HOLD_DLNA, false);
    if (st->listening) { st->ssdp.stop(); st->listening = false; }
    if (firmwareUpdating) { stop(); st->uri = ""; st->metadata = ""; }
    return;
  }
  if (st->listening && st->boundIp != WiFi.localIP()) { st->ssdp.stop(); st->listening = false; }
  if (!st->listening && (int32_t)(now - st->retryAt) >= 0) {
    st->retryAt = now + 10000;
    st->listening = st->ssdp.beginMulticast(kGroup, 1900);
    consoleLog("[DLNA] SSDP multicast listener %s\n", st->listening ? "open" : "failed to open");
    if (st->listening) { st->boundIp = WiFi.localIP(); st->announceIndex = 0; st->replyIndex = -1; st->nextPacketAt = now; }
  }
  if (st->listening) {
    if (st->announceIndex < 0 && (int32_t)(now - st->announceAt) >= 0) st->announceIndex = 0;
    if ((int32_t)(now - st->nextPacketAt) >= 0) {
      if (st->replyIndex >= 0) {
        packet(st->replyIndex, true, st->replyIp, st->replyPort);
        if (st->replyOnly >= 0 || ++st->replyIndex >= 6) st->replyIndex = -1;
        st->nextPacketAt = now + 50;
      } else if (st->announceIndex >= 0) {
        packet(st->announceIndex, false, kGroup, 1900);
        if (++st->announceIndex >= 6) { st->announceIndex = -1; st->announceAt = now + 300000; }
        st->nextPacketAt = now + 50;
      }
    }
    const int size = st->ssdp.parsePacket();
    if (size > 0) {
      char buffer[1025];
      const IPAddress remote = st->ssdp.remoteIP(); const uint16_t port = st->ssdp.remotePort();
      const int count = size <= 1024 ? st->ssdp.read(buffer, size) : 0;
      st->ssdp.clear(); buffer[count > 0 ? count : 0] = 0;
      String request(buffer);
      if (st->replyIndex < 0 && request.startsWith("M-SEARCH * HTTP/1.1") && header(request, "MAN").equalsIgnoreCase("\"ssdp:discover\"")) {
        const String search = header(request, "ST");
        consoleLog("[DLNA] M-SEARCH from %s for %s\n", remote.toString().c_str(), search.c_str());
        int match = -1;
        for (int i = 0; i < 6; i++) if (search == target(i)) { match = i; break; }
        if (search == "ssdp:all" || match >= 0) {
          st->replyIp = remote; st->replyPort = port; st->replyOnly = match; st->replyIndex = match < 0 ? 0 : match;
          const int mx = constrain(header(request, "MX").toInt(), 1L, 3L);
          st->nextPacketAt = now + esp_random() % (mx * 1000);
        }
      }
    }
  }
  if (st->playing) {
    notice::holdFeeds(notice::HOLD_DLNA, true);  // feeds park until Stop: the picture needs the heap to load, and keeps it
    if (!st->image && now - st->loadAt < 30000) st->image = notice::loadImage(st->uri.c_str(), false);
    if ((st->image && st->image->status == notice::Image::FAILED) || (!st->image && now - st->loadAt >= 30000)) {
      stop(); st->failed = true;
      consoleLog("[DLNA] Picture could not be loaded\n"); // signed URLs are never logged
    }
  }
}
bool active() { return st && st->playing && st->image && st->image->status == notice::Image::READY; }
void hidden() { if (st) st->painted = false; }
void draw(MatrixPanel_I2S_DMA &gfx) {
  if (!active() || st->painted) return;
  int lit = 0;
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { const uint16_t c = st->image->pixels[y * 64 + x]; lit += c != 0; gfx.drawPixel(x, y, c); }
  consoleLog("[DLNA] Picture drawn, %d of 4096 pixels not black\n", lit);
  gfx.flipDMABuffer(); st->painted = true;
}
} // namespace dlna
