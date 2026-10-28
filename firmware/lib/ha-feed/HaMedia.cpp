// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "HaMedia.h"

#include <Arduino.h>
#include <atomic>
#include <stdarg.h>
#include <memory>
#include <mutex>
#include <new>
#include <string.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <rom/tjpgd.h>
#include "Console.h"

#include <NetworkBudget.h>
#include <Settings.h>
#include <TimeService.h>

namespace ha {
namespace media {

namespace {

std::mutex lock;
Snapshot current{};
bool haveSnapshot = false;
std::atomic<uint32_t> changes{0};

// ---- Radio station names: read from the stream's own headers when Home Assistant gives none ----
char icyFor[kStream] = "", icyFailedFor[kStream] = "", icyName[kText] = "";
bool icyKnown = false;
uint32_t icyFailedAt = 0;
std::atomic<bool> icyFetching{false};

// What the Now playing page works from: the station filled in, a player with nothing to say (a radio ad break) left out
void resolve(Snapshot &s) {
  int kept = 0;
  for (int i = 0; i < s.count; i++) {
    Player p = s.player[i];
    if (p.needsStation && icyKnown && !strcmp(icyFor, s.stream) && icyName[0]) { strlcpy(p.station, icyName, sizeof(p.station)); p.needsStation = 0; }
    if (!p.title[0] && !p.artist[0] && p.station[0]) strlcpy(p.title, p.station, sizeof(p.title));
    if (!p.title[0] && !p.artist[0] && !p.picture[0]) continue;
    s.player[kept++] = p;
  }
  for (int i = kept; i < s.count; i++) memset(&s.player[i], 0, sizeof(Player));
  s.count = kept;
}

// ---- Album art ----
constexpr uint32_t kRetryMs = 60000;
constexpr uint32_t kLookupRetryMs = 300000;  // a track nobody has a cover for stays that way
constexpr size_t kWorkArea = 4096;  // the decoder's own memory (it needs about 3.1 KB)
std::atomic<bool> fetching{false};
char haveFor[kPicture] = "";      // the picture the ready pixels belong to
char failedFor[kPicture] = "";
uint32_t failedAt = 0;
uint16_t *ready = nullptr;
uint16_t coverId = 0;             // changes with every cover that becomes ready        // kArt*kArt RGB565, written by the fetch task, read under the lock
char wanted[kPicture] = "";
char wantedStation[kText] = "";  // the station of the wanted cover, when looking online is allowed: the logo is the last fallback
char wantedQuery[kText * 2 + 2] = "";  // "artist|title" of the wanted cover when looking online is allowed: the fallback when its picture fails
char note[48] = "No picture yet";  // the last step of the art fetch, for the settings page

void setNote(const char *format, ...) __attribute__((format(printf, 1, 2)));
void setNote(const char *format, ...) {
  va_list args;
  va_start(args, format);
  char text[sizeof(note)];
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  bool changed;
  {
    std::lock_guard<std::mutex> guard(lock);
    changed = strcmp(note, text) != 0;
    strlcpy(note, text, sizeof(note));
  }
  if (changed) consoleLog("[HA] album art: %s\n", text);  // the art line of the settings page, also in the remote console
}

// A character of the page: the 3x5/4x5 fonts only have plain ASCII
char fold(uint32_t cp) {
  static const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";  // U+00C0..U+00FF
  if (cp < 0x20) return ' ';
  if (cp < 0x7f) return (char)cp;
  if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0];
  if (cp == 0x2018 || cp == 0x2019) return '\'';
  if (cp == 0x201C || cp == 0x201D) return '"';
  if (cp == 0x2013 || cp == 0x2014) return '-';
  return '?';
}

void copyField(char *to, size_t size, const char *from, size_t length) {
  if (length >= size) length = size - 1;
  memcpy(to, from, length);
  to[length] = 0;
}

struct Fetch {
  Client *stream;
  uint16_t *out;               // kArt*kArt RGB565
  uint8_t *column, *row;       // per source column/row of the decoded image: destination index + 1 when that one is the sample, else 0
  int width, height;           // of the decoded (scaled) image
};

// Up to size bytes from the connection: waits for data in flight, and only gives up when it ends or goes quiet for 5 s
size_t readSome(Client *stream, uint8_t *buffer, size_t size) {
  size_t got = 0;
  uint32_t last = millis();
  while (got < size) {
    const int ready = stream->available();
    if (ready > 0) {
      const int n = stream->read(buffer + got, std::min<size_t>(ready, size - got));
      if (n > 0) { got += n; last = millis(); continue; }
    } else if (!stream->connected() || millis() - last > 5000) break;
    delay(1);
  }
  return got;
}

UINT readJpeg(JDEC *jd, BYTE *buffer, UINT size) {
  Fetch *fetch = (Fetch *)jd->device;
  if (buffer) return readSome(fetch->stream, buffer, size);
  UINT skipped = 0;
  uint8_t scrap[64];
  while (skipped < size) {
    const size_t got = readSome(fetch->stream, scrap, std::min<UINT>(sizeof(scrap), size - skipped));
    if (!got) break;
    skipped += got;
  }
  return skipped;
}

UINT writeJpeg(JDEC *jd, void *bitmap, JRECT *rect) {
  Fetch *fetch = (Fetch *)jd->device;
  const uint8_t *rgb = (const uint8_t *)bitmap;
  for (int y = rect->top; y <= rect->bottom; y++) {
    const int dy = fetch->row[y];
    for (int x = rect->left; x <= rect->right; x++, rgb += 3) {
      const int dx = fetch->column[x];
      if (dx && dy) fetch->out[(dy - 1) * kArt + dx - 1] = (rgb[0] & 0xF8) << 8 | (rgb[1] & 0xFC) << 3 | rgb[2] >> 3;
    }
  }
  return 1;
}

bool decodeStream(HTTPClient &http, uint16_t *out) {  // the JPEG being received, sampled down to kArt x kArt, into out
  std::unique_ptr<Fetch> fetch(new (std::nothrow) Fetch{});
  std::unique_ptr<uint8_t[]> work(new (std::nothrow) uint8_t[kWorkArea]);
  std::unique_ptr<JDEC> jd(new (std::nothrow) JDEC);
  if (!fetch || !work || !jd) { setNote("Out of memory (%uk free, block %uk)", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024), (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024)); return false; }
  fetch->out = out;
  fetch->stream = http.getStreamPtr();
  const int prepared = jd_prepare(jd.get(), readJpeg, work.get(), kWorkArea, fetch.get());
  bool ok = prepared == JDR_OK;
  if (!ok) setNote("Not a baseline JPEG (code %d)", prepared);
  if (ok) consoleLog("[HA] album art: %ux%u JPEG\n", (unsigned)jd->width, (unsigned)jd->height);
  if (ok) {
    // The biggest reduction (1/1 to 1/8) that still leaves at least kArt pixels each way
    uint8_t scale = 0;
    while (scale < 3 && (jd->width >> (scale + 1)) >= (UINT)kArt && (jd->height >> (scale + 1)) >= (UINT)kArt) scale++;
    fetch->width = jd->width >> scale;
    fetch->height = jd->height >> scale;
    ok = fetch->width >= 8 && fetch->height >= 8;
    std::unique_ptr<uint8_t[]> maps(ok ? new (std::nothrow) uint8_t[fetch->width + fetch->height]() : nullptr);
    if (ok && !maps) { setNote("Out of memory (map)"); ok = false; }
    if (ok) {
      fetch->column = maps.get();
      fetch->row = maps.get() + fetch->width;
      // Each destination pixel takes the middle source pixel of its share of the picture
      for (int i = 0; i < kArt; i++) {
        fetch->column[std::min(fetch->width - 1, (i * fetch->width + fetch->width / 2) / kArt)] = i + 1;
        fetch->row[std::min(fetch->height - 1, (i * fetch->height + fetch->height / 2) / kArt)] = i + 1;
      }
      const int decoded = jd_decomp(jd.get(), writeJpeg, scale);
      ok = decoded == JDR_OK;
      if (!ok) setNote("JPEG decode failed (code %d, %ux%u)", decoded, (unsigned)jd->width, (unsigned)jd->height);
    } else setNote("Picture too small");
  }
  return ok;
}

bool download(const Config &c, const char *path, uint16_t *out) {  // a picture Home Assistant serves, with its token
  std::unique_ptr<WiFiClient> plain;
  std::unique_ptr<WiFiClientSecure> secure;
  std::unique_ptr<HTTPClient> http(new (std::nothrow) HTTPClient);
  std::unique_ptr<char[]> bearer(new (std::nothrow) char[kTokenMax + 8]);
  if (!http || !bearer) { setNote("Out of memory"); return false; }

  bool started;
  if (c.tls) {
    secure.reset(new (std::nothrow) WiFiClientSecure);
    if (!secure) return false;
    secure->setInsecure();
    started = http->begin(*secure, c.host, c.port, path, true);
  } else {
    plain.reset(new (std::nothrow) WiFiClient);
    if (!plain) return false;
    started = http->begin(*plain, c.host, c.port, path);
  }
  if (!started) { setNote("Cannot connect"); return false; }
  http->useHTTP10(true);  // no chunked answer: the stream is the picture itself
  http->setTimeout(4000);
  snprintf(bearer.get(), kTokenMax + 8, "Bearer %s", c.token);
  http->addHeader("Authorization", bearer.get());
  const int code = http->GET();
  if (code != 200) { http->end(); setNote(code > 0 ? "Picture request: HTTP %d" : "Picture request failed (%d)", code); return false; }
  const bool ok = decodeStream(*http, out);
  http->end();
  return ok;
}

// ---- Public sources: pictures outside Home Assistant, never sent the Home Assistant token ----

// GET url (http or https) and hand the open connection to use(); one connection at a time, closed on return
template <typename Use>
bool getPublic(const char *url, const char *what, Use use) {
  std::unique_ptr<WiFiClient> plain;
  std::unique_ptr<WiFiClientSecure> secure;
  std::unique_ptr<HTTPClient> http(new (std::nothrow) HTTPClient);
  if (!http) { setNote("Out of memory"); return false; }
  bool started;
  if (!strncmp(url, "https://", 8)) {
    secure.reset(new (std::nothrow) WiFiClientSecure);
    if (!secure) { setNote("Out of memory"); return false; }
    secure->setInsecure();  // public covers and search results, nothing private travels
    started = http->begin(*secure, String(url));
  } else {
    plain.reset(new (std::nothrow) WiFiClient);
    if (!plain) { setNote("Out of memory"); return false; }
    started = http->begin(*plain, String(url));
  }
  if (!started) { setNote("Cannot connect (%s)", what); return false; }
  http->useHTTP10(true);
  http->setTimeout(4000);
  http->setUserAgent("Ultimatrix");
  const int code = http->GET();
  if (code != 200) { http->end(); setNote(code > 0 ? "%s: HTTP %d" : "%s failed (%d)", what, code); return false; }
  const bool ok = use(*http);
  http->end();
  return ok;
}

bool downloadPublic(const char *url, uint16_t *out) {
  return getPublic(url, "Picture", [&](HTTPClient &http) { return decodeStream(http, out); });
}

// The covers of Deezer and Apple are served over plain http too: saves the TLS memory
void plainCoverUrl(char *url) {
  if (strncmp(url, "https://", 8) || (!strstr(url, "dzcdn.net/") && !strstr(url, "mzstatic.com/"))) return;
  memmove(url + 4, url + 5, strlen(url + 5) + 1);  // "https://" -> "http://"
}

// Lower-case letters and digits only, so "Beyoncé & Jay-Z" and "beyonc jayz" compare alike
void normalise(const char *from, char *to, size_t size) {
  size_t n = 0;
  for (size_t i = 0; from[i] && n + 1 < size; i++) {
    const char c = from[i];
    if (c >= 'A' && c <= 'Z') to[n++] = c + 32;
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) to[n++] = c;
  }
  to[n] = 0;
}

