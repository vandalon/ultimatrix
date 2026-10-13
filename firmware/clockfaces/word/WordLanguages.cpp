// SPDX-License-Identifier: MIT
#include "WordLanguages.h"

// 1 to 12, as said on a 12 hour clock
static int hour12(int h) {
  h %= 12;
  return h ? h : 12;
}

// ---- English: the time as read from a digital clock, "seven forty five" ----

static const char *const EN[] = {"", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                 "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
                                 "eighteen", "nineteen"};
static const char *const EN_TENS[] = {"", "", "twenty", "thirty", "forty", "fifty"};

static void sayEnglish(int h, int m, WordPhrase &p) {
  if (m == 0 && h == 0) {
    p.hour("mid");
    p.hour("night");
    return;
  }
  if (m == 0 && h == 12) {
    p.hour("noon");
    return;
  }
  p.hour(EN[hour12(h)]);
  if (m == 0) {
    p.small("o'clock");
  } else if (m < 10) {
    p.small("oh");
    p.small(EN[m]);
  } else if (m < 20) {
    p.small(EN[m]);
  } else {
    p.small(EN_TENS[m / 10]);
    if (m % 10) p.small(EN[m % 10]);
  }
}

// ---- Dutch, German and the Scandinavian languages count around the half hour:
// "tien voor half acht", "fünf nach halb acht", "ti på halv åtte" ----

struct HalfHourWords {
  const char *const *numbers;  // 1 to 20, the hours 1 to 12 too
  const char *past, *to, *half, *pastHalf, *toHalf, *quarterPast, *quarterTo;
  const char *oclock;          // said with a full hour
  bool oclockFirst;            // "klokken syv" instead of "zeven uur"
  const char *oneOclock;       // the hour one when it is said on its own ("ein Uhr"), or nullptr
  const char *midnight[2];     // the second line may be nullptr
  uint8_t halfFrom, halfUntil; // the minutes that are counted from the half hour
};

static void sayAroundHalf(const HalfHourWords &w, int h, int m, WordPhrase &p) {
  if (m == 0) {
    if (h == 0) {
      p.hour(w.midnight[0]);
      if (w.midnight[1]) p.hour(w.midnight[1]);
      return;
    }
    if (w.oclockFirst) p.small(w.oclock);
    p.hour(hour12(h) == 1 && w.oneOclock ? w.oneOclock : w.numbers[hour12(h)]);
    if (!w.oclockFirst) p.small(w.oclock);
    return;
  }
  if (m == 15) {
    p.small(w.quarterPast);
  } else if (m < w.halfFrom) {
    p.small(w.numbers[m]);
    p.small(w.past);
  } else if (m < 30) {
    p.small(w.numbers[30 - m]);
    p.small(w.toHalf);
  } else if (m == 30) {
    p.small(w.half);
  } else if (m <= w.halfUntil) {
    p.small(w.numbers[m - 30]);
    p.small(w.pastHalf);
  } else if (m == 45) {
    p.small(w.quarterTo);
  } else {
    p.small(w.numbers[60 - m]);
    p.small(w.to);
  }
  p.hour(w.numbers[hour12(m < w.halfFrom ? h : h + 1)]);
}

static const char *const NL[] = {"", "één", "twee", "drie", "vier", "vijf", "zes", "zeven", "acht", "negen", "tien",
                                 "elf", "twaalf", "dertien", "veertien"};
static const HalfHourWords DUTCH = {NL, "over", "voor", "half", "over half", "voor half", "kwart over",
                                    "kwart voor", "uur", false, nullptr, {"midder", "nacht"}, 16, 44};
static void sayDutch(int h, int m, WordPhrase &p) { sayAroundHalf(DUTCH, h, m, p); }

static const char *const DE[] = {"", "eins", "zwei", "drei", "vier", "fünf", "sechs", "sieben", "acht", "neun",
                                 "zehn", "elf", "zwölf", "dreizehn", "vierzehn"};
static const HalfHourWords GERMAN = {DE, "nach", "vor", "halb", "nach halb", "vor halb", "viertel nach",
                                     "viertel vor", "uhr", false, "ein", {"mitter", "nacht"}, 16, 44};
static void sayGerman(int h, int m, WordPhrase &p) { sayAroundHalf(GERMAN, h, m, p); }

