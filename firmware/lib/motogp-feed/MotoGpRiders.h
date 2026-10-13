// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <string.h>

// The MotoGP grid: the three-letter code and the name shown in the standings, by race number (a rider keeps his number
// for his career). The codes are the first letters of the surname, except where two riders would share them. A rider who
// is not in the list gets a code from his surname. Update it here when the grid changes.

enum class Maker : uint8_t { Ducati, Aprilia, KTM, Yamaha, Honda, Other };

struct MotoGpRider {
  uint8_t number;
  const char *code, *name;
  Maker maker;
};

// The 2026 season, including the riders who stood in for a race
static const MotoGpRider MOTOGP_RIDERS[] = {
  {89, "MRT", "MARTIN", Maker::Aprilia},        {93, "MAR", "M MARQUEZ", Maker::Ducati},   {72, "BEZ", "BEZZECCHI", Maker::Aprilia},
  {79, "OGU", "OGURA", Maker::Aprilia},         {37, "ACO", "ACOSTA", Maker::KTM},         {49, "DIG", "DI GIANNANTONIO", Maker::Ducati},
  {25, "RFE", "R FERNANDEZ", Maker::Aprilia},   {73, "AMA", "A MARQUEZ", Maker::Ducati},   {63, "BAG", "BAGNAIA", Maker::Ducati},
  {54, "ALD", "ALDEGUER", Maker::Ducati},       {23, "BAS", "BASTIANINI", Maker::KTM},     {10, "MRI", "MARINI", Maker::Honda},
  {33, "BIN", "BINDER", Maker::KTM},            {20, "QUA", "QUARTARARO", Maker::Yamaha},  {11, "MRA", "MOREIRA", Maker::Honda},
  {21, "MBD", "MORBIDELLI", Maker::Ducati},     {5, "ZAR", "ZARCO", Maker::Honda},         {36, "MIR", "MIR", Maker::Honda},
  {43, "MIL", "MILLER", Maker::Yamaha},         {42, "RIN", "RINS", Maker::Yamaha},        {7, "RAZ", "RAZGATLIOGLU", Maker::Yamaha},
  {12, "VIN", "VINALES", Maker::KTM},           {27, "LCN", "LECUONA", Maker::Ducati},     {47, "AFE", "A FERNANDEZ", Maker::Yamaha},
  {44, "PES", "P ESPARGARO", Maker::KTM},       {15, "CHA", "CHANTRA", Maker::Honda},      {30, "NAK", "NAKAGAMI", Maker::Honda},
  {35, "CRU", "CRUTCHLOW", Maker::Honda},       {94, "FOL", "FOLGER", Maker::KTM},         {32, "SAV", "SAVADORI", Maker::Aprilia},
  {51, "PIR", "PIRRO", Maker::Ducati},          {41, "AES", "A ESPARGARO", Maker::Honda},
};

static inline const MotoGpRider *motoGpRider(int number) {
  for (const MotoGpRider &rider : MOTOGP_RIDERS)
    if (rider.number == number) return &rider;
  return nullptr;
}

static inline const MotoGpRider *motoGpRiderByCode(const char *code) {
  for (const MotoGpRider &rider : MOTOGP_RIDERS)
    if (!strcmp(rider.code, code)) return &rider;
  return nullptr;
}

// The colour of a maker's bikes on the panel, as 5-6-5
static inline uint16_t motoGpColor(Maker maker) {
#define MOTO_RGB(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
  switch (maker) {
    case Maker::Ducati: return MOTO_RGB(232, 24, 40);
    case Maker::Aprilia: return MOTO_RGB(150, 96, 240);
    case Maker::KTM: return MOTO_RGB(255, 122, 20);
    case Maker::Yamaha: return MOTO_RGB(40, 120, 255);
    case Maker::Honda: return MOTO_RGB(255, 255, 255);
    default: return MOTO_RGB(150, 150, 160);
  }
#undef MOTO_RGB
}

static inline Maker motoGpMaker(const char *name) {
  if (!strncasecmp(name, "Ducati", 6)) return Maker::Ducati;
  if (!strncasecmp(name, "Aprilia", 7)) return Maker::Aprilia;
  if (!strncasecmp(name, "KTM", 3)) return Maker::KTM;
  if (!strncasecmp(name, "Yamaha", 6)) return Maker::Yamaha;
  if (!strncasecmp(name, "Honda", 5)) return Maker::Honda;
  return Maker::Other;
}