// The first artist of "A, B", "A & B", "A feat. B", "A x B", "A; B": the length of that part
size_t firstArtistLength(const char *artist) {
  size_t n = strlen(artist);
  static const char *const kSeparators[] = {", ", " & ", " feat", " ft.", " x ", " X ", "; ", " / ", " and "};
  for (const char *separator : kSeparators) {
    const char *at = strstr(artist, separator);
    if (at && (size_t)(at - artist) > 0 && (size_t)(at - artist) < n) n = at - artist;
  }
  return n;
}

// "Song (feat. X)", "Song - Remastered 2011", "Song [Live]" -> "Song"
void cleanTitle(const char *title, char *out, size_t size) {
  strlcpy(out, title, size);
  static const char *const kCuts[] = {" (", " [", " - ", " / "};
  for (const char *cut : kCuts) {
    char *at = strstr(out, cut);
    if (at && at != out) *at = 0;
  }
}

void encode(const char *from, char *to, size_t size) {  // percent-encoding for a query value
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)from; *p && n + 4 < size; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') to[n++] = *p;
    else n += snprintf(to + n, size - n, "%%%02X", *p);
  }
  to[n] = 0;
}

// Text between key and the closing quote of the JSON string after it, unescaped; false when key is not in json
bool jsonString(const char *json, const char *key, char *out, size_t size) {
  const char *at = strstr(json, key);
  if (!at) return false;
  unescape(at + strlen(key), out, size);
  return out[0] != 0;
}