static const char *const DA[] = {"", "et", "to", "tre", "fire", "fem", "seks", "syv", "otte", "ni", "ti", "elleve",
                                 "tolv", "tretten", "fjorten", "femten", "seksten", "sytten", "atten", "nitten", "tyve"};
static const HalfHourWords DANISH = {DA, "over", "i", "halv", "over halv", "i halv", "kvart over", "kvart i",
                                     "klokken", true, nullptr, {"midnat", nullptr}, 20, 39};
static void sayDanish(int h, int m, WordPhrase &p) { sayAroundHalf(DANISH, h, m, p); }

static const char *const SV[] = {"", "ett", "två", "tre", "fyra", "fem", "sex", "sju", "åtta", "nio", "tio", "elva",
                                 "tolv", "tretton", "fjorton", "femton", "sexton", "sjutton", "arton", "nitton", "tjugo"};
static const HalfHourWords SWEDISH = {SV, "över", "i", "halv", "över halv", "i halv", "kvart över", "kvart i",
                                      "klockan", true, nullptr, {"midnatt", nullptr}, 20, 39};
static void saySwedish(int h, int m, WordPhrase &p) { sayAroundHalf(SWEDISH, h, m, p); }

static const char *const NO[] = {"", "ett", "to", "tre", "fire", "fem", "seks", "sju", "åtte", "ni", "ti", "elleve",
                                 "tolv", "tretten", "fjorten", "femten", "seksten", "sytten", "atten", "nitten", "tjue"};
static const HalfHourWords NORWEGIAN = {NO, "over", "på", "halv", "over halv", "på halv", "kvart over", "kvart på",
                                        "klokka", true, nullptr, {"midnatt", nullptr}, 20, 39};
static void sayNorwegian(int h, int m, WordPhrase &p) { sayAroundHalf(NORWEGIAN, h, m, p); }

// ---- French, Spanish and Italian count down to the next hour in the last part of it:
// "huit heures moins dix", "las ocho menos diez", "le otto meno dieci" ----

static const char *const FR[] = {"", "une", "deux", "trois", "quatre", "cinq", "six", "sept", "huit", "neuf", "dix",
                                 "onze", "douze", "treize", "quatorze", "quinze", "seize", "dix-sept", "dix-huit",
                                 "dix-neuf", "vingt", "vingt et une", "vingt-deux", "vingt-trois", "vingt-quatre",
                                 "vingt-cinq", "vingt-six", "vingt-sept", "vingt-huit", "vingt-neuf", "trente",
                                 "trente et une", "trente-deux", "trente-trois", "trente-quatre", "trente-cinq",
                                 "trente-six", "trente-sept", "trente-huit", "trente-neuf", "quarante",
                                 "quarante et une", "quarante-deux", "quarante-trois", "quarante-quatre",
                                 "quarante-cinq", "quarante-six", "quarante-sept", "quarante-huit", "quarante-neuf"};

// "sept heures", "une heure", and on their own "midi" and "minuit"
static void frenchHour(int h, WordPhrase &p) {
  h %= 24;
  if (h == 0) {
    p.hour("minuit");
  } else if (h == 12) {
    p.hour("midi");
  } else {
    p.hour(FR[hour12(h)]);
    p.small(hour12(h) == 1 ? "heure" : "heures");
  }
}

// "moins" for the round minutes from 35 on and for the last ten: "sept heures quarante-deux",
// "huit heures moins vingt", "huit heures moins trois"
static void sayFrench(int h, int m, WordPhrase &p) {
  if (m < 35 || (m < 50 && m % 5)) {
    frenchHour(h, p);
    if (m == 15) p.small("et quart");
    else if (m == 30) p.small(h % 12 == 0 ? "et demi" : "et demie");  // midi et demi, sept heures et demie
    else if (m) p.small(FR[m]);
  } else {
    frenchHour(h + 1, p);
    p.small("moins");
    p.small(m == 45 ? "le quart" : FR[60 - m]);
  }
}

static const char *const ES_HOURS[] = {"", "una", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve",
                                       "diez", "once", "doce"};
