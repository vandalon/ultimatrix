// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ClockTime.h>
#include "MlbTypes.h"

// The pages of the MLB face. Everything is drawn through the facekit put/box helpers, so the face can slide one page
// over another; the same code renders the native previews.
namespace mlbview {

constexpr int kGamesPerPage = 4, kStandingsPerPage = 8;
constexpr uint32_t kRunMs = 2600;

// What is behind a page. A page draws its own unless the face asked otherwise (backgroundDrawnByFace) and drew it itself: two pages that share a
// background then slide over it instead of taking it along.
enum Background : uint8_t { BgGame, BgLine, BgToday, BgNext, BgAmerican, BgNational, BgClock };
void drawBackground(MatrixPanel_I2S_DMA &d, Background background, uint32_t ms);
void backgroundDrawnByFace(bool yes);

// "NYY,LAD" favourites: is this team one
bool isFavourite(const char *favourites, const char *abbr);
bool hasFavourite(const char *favourites, const mlb::Game &game);

// The games of today in the order of the list page: favourites, then live, coming and finished
int orderGames(const mlb::Snapshot &snap, const char *favourites, int *out);
int gamePages(int count);
int fixturePages(const mlb::Snapshot &snap, ClockTime *time);
uint16_t upcomingMs(const mlb::Snapshot &snap, ClockTime *time, uint16_t pageMs);  // how long the scrolling list needs to pass once (at least pageMs)
int standingsPages(const mlb::Table &table);

// One game, big: the time on top, both teams with the runs of the innings and the score, the bases, count and outs, an inning bar at the bottom
void drawGame(MatrixPanel_I2S_DMA &d, const mlb::Game &game, ClockTime *time, uint32_t ms, bool flashAway, bool flashHome, bool favourite, int page, int pages);
// The line score: runs per inning (the latest seven) with runs, hits and errors, and the bases, count and outs
void drawLineScore(MatrixPanel_I2S_DMA &d, const mlb::Game &game, ClockTime *time, uint32_t ms);
// Four games to a page, one line of teams and score and one of status
void drawToday(MatrixPanel_I2S_DMA &d, const mlb::Snapshot &snap, const int *order, int count, const char *favourites, int page, ClockTime *time, uint32_t ms);
// The coming days
void drawUpcoming(MatrixPanel_I2S_DMA &d, const mlb::Snapshot &snap, const char *favourites, int page, ClockTime *time, uint32_t ms, uint32_t elapsed = 0, uint32_t groupMs = 8000);
// The playoff series for the left column of the next-up page: round, both teams with their wins, a box per game, the next game
void drawSeriesCard(MatrixPanel_I2S_DMA &d, const mlb::Series &series);
// The league table, eight teams to a page
void drawStandings(MatrixPanel_I2S_DMA &d, const mlb::Table &table, bool american, const char *favourites, int page, ClockTime *time, uint32_t ms);
// Without games: hours over minutes, the date, scenes with a ball or a player, and the next game under the date (none: nullptr)
void drawClock(MatrixPanel_I2S_DMA &d, ClockTime *time, uint32_t ms, const mlb::Fixture *next);
// The next game in the free space under the date of the clock page
void drawNextGame(MatrixPanel_I2S_DMA &d, ClockTime *time, const mlb::Fixture &fixture);
// A run: the ball over the fence, +runs and the new score (ms since it began, 0 to kRunMs)
void drawRun(MatrixPanel_I2S_DMA &d, const mlb::Game &game, bool home, int runs, uint32_t ms);

}  // namespace mlbview