// iTunes Search: the first of a few matches that names the same (first) artist; its 100 px cover asked for at 200 px
bool findItunes(const char *artist, const char *title, char *buffer, size_t size, char *coverUrl, size_t coverSize) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 36000) { setNote("No memory for iTunes (TLS)"); return false; }  // iTunes only answers over https
  char encodedTerm[kText * 6 + 8], url[400];
  char cleaned[kText];
  cleanTitle(title, cleaned, sizeof(cleaned));
  char first[kText];
  strlcpy(first, artist, std::min(sizeof(first), firstArtistLength(artist) + 1));
  char term[kText * 2 + 2];
  snprintf(term, sizeof(term), "%s %s", cleaned, first);
  encode(term, encodedTerm, sizeof(encodedTerm));
  // The store of the clock's country: the default (US) store lacks many local artists
  char country[3] = "", storeArg[16] = "";
  if (ultime::countryForZone(Settings::getInstance()->timezone.c_str(), country)) snprintf(storeArg, sizeof(storeArg), "&country=%s", country);
  snprintf(url, sizeof(url), "https://itunes.apple.com/search?term=%s&media=music&entity=song&limit=3%s", encodedTerm, storeArg);
  size_t length = 0;
  if (!getPublic(url, "iTunes", [&](HTTPClient &http) { length = readSome(http.getStreamPtr(), (uint8_t *)buffer, size - 1); return length > 0; })) return false;
  buffer[length] = 0;
  const char *count = strstr(buffer, "\"resultCount\":");
  consoleLog("[HA] album art: iTunes \"%.50s\" -> %u bytes, %.18s\n", term, (unsigned)length, count ? count + 1 : "no resultCount");
  char gotArtist[kText + 8], wantNorm[kText], gotNorm[kText + 8];
  normalise(first, wantNorm, sizeof(wantNorm));
  for (const char *result = buffer; (result = strstr(result, "\"artistName\":\"")) != nullptr; result += 14) {
    unescape(result + 14, gotArtist, sizeof(gotArtist));
    normalise(gotArtist, gotNorm, sizeof(gotNorm));
    // Same artist when one name holds the other (the result may list "A & B"); a name with no Latin letters cannot be compared and is trusted
    if (wantNorm[0] && gotNorm[0] && !strstr(gotNorm, wantNorm) && !strstr(wantNorm, gotNorm)) { consoleLog("[HA] album art: skipped artist %.30s\n", gotArtist); continue; }
    const char *art = strstr(result + 14, "\"artworkUrl100\":\"");
    const char *next = strstr(result + 14, "\"artistName\":\"");
    if (!art || (next && next < art)) continue;
    unescape(art + 17, coverUrl, coverSize);
    char *size100 = strstr(coverUrl, "100x100");
    if (size100) memcpy(size100, "200x200", 7);
    return coverUrl[0] != 0;
  }
  setNote("No match on iTunes");
  return false;
}