static const char *const ES[] = {"", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve", "diez",
                                 "once", "doce", "trece", "catorce", "quince", "dieci|séis", "dieci|siete",
                                 "dieci|ocho", "dieci|nueve", "veinte", "veinti|uno", "veinti|dós", "veinti|trés",
                                 "veinti|cuatro", "veinti|cinco", "veinti|séis", "veinti|siete", "veinti|ocho",
                                 "veinti|nueve", "treinta", "treinta y uno", "treinta y dos", "treinta y tres",
                                 "treinta y cuatro"};

static void saySpanish(int h, int m, WordPhrase &p) {
  if (m == 0 && h == 0) {
    p.hour("media");
    p.hour("noche");
  } else if (m == 0 && h == 12) {
    p.hour("medio");
    p.hour("día");
  } else if (m == 0) {
    p.hour(ES_HOURS[hour12(h)]);
    p.small("en punto");
  } else if (m < 35) {
    p.hour(ES_HOURS[hour12(h)]);
    p.small("y");
    p.small(m == 15 ? "cuarto" : m == 30 ? "media" : ES[m]);
  } else {
    p.hour(ES_HOURS[hour12(h + 1)]);
    p.small("menos");
    p.small(m == 45 ? "cuarto" : ES[60 - m]);
  }
}

static const char *const IT_HOURS[] = {"", "una", "due", "tre", "quattro", "cinque", "sei", "sette", "otto", "nove",
                                       "dieci", "undici", "dodici"};
static const char *const IT[] = {"", "uno", "due", "tre", "quattro", "cinque", "sei", "sette", "otto", "nove",
                                 "dieci", "undici", "dodici", "tredici", "quattor|dici", "quindici", "sedici",
                                 "dicias|sette", "diciotto", "dician|nove", "venti", "ventuno", "ventidue", "venti|tré",
                                 "venti|quattro", "venti|cinque", "ventisei", "venti|sette", "ventotto", "venti|nove",
                                 "trenta", "trentuno", "trenta|due", "trenta|tré", "trenta|quattro"};

static void sayItalian(int h, int m, WordPhrase &p) {
  if (m == 0 && h == 0) {
    p.hour("mezza");
    p.hour("notte");
  } else if (m == 0 && h == 12) {
    p.hour("mezzo");
    p.hour("giorno");
  } else if (m == 0) {
    p.hour(IT_HOURS[hour12(h)]);
    p.small("in punto");
  } else if (m < 35) {
    p.hour(IT_HOURS[hour12(h)]);
    p.small("e");
    p.small(m == 15 ? "un quarto" : m == 30 ? "mezza" : IT[m]);
  } else {
    p.hour(IT_HOURS[hour12(h + 1)]);
    p.small("meno");
    p.small(m == 45 ? "un quarto" : IT[60 - m]);
  }
}

// ---- Portuguese (Brazil): "sete e vinte", "sete vinte e cinco" ----

static const char *const PT_HOURS[] = {"", "uma", "duas", "três", "quatro", "cinco", "seis", "sete", "oito", "nove",
                                       "dez", "onze", "doze"};
static const char *const PT[] = {"", "um", "dois", "três", "quatro", "cinco", "seis", "sete", "oito", "nove", "dez",
                                 "onze", "doze", "treze", "quatorze", "quinze", "dezes|seis", "dezes|sete",
                                 "dezoito", "dezenove"};
static const char *const PT_AND[] = {"", "e um", "e dois", "e três", "e quatro", "e cinco", "e seis", "e sete",
                                     "e oito", "e nove"};
static const char *const PT_TENS[] = {"", "", "vinte", "trinta", "quarenta", "cinquenta"};

static void sayPortuguese(int h, int m, WordPhrase &p) {
  if (m == 0 && h == 0) {
    p.hour("meia");
    p.hour("noite");
    return;
  }
  if (m == 0 && h == 12) {
    p.hour("meio");
    p.hour("dia");
    return;
  }
  p.hour(PT_HOURS[hour12(h)]);
  if (m == 0) {
    p.small(hour12(h) == 1 ? "hora" : "horas");
  } else if (m == 30) {
    p.small("e meia");
  } else if (m < 20) {
    p.small("e");
    p.small(PT[m]);
  } else {
    p.small(PT_TENS[m / 10]);
    if (m % 10) p.small(PT_AND[m % 10]);
  }
}

// ---- Polish, Czech, Slovak and Hungarian, read from a digital clock: "siódma zero pięć",
// "sedm hodin", "hét óra negyvenöt" ----

