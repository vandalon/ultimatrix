// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ClockTime.h>
#include "NflTypes.h"

// The pages of the NFL face. Everything is drawn through the facekit put/box helpers, so the face can slide one page
// over another; the same code renders the native previews.
namespace nflview {

constexpr int kStandingsPerPage = 6;
constexpr uint32_t kScoreMs = 4200;

// What is behind a page. A page draws its own unless the face asked otherwise (backgroundDrawnByFace) and drew it itself: two pages that share a
// background then slide over it instead of taking it along.
enum Background : uint8_t { BgGame, BgLine, BgNext, BgAfc, BgNfc, BgClock };
void drawBackground(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms);
void backgroundDrawnByFace(bool yes);

// "KC,GB" favourites: is this team one
bool isFavourite(const char *favourites, const char *abbr);
bool hasFavourite(const char *favourites, const nfl::Game &game);

// The games of the week in the order of the pages: favourites, then live, coming and finished
int orderGames(const nfl::Snapshot &snap, const char *favourites, int *out);
uint16_t upcomingMs(const nfl::Snapshot &snap, ClockTime *time, uint16_t pageMs);  // how long the scrolling list needs to pass once (at least pageMs)
int standingsPages(const nfl::Table &table);

// One game: quarter and clock on top, both teams with the score, the ball and its field strip, down and distance, timeouts
void drawGame(MatrixPanel_I2S_DMA &d, const nfl::Game &game, ClockTime *time, uint32_t ms, bool flashAway, bool flashHome, bool favourite);
// The points per quarter, the down and distance and the timeouts
void drawLineScore(MatrixPanel_I2S_DMA &d, const nfl::Game &game, ClockTime *time, uint32_t ms);
// The coming days next to the conference seeds; elapsed = how long the page has been up
void drawUpcoming(MatrixPanel_I2S_DMA &d, const nfl::Snapshot &snap, const char *favourites, ClockTime *time, uint32_t ms, uint32_t elapsed, uint32_t groupMs);
// A conference table, six teams to a page
void drawStandings(MatrixPanel_I2S_DMA &d, const nfl::Table &table, bool afc, const char *favourites, int page, ClockTime *time, uint32_t ms);
// The date and the seconds on top, hours and minutes in two colours (the pacmancolors list: 10 white, 3 pastel blue), the next game over the field and scenes with a ball (next: nullptr when nothing is coming)
void drawClock(MatrixPanel_I2S_DMA &d, ClockTime *time, uint32_t ms, const nfl::Fixture *next, uint8_t hoursColour = 10, uint8_t minutesColour = 3);
// A score: touchdown, field goal, extra point or safety, as the text flash (ms since it began, 0 to kScoreMs)
void drawScore(MatrixPanel_I2S_DMA &d, const nfl::Game &game, bool home, int points, uint32_t ms);

}  // namespace nflview