// Deezer: a strict artist/track search, its first hit's medium cover (250 px)
bool findDeezer(const char *artist, const char *title, char *buffer, size_t size, char *coverUrl, size_t coverSize) {
  char cleaned[kText], first[kText];
  cleanTitle(title, cleaned, sizeof(cleaned));
  strlcpy(first, artist, std::min(sizeof(first), firstArtistLength(artist) + 1));
  char query[kText * 2 + 24], encoded[kText * 6 + 32], url[400];
  snprintf(query, sizeof(query), "%s %s", first, cleaned);
  encode(query, encoded, sizeof(encoded));
  snprintf(url, sizeof(url), "http://api.deezer.com/search?q=%s&limit=3", encoded);  // plain http: no TLS handshake, which the Home face has no memory for
  size_t length = 0;
  if (!getPublic(url, "Deezer", [&](HTTPClient &http) { length = readSome(http.getStreamPtr(), (uint8_t *)buffer, size - 1); return length > 0; })) return false;
  buffer[length] = 0;
  consoleLog("[HA] album art: Deezer \"%.50s\" -> %u bytes\n", query, (unsigned)length);
  char wantNorm[kText], gotArtist[kText + 8], gotNorm[kText + 8];
  normalise(first, wantNorm, sizeof(wantNorm));
  // Each hit lists its artist, then its album with the cover: the first hit by the same artist wins
  for (const char *hit = buffer; (hit = strstr(hit, "\"artist\":{")) != nullptr; hit += 10) {
    const char *name = strstr(hit, "\"name\":\"");
    const char *cover = strstr(hit + 10, "\"cover_medium\":\"");
    const char *next = strstr(hit + 10, "\"artist\":{");
    if (!name || !cover || (next && next < cover)) continue;
    unescape(name + 8, gotArtist, sizeof(gotArtist));
    normalise(gotArtist, gotNorm, sizeof(gotNorm));
    if (wantNorm[0] && gotNorm[0] && !strstr(gotNorm, wantNorm) && !strstr(wantNorm, gotNorm)) { consoleLog("[HA] album art: skipped artist %.30s\n", gotArtist); continue; }
    unescape(cover + 16, coverUrl, coverSize);
    return coverUrl[0] != 0;
  }
  setNote("No match on Deezer");
  return false;
}

// "artist|title" -> pixels: look the cover up on Deezer, then iTunes, then fetch it
bool lookup(const char *query, uint16_t *out) {
  constexpr size_t kReply = 4096;
  std::unique_ptr<char[]> memory(new (std::nothrow) char[kReply + kPicture + kText * 2 + 2]);
  if (!memory) { setNote("Out of memory"); return false; }
  char *reply = memory.get(), *coverUrl = reply + kReply, *artist = coverUrl + kPicture;
  const char *bar = strchr(query, '|');
  if (!bar) { setNote("Bad lookup key"); return false; }
  copyField(artist, kText, query, bar - query);
  char *title = artist + kText;
  strlcpy(title, bar + 1, kText);
  coverUrl[0] = 0;
  // Deezer first: one catalogue for every country. iTunes searches the US store, which lacks many local artists
  bool found = findDeezer(artist, title, reply, kReply, coverUrl, kPicture);
  if (!found) found = findItunes(artist, title, reply, kReply, coverUrl, kPicture);
  if (!found || strncmp(coverUrl, "http", 4)) return false;
  plainCoverUrl(coverUrl);
  consoleLog("[HA] album art: found %.60s\n", coverUrl);
  char cover[kPicture];  // on the task's stack: the 4 KB reply buffer goes back before the picture is downloaded and decoded
  strlcpy(cover, coverUrl, sizeof(cover));
  memory.reset();
  if (downloadPublic(cover, out)) return true;
  // Progressive JPEGs (some Deezer covers) are beyond the decoder: wsrv.nl sends back a baseline 64x64 one
  char encoded[300], proxied[420];
  encode(cover, encoded, sizeof(encoded));
  snprintf(proxied, sizeof(proxied), "http://wsrv.nl/?url=%s&w=64&h=64&fit=cover&output=jpg&q=95", encoded);
  consoleLog("[HA] album art: trying again through the resizer\n");
  return downloadPublic(proxied, out);
}