// The hour, then "zero" and the minute below ten, then the tens and the ones
struct DigitalWords {
  const char *const *hours;    // 1 to 12
  const char *const *numbers;  // 1 to 19
  const char *const *tens;     // 20, 30, 40 and 50 at 2 to 5
  const char *zero;            // before a minute below ten, or nullptr
  const char *oclock[3];       // after the hour 1, 2 to 4 and 5 or more, or before it when oclockFirst
  bool oclockFirst;
  const char *midnight, *noon;
};

static void sayDigital(const DigitalWords &w, int h, int m, WordPhrase &p) {
  if (m == 0 && h == 0) {
    p.hour(w.midnight);
    return;
  }
  if (m == 0 && h == 12) {
    p.hour(w.noon);
    return;
  }
  int h12 = hour12(h);
  if (m == 0) {
    const char *oclock = w.oclock[h12 == 1 ? 0 : h12 <= 4 ? 1 : 2];
    if (w.oclockFirst) p.small(oclock);
    p.hour(w.hours[h12]);
    if (!w.oclockFirst) p.small(oclock);
    return;
  }
  p.hour(w.hours[h12]);
  if (m < 10 && w.zero) p.small(w.zero);
  if (m < 20) {
    p.small(w.numbers[m]);
  } else {
    p.small(w.tens[m / 10]);
    if (m % 10) p.small(w.numbers[m % 10]);
  }
}

static const char *const PL_HOURS[] = {"", "pierwsza", "druga", "trzecia", "czwarta", "piąta", "szósta", "siódma",
                                       "ósma", "dzie|wiąta", "dzie|siąta", "jede|nasta", "dwu|nasta"};
static const char *const PL[] = {"", "jeden", "dwa", "trzy", "cztery", "pięć", "sześć", "siedem", "osiem",
                                 "dzie|więć", "dzie|sięć", "jede|naście", "dwa|naście", "trzy|naście", "czter|naście",
                                 "pięt|naście", "szes|naście", "siedem|naście", "osiem|naście", "dziewięt|naście"};
static const char *const PL_TENS[] = {"", "", "dwa|dzieścia", "trzy|dzieści", "czter|dzieści", "pięćdzie|siąt"};
static const DigitalWords POLISH = {PL_HOURS, PL, PL_TENS, "zero", {"godzina", "godzina", "godzina"}, true,
                                    "pół|noc", "połu|dnie"};
static void sayPolish(int h, int m, WordPhrase &p) { sayDigital(POLISH, h, m, p); }

static const char *const CS_HOURS[] = {"", "jedna", "dvě", "tři", "čtyři", "pět", "šest", "sedm", "osm", "devět",
                                       "deset", "jede|náct", "dva|náct"};
static const char *const CS[] = {"", "jedna", "dvě", "tři", "čtyři", "pět", "šest", "sedm", "osm", "devět", "deset",
                                 "jede|náct", "dva|náct", "tři|náct", "čtr|náct", "pat|náct", "šest|náct", "sedm|náct",
                                 "osm|náct", "devate|náct"};
static const char *const CS_TENS[] = {"", "", "dvacet", "třicet", "čtyři|cet", "pade|sát"};
static const DigitalWords CZECH = {CS_HOURS, CS, CS_TENS, "nula", {"hodina", "hodiny", "hodin"}, false,
                                   "půlnoc", "poledne"};
static void sayCzech(int h, int m, WordPhrase &p) { sayDigital(CZECH, h, m, p); }

static const char *const SK_HOURS[] = {"", "jedna", "dve", "tri", "štyri", "päť", "šesť", "sedem", "osem", "deväť",
                                       "desať", "jede|násť", "dva|násť"};
static const char *const SK[] = {"", "jedna", "dve", "tri", "štyri", "päť", "šesť", "sedem", "osem", "deväť",
                                 "desať", "jede|násť", "dva|násť", "tri|násť", "štr|násť", "pät|násť", "šest|násť",
                                 "sedem|násť", "osem|násť", "devät|násť"};
static const char *const SK_TENS[] = {"", "", "dvad|sať", "trid|sať", "štyrid|sať", "päť|desiat"};
static const DigitalWords SLOVAK = {SK_HOURS, SK, SK_TENS, "nula", {"hodina", "hodiny", "hodín"}, false,
                                    "polnoc", "poludnie"};
