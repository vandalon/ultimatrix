// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "HaConfig.h"

// The Now playing page: what the chosen media players are playing, and the album art of the one on show.
// Home Assistant renders a second, small template for the players (HaClient subscribes to it); the art comes from
// the player's entity_picture, fetched over HTTP by a short-lived task and shrunk by the ROM JPEG decoder.
namespace ha {
namespace media {

constexpr int kArt = 64;          // album art fills the panel
constexpr int kText = 48, kRoom = 16, kPicture = 256;

struct Player {
  char room[kRoom];       // Home Assistant's friendly name
  char title[kText], artist[kText];
  char picture[kPicture];  // entity_picture path, empty when the player has none
  uint32_t position, duration;  // seconds; position as of `at`, duration 0 when unknown (streams)
  uint32_t at;                  // millis() when the result arrived
};
struct Snapshot {
  Player player[kRooms];  // only players that are playing, in the order of the settings
  uint8_t count;
};

// The template for the saved rooms; returns its length, 0 when there is no room or it does not fit
size_t buildTemplate(const Config &config, char *out, size_t capacity);
// A rendered result ("room|title|artist|picture|position|duration;..."), already unescaped, into a snapshot
void parseResult(const char *text, Snapshot &out);
// A JSON string value ("result":"...") copied out with its escapes resolved and accents folded to plain ASCII
size_t unescape(const char *json, char *out, size_t capacity);

void clear();                            // nothing is playing as far as we know (connection lost, no player chosen)
void publish(const Snapshot &snapshot);   // from the Home Assistant task
bool snapshot(Snapshot &out);             // false until a result has arrived
uint32_t version();

enum ArtState : uint8_t { ART_NONE, ART_LOADING, ART_READY, ART_FAILED };
// The art of a picture path: the first call starts the fetch, later calls report it. When READY, *pixels points at kArt*kArt RGB565
// (kept until the next fetch) and *id changes with every new cover.
void artNote(char *out, size_t size);  // what the last album art fetch did, in a few words
ArtState art(const char *picture, const uint16_t **pixels, uint16_t *id);
void release();  // frees the cover (8 KB) once nothing plays; the caller no longer draws from the pointer

}  // namespace media
}  // namespace ha