// A station's logo: radio-browser.info knows stations by name and the logo (mostly a PNG) by address; wsrv.nl turns it into the
// 64x64 baseline JPEG the decoder reads
bool findLogo(const char *station, char *buffer, size_t size, char *logoUrl, size_t logoSize) {
  char country[3] = "", countryArg[24] = "";
  if (ultime::countryForZone(Settings::getInstance()->timezone.c_str(), country)) snprintf(countryArg, sizeof(countryArg), "&countrycode=%s", country);
  char encoded[kText * 3 + 8], url[300];
  encode(station, encoded, sizeof(encoded));
  static const char *const kServers[] = {"de1", "all"};
  size_t length = 0;
  for (const char *server : kServers) {
    snprintf(url, sizeof(url), "http://%s.api.radio-browser.info/json/stations/search?name=%s%s&limit=3&hidebroken=true&order=clickcount&reverse=true", server, encoded, countryArg);
    length = 0;
    if (getPublic(url, "Station directory", [&](HTTPClient &http) { length = readSome(http.getStreamPtr(), (uint8_t *)buffer, size - 1); return length > 0; })) break;
    length = 0;
  }
  if (!length) return false;
  buffer[length] = 0;
  consoleLog("[HA] album art: station directory \"%.30s\" -> %u bytes\n", station, (unsigned)length);
  for (const char *at = buffer; (at = strstr(at, "\"favicon\":\"")) != nullptr; at += 11) {  // the most played match that has a logo
    unescape(at + 11, logoUrl, logoSize);
    if (!strncmp(logoUrl, "http", 4) && strlen(logoUrl) < 150) return true;
  }
  setNote("No station logo found");
  return false;
}

bool stationLogo(const char *station, uint16_t *out) {
  constexpr size_t kReply = 4096;
  std::unique_ptr<char[]> memory(new (std::nothrow) char[kReply + 160 + 700]);
  if (!memory) { setNote("Out of memory"); return false; }
  char *reply = memory.get(), *logoUrl = reply + kReply, *proxied = logoUrl + 160;
  if (!findLogo(station, reply, kReply, logoUrl, 160)) return false;
  char encoded[500];
  encode(logoUrl, encoded, sizeof(encoded));
  snprintf(proxied, 700, "http://wsrv.nl/?url=%s&w=64&h=64&fit=contain&bg=black&output=jpg&q=95", encoded);
  consoleLog("[HA] album art: station logo %.60s\n", logoUrl);
  char logo[560];  // the proxy address: up to 150 characters of logo address, percent-encoded, plus the resize options
  strlcpy(logo, proxied, sizeof(logo));
  memory.reset();
  return downloadPublic(logo, out);
}

// The panel washes pictures out, and a JPEG leaves greys where a logo should be black and white. Levels first: the darkest 5% of
// the picture (at most up to 48) becomes black and the lightest 5% (at least from 160) white, then the last 21 steps next to black and next to white are cut off (a stretch of 1.2), which removes the JPEG speckle in flat areas
void addContrast(uint16_t *pixels) {
  constexpr size_t kPixels = (size_t)kArt * kArt;
  uint16_t histogram[256] = {};
  for (size_t i = 0; i < kPixels; i++) {
    const uint16_t c = pixels[i];
    const uint32_t r = (c >> 8 & 0xF8) | (c >> 13), g = (c >> 3 & 0xFC) | (c >> 9 & 3), b = (c << 3 & 0xF8) | (c >> 2 & 7);
    histogram[(r * 77 + g * 150 + b * 29) >> 8]++;
  }
  int low = 0, high = 255;
  for (size_t seen = 0; low < 48 && (seen += histogram[low]) < kPixels / 20; low++) {}
  for (size_t seen = 0; high > 160 && (seen += histogram[high]) < kPixels / 20; high--) {}
  uint8_t lut[256];
  for (int v = 0; v < 256; v++) {
    const int level = std::min(255, std::max(0, (v - low) * 255 / (high - low)));
    lut[v] = (uint8_t)std::min(255, std::max(0, (level - 21) * 255 / 213));  // what is left of black or white after the JPEG (dots) goes flat
  }
  for (size_t i = 0; i < kPixels; i++) {
    const uint16_t c = pixels[i];
    const uint8_t r = lut[(c >> 8 & 0xF8) | (c >> 13)], g = lut[(c >> 3 & 0xFC) | (c >> 9 & 3)], b = lut[(c << 3 & 0xF8) | (c >> 2 & 7)];
    pixels[i] = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
  }
}