static void saySlovak(int h, int m, WordPhrase &p) { sayDigital(SLOVAK, h, m, p); }

// Hungarian writes the tens and the ones as one word: "negyvenöt"
static const char *const HU_HOURS[] = {"", "egy", "kettő", "három", "négy", "öt", "hat", "hét", "nyolc", "kilenc",
                                       "tíz", "tizen|egy", "tizen|kettő"};
static const char *const HU[] = {
    "", "egy", "kettő", "három", "négy", "öt", "hat", "hét", "nyolc", "kilenc", "tíz",
    "tizen|egy", "tizen|kettő", "tizen|három", "tizen|négy", "tizen|öt", "tizen|hat", "tizen|hét", "tizen|nyolc",
    "tizen|kilenc", "húsz", "huszon|egy", "huszon|kettő", "huszon|három", "huszon|négy", "huszon|öt", "huszon|hat",
    "huszon|hét", "huszon|nyolc", "huszon|kilenc", "harminc", "harminc|egy", "harminc|kettő", "harminc|három",
    "harminc|négy", "harminc|öt", "harminc|hat", "harminc|hét", "harminc|nyolc", "harminc|kilenc", "negyven",
    "negyven|egy", "negyven|kettő", "negyven|három", "negyven|négy", "negyven|öt", "negyven|hat", "negyven|hét",
    "negyven|nyolc", "negyven|kilenc", "ötven", "ötven|egy", "ötven|kettő", "ötven|három", "ötven|négy", "ötven|öt",
    "ötven|hat", "ötven|hét", "ötven|nyolc", "ötven|kilenc"};

static void sayHungarian(int h, int m, WordPhrase &p) {
  if (m == 0 && h == 0) {
    p.hour("éjfél");
  } else if (m == 0 && h == 12) {
    p.hour("dél");
  } else {
    p.hour(HU_HOURS[hour12(h)]);
    p.small("óra");
    if (m) p.small(HU[m]);
  }
}

// ---- Romanian (from 40 minutes on) and Croatian count down to the next hour in the second half of it:
// "opt fără zece", "deset do osam", and Croatian says the half hour as "pola osam" ----

static const char *const RO_HOURS[] = {"", "unu", "două", "trei", "patru", "cinci", "șase", "șapte", "opt", "nouă",
                                       "zece", "unspre|zece", "două|spre|zece"};
static const char *const RO[] = {"", "un minut", "două", "trei", "patru", "cinci", "șase", "șapte", "opt", "nouă",
                                 "zece", "un|spre|zece", "două|spre|zece", "trei|spre|zece", "pai|spre|zece",
                                 "cinci|spre|zece", "șai|spre|zece", "șapte|spre|zece", "opt|spre|zece",
                                 "nouă|spre|zece", "două|zeci", "două|zeci și unu", "două|zeci și două",
                                 "două|zeci și trei", "două|zeci și patru", "două|zeci și cinci", "două|zeci și șase",
                                 "două|zeci și șapte", "două|zeci și opt", "două|zeci și nouă", "trei|zeci",
                                 "trei|zeci și unu", "trei|zeci și două", "trei|zeci și trei", "trei|zeci și patru",
                                 "trei|zeci și cinci", "trei|zeci și șase", "trei|zeci și șapte",
                                 "trei|zeci și opt", "trei|zeci și nouă", "patru|zeci", "patru|zeci și unu",
                                 "patru|zeci și două", "patru|zeci și trei", "patru|zeci și patru"};

static void sayRomanian(int h, int m, WordPhrase &p) {
  if (m == 0 && h == 0) {
    p.hour("miezul");
    p.hour("nopții");
  } else if (m == 0 && h == 12) {
    p.hour("amiază");
  } else if (m == 0) {
    p.small("ora");
    p.hour(RO_HOURS[hour12(h)]);
  } else if (m < 40 || (m < 45 && hour12(h + 1) == 12)) {  // "douăsprezece fără șaptesprezece" is too long
    p.hour(RO_HOURS[hour12(h)]);
    p.small("și");
    p.small(m == 15 ? "un sfert" : m == 30 ? "jumătate" : RO[m]);
  } else {
    p.hour(RO_HOURS[hour12(h + 1)]);
    p.small("fără");
    p.small(m == 45 ? "un sfert" : RO[60 - m]);
  }
}

