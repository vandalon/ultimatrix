// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "TeamColors.h"

// Colours as 0xRRGGBB. Black doesn't show on the LEDs, so black shorts are a
// dark grey, and a black second colour means none. Striped or hooped shirts
// use their most recognisable colour.
static const uint32_t WHITE = 0xffffff;
static const uint32_t BLACK = 0x404040;
static const uint32_t RED = 0xe30613;
static const uint32_t NAVY = 0x1a2d69;

// second is the club's other colour where it differs from the shorts (e.g.
// Feyenoord's white); left out it's the shorts colour.
static const struct {
  const char *team;  // ESPN abbreviation, or "ABBR@league" when ESPN uses it twice
  uint32_t shirt, shorts, second = 0;
} KITS[] = {
  // Eredivisie
  {"ADO", 0xffd200, 0x00843d},  // ADO Den Haag: yellow and green, green shorts
  {"AJA", 0xdf1b27, WHITE},     // Ajax: red and white, white shorts
  {"AZ", 0xef2f24, WHITE},
  {"CAM", 0xffd200, 0x0055a5},  // Cambuur: yellow and blue
  {"EXC", 0xd2112c, BLACK, BLACK},     // Excelsior: red and black stripes
  {"FEY", 0xef2f24, BLACK, WHITE},     // Feyenoord: red and white halves, black shorts
  {"FOR", 0xffd200, 0x00843d},  // Fortuna Sittard: yellow and green
  {"GAE", 0xe30613, 0xffd200},  // Go Ahead Eagles: red and yellow
  {"GRO", 0x00843d, WHITE},     // Groningen: green and white
  {"HEE", 0x0055a5, WHITE},     // Heerenveen: blue and white
  {"NEC", 0xe30613, BLACK, 0x00843d},     // NEC: red, green and black stripes
  {"PEC", 0x0050a0, WHITE},     // PEC Zwolle: blue and white
  {"PSV", 0xef2f24, BLACK, WHITE},     // PSV: red and white stripes, black shorts
  {"SPA", 0xe30613, BLACK, WHITE},     // Sparta: red and white stripes, black shorts
  {"TEL", WHITE, 0xc60000},     // Telstar: white, red
  {"TWE", 0xf31522, WHITE},
  {"UTR", 0xf31522, WHITE},
  {"WIL", 0xd71920, NAVY, WHITE},      // Willem II: red, white and blue stripes, blue shorts

  // Bundesliga
  {"B04", 0xda0308, BLACK, BLACK},     // Leverkusen
  {"BMG", WHITE, BLACK, 0x03915c},        // Gladbach
  {"DOR", 0xffee00, BLACK, BLACK},     // Dortmund
  {"ELV", WHITE, BLACK, BLACK},        // Elversberg: black and white
  {"FCA", 0xc8102e, WHITE, 0x00843d},     // Augsburg: red, green and white
  {"FCU", 0xda0308, WHITE},     // Union Berlin
  {"HSV", WHITE, 0xda0308},     // Hamburg: white shirts, red shorts
  {"KOE", WHITE, WHITE, 0xda0308},        // Cologne
  {"M05", 0xda0308, WHITE},     // Mainz
  {"MUN", 0xdc052d, WHITE},     // Bayern
  {"RBL", WHITE, 0xdd0741},     // Leipzig
  {"S04", 0x0050c8, WHITE},     // Schalke
  {"SCF", 0xda0308, BLACK, BLACK},     // Freiburg
  {"SCP@ger.1", 0x0050a0, BLACK, BLACK},  // Paderborn: blue and black
  {"SGE", 0xe1000f, BLACK, BLACK},     // Frankfurt: red and black
  {"SVW", 0x03915c, WHITE},     // Werder Bremen
  {"TSG", 0x1c63b7, WHITE},     // Hoffenheim
  {"VFB", WHITE, WHITE, 0xda0308},        // Stuttgart

  // Champions League regulars from other countries
  {"ARS", 0xef0107, WHITE},     // Arsenal
  {"ATM", 0xcb3524, 0x1a2d69, WHITE},  // Atlético: red and white stripes, blue shorts
  {"AVL", 0x7a1e3a, WHITE, 0x95bfe5},     // Aston Villa: claret
  {"BAR", 0xa50044, 0x004d98},  // Barcelona: blaugrana
  {"BET", 0x00954c, WHITE},     // Real Betis: green and white stripes
  {"BRU", 0x0062b0, BLACK, BLACK},     // Club Brugge: blue and black stripes
  {"CEL", 0x018749, WHITE},     // Celtic: green and white hoops
  {"CHE", 0x034694, 0x034694, WHITE},  // Chelsea
  {"FCP", 0x0060a8, WHITE},     // Porto: blue and white stripes
  {"FEN", 0xffed00, NAVY},      // Fenerbahçe: yellow and navy stripes
  {"GAL", 0xa90432, 0xfdb912},  // Galatasaray: red and yellow
  {"INT", 0x0068a8, BLACK, BLACK},     // Inter: blue and black stripes
  {"JUV", WHITE, BLACK, BLACK},        // Juventus: black and white stripes
  {"LIV", 0xc8102e, 0xc8102e, WHITE},  // Liverpool
  {"MAN", 0xda291c, WHITE},     // Manchester United
  {"MIL", 0xfb090b, WHITE, BLACK},     // AC Milan: red and black stripes
  {"MNC", 0x6cabdd, WHITE},     // Manchester City
  {"NAP", 0x12a0d7, WHITE},     // Napoli
  {"PSG", 0x004170, 0x004170, 0xe30613},  // Paris Saint-Germain
  {"RMA", WHITE, WHITE},        // Real Madrid
  {"ROMA", 0x8e1f2f, WHITE, 0xf0bc42},    // Roma
  {"SCP", 0x008057, WHITE},     // Sporting: green and white hoops
  {"SHK", 0xf26a21, BLACK, BLACK},     // Shakhtar: orange and black
  {"SLB", 0xe83030, WHITE},     // Benfica
  {"TOT", WHITE, NAVY},         // Tottenham
  {"VIL", 0xffe667, 0xffe667},  // Villarreal

  // European national teams
  {"ALB", 0xe41e20, BLACK, BLACK},
  {"AUT", 0xd72b2c, WHITE},
  {"BEL", 0xe30613, 0xe30613, 0xfdda24},
  {"BIH", 0x002395, 0x002395, 0xfecb00},
  {"BUL", WHITE, 0x00966e},
  {"CRO", 0xff0000, WHITE},     // red and white checks
  {"CZE", 0xd7141a, WHITE},
  {"DEN", 0xd02a3e, WHITE},
  {"ENG", WHITE, NAVY},
  {"ESP", 0xc60b1e, NAVY, 0xffc400},
  {"FIN", WHITE, 0x003580},
  {"FRA", 0x1d2d5b, WHITE},
  {"GER", WHITE, BLACK, BLACK},
  {"HUN", 0xce2029, WHITE},
  {"IRL", 0x049a64, WHITE},
  {"ISL", 0x0c2fff, 0x0c2fff, WHITE},
  {"ITA", 0x1e5fbf, WHITE},
  {"LTU", 0xffe400, 0x006a44},
  {"NED", 0xff6200, WHITE},
  {"NIR", 0x00843d, WHITE},
  {"NOR", 0xc8102e, WHITE},
  {"POL", WHITE, 0xdc143c},
  {"POR", 0xda291c, 0x006600},
  {"ROU", 0xfcd116, 0x002b7f},
  {"SCO", NAVY, WHITE},
  {"SRB", 0xe70000, 0x0c4076},
  {"SUI", 0xff0000, WHITE},
  {"SWE", 0xfecb00, 0x006aa7},
  {"TUR", 0xe30a17, WHITE},
  {"UKR", 0xfede00, 0x0057b7},
  {"WAL", 0xe70000, WHITE},

  // National teams from other continents (World Cup)
  {"ARG", 0x75aadb, BLACK, WHITE},     // light blue and white stripes
  {"AUS", 0xffcd00, 0x00843d},
  {"BRA", 0xffdc02, 0x0047ab},
  {"CAN", 0xd80621, 0xd80621, WHITE},
  {"CHI", 0xd52b1e, 0x0039a6},
  {"CIV", 0xf77f00, WHITE},
  {"CMR", 0x007a5e, 0xce1126},
  {"COL", 0xfcd116, 0x003893},
  {"ECU", 0xffdd00, 0x034ea2},
  {"EGY", 0xce1126, WHITE},
  {"JPN", 0x0033a0, WHITE},
  {"KOR", 0xe2001a, BLACK, BLACK},
  {"MAR", 0xc1272d, 0x006233},
  {"MEX", 0x006847, WHITE},
  {"NGA", 0x008751, WHITE},
  {"PAR", 0xd52b1e, 0x0038a8},
  {"RSA", 0xffb612, 0x007749},
  {"SEN", WHITE, WHITE, 0x00853f},
  {"URU", 0x5cbfeb, BLACK, WHITE},
  {"USA", WHITE, NAVY},
};

static uint16_t rgb565(uint32_t c) {
  return ((c >> 19) & 0x1F) << 11 | ((c >> 10) & 0x3F) << 5 | ((c >> 3) & 0x1F);
}

// Bright enough for small two-tone details (dark grey and black are not)
static bool showsAsDetail(uint32_t c) {
  return ((c >> 16) & 0xff) >= 96 || ((c >> 8) & 0xff) >= 96 || (c & 0xff) >= 96;
}

bool lookupTeamKit(const String &team, const char *league, TeamKit &kit) {
  String withLeague = team + "@" + league;
  int found = -1;
  for (size_t i = 0; i < sizeof(KITS) / sizeof(KITS[0]); i++) {
    if (withLeague == KITS[i].team) {
      found = i;
      break;  // a league-specific entry wins
    }
    if (found < 0 && team == KITS[i].team) found = i;
  }
  if (found < 0) return false;
  uint32_t second = KITS[found].second ? KITS[found].second : KITS[found].shorts;
  kit.shirt = rgb565(KITS[found].shirt);
  kit.shorts = rgb565(KITS[found].shorts);
  kit.second = showsAsDetail(second) && second != KITS[found].shirt ? rgb565(second) : 0;
  return true;
}