void fetchTask(void *) {
  char picture[kPicture], query[sizeof(wantedQuery)], station[sizeof(wantedStation)];
  uint16_t *pixels;
  {
    std::lock_guard<std::mutex> guard(lock);
    strlcpy(picture, wanted, sizeof(picture));
    strlcpy(query, wantedQuery, sizeof(query));
    strlcpy(station, wantedStation, sizeof(station));
    haveFor[0] = 0;  // the cover is rewritten in place: nobody draws it meanwhile
    if (!ready) ready = (uint16_t *)malloc(kArt * kArt * sizeof(uint16_t));
    pixels = ready;
  }
  bool ok = false;
  consoleLog("[HA] album art: fetch task started for %.50s (free %u, largest %u)\n", picture, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (!pixels) setNote("Out of memory");
  else if (WiFi.status() != WL_CONNECTED) setNote("No Wi-Fi for the cover");
  else if (feedsHalted()) setNote("Feeds are paused (update, WLED or a picture)");
  else {
    if (picture[0] == '/') ok = download(config(), picture, pixels);
    else if (picture[0] == '~') ok = stationLogo(picture + 1, pixels);  // "~station": nothing but the station to go on
    else if (picture[0] == '?') ok = lookup(picture + 1, pixels);  // "?artist|title": no picture of its own, search for one
    else ok = downloadPublic(picture, pixels);
    if (!ok && picture[0] != '?' && query[0]) {  // the player's own picture is missing, wrong or not a baseline JPEG: search for the cover
      consoleLog("[HA] album art: picture failed, searching %.40s\n", query);
      ok = lookup(query, pixels);
    }
    if (!ok && picture[0] != '~' && station[0]) {  // no cover for the track anywhere: the station's logo instead
      consoleLog("[HA] album art: no cover, looking for the logo of %.40s\n", station);
      ok = stationLogo(station, pixels);
    }
  }
  if (ok) addContrast(pixels);
  {
    std::lock_guard<std::mutex> guard(lock);
    if (ok) { strlcpy(haveFor, picture, sizeof(haveFor)); coverId++; }
    else { strlcpy(failedFor, picture, sizeof(failedFor)); failedAt = millis(); }
  }
  if (ok) setNote("Loaded");
  fetching = false;
  vTaskDelete(nullptr);
}

// "ICY" servers (Shoutcast, Icecast, StreamTheWorld) name the station in the response headers, before any audio: ask and hang up
bool readIcyName(const char *url, char *name, size_t size) {
  if (strncmp(url, "http://", 7)) return false;
  const char *at = url + 7, *slash = strchr(at, '/');
  char host[64];
  const size_t hostLength = slash ? (size_t)(slash - at) : strlen(at);
  if (!hostLength || hostLength >= sizeof(host)) return false;
  memcpy(host, at, hostLength);
  host[hostLength] = 0;
  int port = 80;
  if (char *colon = strchr(host, ':')) { *colon = 0; port = atoi(colon + 1); }
  std::unique_ptr<WiFiClient> client(new (std::nothrow) WiFiClient);
  if (!client) { consoleLog("[HA] station: out of memory\n"); return false; }
  client->setTimeout(12000);  // milliseconds (Stream::setTimeout): StreamTheWorld takes about 5 s to start answering
  if (!client->connect(host, port, 4000)) { consoleLog("[HA] station: cannot connect to %.40s:%d\n", host, port); return false; }
  client->printf("GET %s HTTP/1.0\r\nHost: %s\r\nIcy-MetaData: 1\r\nUser-Agent: Ultimatrix\r\nConnection: close\r\n\r\n", slash ? slash : "/", host);
  bool found = false;
  for (int lines = 0; lines < 40 && !found; lines++) {  // "ICY 200 OK" or "HTTP/1.0 200 OK", then headers up to the empty line
    String line = client->readStringUntil('\n');
    line.trim();
    if (!line.length()) break;
    String lower = line;
    lower.toLowerCase();
    if (!lower.startsWith("icy-name:")) continue;
    String value = line.substring(9);
    value.trim();
    char raw[kText * 3];
    size_t n = 0;
    for (size_t i = 0; i < value.length() && n + 1 < sizeof(raw); i++) raw[n++] = value[i] == '"' ? '\'' : value[i] == '\\' ? ' ' : value[i];
    raw[n] = 0;
    unescape(raw, name, size);  // UTF-8 folded to what the fonts have
    found = name[0] != 0;
  }
  client->stop();
  if (!found) consoleLog("[HA] station: no icy-name header from %.40s\n", host);
  return found;
}

void icyTask(void *) {
  char url[kStream], name[kText] = "";
  {
    std::lock_guard<std::mutex> guard(lock);
    strlcpy(url, current.stream, sizeof(url));
  }
  bool ok = false;
  if (!url[0]) consoleLog("[HA] station: no stream address\n");
  else if (WiFi.status() != WL_CONNECTED) consoleLog("[HA] station: no Wi-Fi\n");
  else if (feedsHalted()) consoleLog("[HA] station: feeds are paused\n");
  else ok = readIcyName(url, name, sizeof(name));
  {
    std::lock_guard<std::mutex> guard(lock);
    if (ok) { strlcpy(icyFor, url, sizeof(icyFor)); strlcpy(icyName, name, sizeof(icyName)); icyKnown = true; changes++; }
    else { strlcpy(icyFailedFor, url, sizeof(icyFailedFor)); icyFailedAt = millis(); }
  }
  if (ok) consoleLog("[HA] station: %s\n", name);
  icyFetching = false;
  vTaskDelete(nullptr);
}

}  // namespace

size_t unescape(const char *json, char *out, size_t capacity) {
  size_t n = 0;
  auto put = [&](char c) { if (n + 1 < capacity) out[n++] = c; };
  for (const unsigned char *p = (const unsigned char *)json; *p && *p != '"';) {
    uint32_t cp;
    if (*p == '\\') {
      p++;
      switch (*p) {
        case 'u': {
          cp = 0;
          for (int i = 1; i <= 4 && p[i]; i++) cp = cp << 4 | (p[i] <= '9' ? p[i] - '0' : (p[i] | 32) - 'a' + 10);
          p += p[1] && p[2] && p[3] && p[4] ? 5 : 1;
          break;
        }
        case 'n': case 't': case 'r': cp = ' '; p++; break;
        case 0: continue;
        default: cp = *p++; break;
      }
    } else if (*p < 0x80) {
      cp = *p++;
    } else {  // UTF-8
      const int extra = *p >= 0xF0 ? 3 : *p >= 0xE0 ? 2 : 1;
      cp = *p & (0x3F >> extra);
      p++;
      for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++) cp = cp << 6 | (*p++ & 0x3F);
    }
    put(fold(cp));
  }
  out[n] = 0;
  return n;
}