static const char *const HR_HOURS[] = {"", "jedan", "dva", "tri", "četiri", "pet", "šest", "sedam", "osam", "devet",
                                       "deset", "jeda|naest", "dva|naest"};
static const char *const HR[] = {"", "jedna", "dvije", "tri", "četiri", "pet", "šest", "sedam", "osam", "devet",
                                 "deset", "jeda|naest", "dva|naest", "tri|naest", "četr|naest", "pet|naest",
                                 "šes|naest", "sedam|naest", "osam|naest", "devet|naest", "dva|deset",
                                 "dva|deset jedna", "dva|deset dvije", "dva|deset tri", "dva|deset četiri",
                                 "dva|deset pet", "dva|deset šest", "dva|deset sedam", "dva|deset osam",
                                 "dva|deset devet"};

static void sayCroatian(int h, int m, WordPhrase &p) {
  int h12 = hour12(h);
  if (m == 0 && h == 0) {
    p.hour("ponoć");
  } else if (m == 0 && h == 12) {
    p.hour("podne");
  } else if (m == 0) {
    p.hour(HR_HOURS[h12]);
    p.small(h12 == 1 ? "sat" : h12 <= 4 ? "sata" : "sati");
  } else if (m < 30) {
    p.hour(HR_HOURS[h12]);
    p.small("i");
    p.small(m == 15 ? "četvrt" : HR[m]);
  } else if (m == 30) {
    p.small("pola");
    p.hour(HR_HOURS[hour12(h + 1)]);
  } else {
    p.small(m == 45 ? "četvrt" : HR[60 - m]);
    p.small("do");
    p.hour(HR_HOURS[hour12(h + 1)]);
  }
}

const WordLanguage WORD_LANGUAGES[] = {
    {"English", sayEnglish, {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"}, "%d/%d"},
    {"Nederlands", sayDutch, {"ZO", "MA", "DI", "WO", "DO", "VR", "ZA"}, "%d-%d"},
    {"Deutsch", sayGerman, {"SO", "MO", "DI", "MI", "DO", "FR", "SA"}, "%d.%d."},
    {"Français", sayFrench, {"DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM"}, "%d/%d"},
    {"Español", saySpanish, {"DOM", "LUN", "MAR", "MIÉ", "JUE", "VIE", "SÁB"}, "%d/%d"},
    {"Italiano", sayItalian, {"DOM", "LUN", "MAR", "MER", "GIO", "VEN", "SAB"}, "%d/%d"},
    {"Português", sayPortuguese, {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SÁB"}, "%d/%d"},
    {"Dansk", sayDanish, {"SØN", "MAN", "TIR", "ONS", "TOR", "FRE", "LØR"}, "%d/%d"},
    {"Svenska", saySwedish, {"SÖN", "MÅN", "TIS", "ONS", "TOR", "FRE", "LÖR"}, "%d/%d"},
    {"Norsk", sayNorwegian, {"SØN", "MAN", "TIR", "ONS", "TOR", "FRE", "LØR"}, "%d.%d."},
    {"Polski", sayPolish, {"NIE", "PON", "WTO", "ŚRO", "CZW", "PIĄ", "SOB"}, "%d.%d"},
    {"Čeština", sayCzech, {"NE", "PO", "ÚT", "ST", "ČT", "PÁ", "SO"}, "%d.%d."},
    {"Slovenčina", saySlovak, {"NE", "PO", "UT", "ST", "ŠT", "PI", "SO"}, "%d.%d."},
    {"Magyar", sayHungarian, {"VAS", "HÉT", "KED", "SZE", "CSÜ", "PÉN", "SZO"}, "%d.%d.", true},
    {"Română", sayRomanian, {"DUM", "LUN", "MAR", "MIE", "JOI", "VIN", "SÂM"}, "%d.%d"},
    {"Hrvatski", sayCroatian, {"NED", "PON", "UTO", "SRI", "ČET", "PET", "SUB"}, "%d.%d."},
};
const uint8_t WORD_LANGUAGE_COUNT = sizeof(WORD_LANGUAGES) / sizeof(WORD_LANGUAGES[0]);