size_t buildTemplate(const Config &config, char *out, size_t capacity) {
  (void)config;
  char (*players)[kEntity] = mediaPlayers();
  size_t at = 0;
  auto add = [&](const char *text) {
    const size_t length = strlen(text);
    if (at + length + 1 > capacity) { at = capacity; return; }
    memcpy(out + at, text, length);
    at += length;
  };
  bool any = false;
  add("{% set ns = namespace(r=[]) %}{% for e in [");
  for (int i = 0; i < kRooms; i++) {
    if (!players[i][0]) continue;
    add(any ? ",'" : "'");
    add(players[i]);
    add("'");
    any = true;
  }
  if (!any) return 0;
  // One entry per playing player: room|title|artist|picture|position|duration|station|stream (a radio stream's address, only for plain-http Sonos radio). Text is cut and cleaned here, the picture is dropped when too long to keep whole.
  add("] %}{% if is_state(e,'playing') %}{% set up = state_attr(e,'media_position_updated_at') %}{% set dur = state_attr(e,'media_duration')|int(0) %}{% set pos = ((state_attr(e,'media_position')|float(0)) + ((as_timestamp(now()) - as_timestamp(up)) if up else 0))|int(0) %}{% set pic = state_attr(e,'entity_picture_local') or state_attr(e,'entity_picture') or '' %}{% set station = state_attr(e,'media_channel') or '' %}{% set cid = (state_attr(e,'media_content_id') or '')|string %}"
      "{% set ns.r = ns.r + [(state_attr(e,'friendly_name') or e)|string|replace('|',' ')|replace(';',' ')|truncate(15,true,'') ~ '|'"
      " ~ (state_attr(e,'media_title') or '')|string|replace('|',' ')|replace(';',' ')|truncate(47,true,'') ~ '|'"
      " ~ (state_attr(e,'media_artist') or '')|string|replace('|',' ')|replace(';',' ')|truncate(47,true,'') ~ '|'"
      " ~ (pic if pic|length < 256 else '!' ~ pic|length)|replace(';','')|replace('|','') ~ '|' ~ pos ~ '|' ~ dur ~ '|' ~ station|string|replace('|',' ')|replace(';',' ')|truncate(47,true,'') ~ '|' ~ (cid|replace('x-rincon-mp3radio://','') if cid.startswith('x-rincon-mp3radio://http://') else '')|replace('|','')|replace(';','')] %}{% endif %}{% endfor %}{{ ns.r|join(';') }}");
  if (at >= capacity) return 0;
  out[at] = 0;
  return at;
}

void parseResult(const char *text, Snapshot &out) {
  memset(&out, 0, sizeof(out));
  const char *at = text;
  while (*at && out.count < kRooms) {
    const char *end = strchr(at, ';');
    if (!end) end = at + strlen(at);
    const char *part[8] = {at, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    size_t length[8] = {};
    int field = 0;
    for (const char *c = at; c <= end && field < 8; c++) {
      if (c == end || *c == '|') {
        length[field] = c - part[field];
        if (++field < 8) part[field] = c + 1;
      }
    }
    if (field >= 6) {  // the station and the stream address are optional
      Player &p = out.player[out.count++];
      copyField(p.room, sizeof(p.room), part[0], length[0]);
      copyField(p.title, sizeof(p.title), part[1], length[1]);
      copyField(p.artist, sizeof(p.artist), part[2], length[2]);
      copyField(p.picture, sizeof(p.picture), part[3], length[3]);
      p.position = strtoul(part[4], nullptr, 10);
      p.duration = strtoul(part[5], nullptr, 10);
      if (field >= 7 && length[6]) copyField(p.station, sizeof(p.station), part[6], length[6]);
      // A radio stream without a station name from Home Assistant: its address, for the clock to ask the stream itself (plain http only)
      if (field >= 8 && length[7] && !p.station[0] && !out.stream[0] && length[7] < sizeof(out.stream) && !strncmp(part[7], "http://", 7)) {
        copyField(out.stream, sizeof(out.stream), part[7], length[7]);
        p.needsStation = 1;
      }
    }
    at = *end ? end + 1 : end;
  }
}

void clear() {
  std::lock_guard<std::mutex> guard(lock);
  if (!current.count) return;
  memset(&current, 0, sizeof(current));
  changes++;
}

void publish(const Snapshot &snapshot) {
  std::lock_guard<std::mutex> guard(lock);
  current = snapshot;
  for (int i = 0; i < current.count; i++) current.player[i].at = millis();
  haveSnapshot = true;
  changes++;
}

bool snapshot(Snapshot &out) {
  std::lock_guard<std::mutex> guard(lock);
  out = current;
  resolve(out);
  return haveSnapshot;
}

void poll() {
  if (icyFetching) return;
  {
    std::lock_guard<std::mutex> guard(lock);
    if (!current.stream[0] || (icyKnown && !strcmp(icyFor, current.stream))) return;  // nothing to ask, or already asked
    if (!strcmp(icyFailedFor, current.stream) && millis() - icyFailedAt < 60000) return;
  }
  static uint32_t nextTry = 0;  // asked every frame: the heap walk below at most once a second
  if ((int32_t)(millis() - nextTry) < 0) return;
  nextTry = millis() + 1000;
  if (feedsHalted() || WiFi.status() != WL_CONNECTED || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 8000) return;
  if (icyFetching.exchange(true)) return;
  if (xTaskCreatePinnedToCore(icyTask, "haIcy", 6144, nullptr, 1, nullptr, 0) != pdPASS) icyFetching = false;  // core 0 with the other feeds
}

uint32_t version() { return changes; }

void artNote(char *out, size_t size) {
  std::lock_guard<std::mutex> guard(lock);
  strlcpy(out, note, size);
}

void release() {
  if (fetching) return;
  std::lock_guard<std::mutex> guard(lock);
  free(ready);
  ready = nullptr;
  haveFor[0] = 0;
}

ArtState art(const Player &player, bool online, bool logo, const uint16_t **pixels, uint16_t *id) {
  const char *picture = player.picture;
  char key[kPicture];  // what the cover is cached under: the picture, or "?artist|title" for one found online
  const bool home = picture[0] == '/';
  const bool outside = !strncmp(picture, "http://", 7) || !strncmp(picture, "https://", 8);
  if (home || (outside && online)) strlcpy(key, picture, sizeof(key));
  else if (online && player.artist[0] && player.title[0]) snprintf(key, sizeof(key), "?%s|%s", player.artist, player.title);
  else if (logo && player.station[0]) snprintf(key, sizeof(key), "~%s", player.station);  // no cover to be had: the station's logo
  else {
    if (!picture[0]) setNote(!online && !logo ? "No picture; online art is off" : "No picture, artist, title or station known");
    else if (picture[0] == '!') setNote("Picture address too long (%s)", picture + 1);
    else setNote("Picture is not on Home Assistant: %.14s", picture);
    return ART_NONE;
  }
  picture = key;
  static int lastFlags = -1;  // a changed switch under Pages is a new chance: an earlier failure must not hold the picture back for minutes
  if (lastFlags != (int)online + 2 * (int)logo) {
    lastFlags = (int)online + 2 * (int)logo;
    std::lock_guard<std::mutex> guard(lock);
    failedFor[0] = 0;
  }
  static char lastKey[kPicture] = "";  // only the art task's caller (the clock loop) touches it
  if (strcmp(lastKey, key)) { strlcpy(lastKey, key, sizeof(lastKey)); consoleLog("[HA] album art: wanted %.60s (online art %s)\n", key, online ? "on" : "off"); }
  {
    std::lock_guard<std::mutex> guard(lock);
    if (ready && !strcmp(haveFor, picture)) { *pixels = ready; *id = coverId; return ART_READY; }
    if (!strcmp(failedFor, picture) && millis() - failedAt < (picture[0] == '?' || picture[0] == '~' ? kLookupRetryMs : kRetryMs)) return ART_FAILED;
    strlcpy(wanted, picture, sizeof(wanted));
    if (online && player.artist[0] && player.title[0]) snprintf(wantedQuery, sizeof(wantedQuery), "%s|%s", player.artist, player.title);
    else wantedQuery[0] = 0;
    if (logo) strlcpy(wantedStation, player.station, sizeof(wantedStation));
    else wantedStation[0] = 0;
  }
  // The page asks every frame: the heap checks below (they walk the heap) run once a second at most
  static uint32_t nextGate = 0;
  if ((int32_t)(millis() - nextGate) < 0) return ART_LOADING;
  if (fetching.exchange(true)) {  // one fetch at a time; the page asks again
    static uint32_t told = 0;
    if (millis() - told > 5000) { told = millis(); consoleLog("[HA] album art: waiting, a cover fetch is still running\n"); }
    return ART_LOADING;
  }
  // Data memory only: getMaxAllocHeap() counts instruction RAM that no buffer can use
  if (feedsHalted() || WiFi.status() != WL_CONNECTED || heap_caps_get_free_size(MALLOC_CAP_8BIT) < 30000 || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 14000) {
    fetching = false;
    nextGate = millis() + 1000;
    setNote("Waiting for memory (%u free)", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ART_LOADING;
  }
  // Core 0 like the other feeds: the TLS handshakes must not take time from the drawing core
  {
    std::lock_guard<std::mutex> guard(lock);
    if (!ready) ready = (uint16_t *)malloc(kArt * kArt * sizeof(uint16_t));  // before the task takes its stack, so the biggest block goes to the buffer
    if (!ready) { fetching = false; strlcpy(note, "Out of memory (cover buffer)", sizeof(note)); }
  }
  if (!fetching) return ART_LOADING;
  if (xTaskCreatePinnedToCore(fetchTask, "haArt", config().tls || (key[0] != '/' && key[0] != '~') ? 12288 : 9216, nullptr, 1, nullptr, 0) != pdPASS) { fetching = false; setNote("Could not start the fetch"); }
  return ART_LOADING;
}

}  // namespace media
}  // namespace ha
