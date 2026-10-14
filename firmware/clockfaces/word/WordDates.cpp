// SPDX-License-Identifier: MIT
// The date in words, in the languages of WordLanguages.cpp (same order): all small, to follow the time.
// A '|' marks where a long word may be broken with a hyphen. Days are spoken the way the language says a date.
#include "WordLanguages.h"
#include <string.h>

namespace {

struct DateWords {
  const char *weekdays[7];  // Sunday first
  const char *months[12];
  const char *days[31];
  bool monthFirst;          // the month, the day, then the weekday
};

const DateWords kDates[] = {
    {{"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"},
     {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"},
     {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
      "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "twenty-one", "twenty-two", "twenty-three",
      "twenty-four", "twenty-five", "twenty-six", "twenty-seven", "twenty-eight", "twenty-nine", "thirty", "thirty-one"},
     false},
    {{"zondag", "maandag", "dinsdag", "woensdag", "donderdag", "vrijdag", "zaterdag"},
     {"januari", "februari", "maart", "april", "mei", "juni", "juli", "augustus", "september", "oktober", "november", "december"},
     {"één", "twee", "drie", "vier", "vijf", "zes", "zeven", "acht", "negen", "tien", "elf", "twaalf", "dertien", "veertien",
      "vijftien", "zestien", "zeventien", "achttien", "negentien", "twintig", "eenentwintig", "tweeëntwintig",
      "drieëntwintig", "vierentwintig", "vijfentwintig", "zesentwintig", "zevenentwintig", "achtentwintig",
      "negenentwintig", "dertig", "eenendertig"},
     false},
    {{"sonntag", "montag", "dienstag", "mittwoch", "donnerstag", "freitag", "samstag"},
     {"januar", "februar", "märz", "april", "mai", "juni", "juli", "august", "september", "oktober", "november", "dezember"},
     {"erste", "zweite", "dritte", "vierte", "fünfte", "sechste", "siebte", "achte", "neunte", "zehnte", "elfte", "zwölfte",
      "dreizehnte", "vierzehnte", "fünfzehnte", "sechzehnte", "siebzehnte", "achtzehnte", "neunzehnte", "zwanzigste",
      "einund|zwanzigste", "zweiund|zwanzigste", "dreiund|zwanzigste", "vierund|zwanzigste", "fünfund|zwanzigste",
      "sechsund|zwanzigste", "siebenund|zwanzigste", "achtund|zwanzigste", "neunund|zwanzigste", "dreißigste",
      "einund|dreißigste"},
     false},
    {{"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"},
     {"janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre", "novembre", "décembre"},
     {"premier", "deux", "trois", "quatre", "cinq", "six", "sept", "huit", "neuf", "dix", "onze", "douze", "treize", "quatorze",
      "quinze", "seize", "dix-sept", "dix-huit", "dix-neuf", "vingt", "vingt et un", "vingt-deux", "vingt-trois", "vingt-quatre",
      "vingt-cinq", "vingt-six", "vingt-sept", "vingt-huit", "vingt-neuf", "trente", "trente et un"},
     false},
    {{"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"},
     {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"},
     {"uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve", "diez", "once", "doce", "trece", "catorce",
      "quince", "dieciséis", "diecisiete", "dieciocho", "diecinueve", "veinte", "veintiuno", "veintidós", "veintitrés",
      "veinticuatro", "veinticinco", "veintiséis", "veintisiete", "veintiocho", "veintinueve", "treinta", "treinta y uno"},
     false},
    {{"domenica", "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato"},
     {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"},
     {"uno", "due", "tre", "quattro", "cinque", "sei", "sette", "otto", "nove", "dieci", "undici", "dodici", "tredici",
      "quattordici", "quindici", "sedici", "diciassette", "diciotto", "diciannove", "venti", "ventuno", "ventidue",
      "ventitré", "ventiquattro", "venticinque", "ventisei", "ventisette", "ventiotto", "ventinove", "trenta", "trentuno"},
     false},
    {{"domingo", "segunda", "terça", "quarta", "quinta", "sexta", "sábado"},
     {"janeiro", "fevereiro", "março", "abril", "maio", "junho", "julho", "agosto", "setembro", "outubro", "novembro", "dezembro"},
     {"um", "dois", "três", "quatro", "cinco", "seis", "sete", "oito", "nove", "dez", "onze", "doze", "treze", "catorze",
      "quinze", "dezasseis", "dezassete", "dezoito", "dezanove", "vinte", "vinte e um", "vinte e dois", "vinte e três",
      "vinte e quatro", "vinte e cinco", "vinte e seis", "vinte e sete", "vinte e oito", "vinte e nove", "trinta", "trintaeum"},
     false},
    {{"søndag", "mandag", "tirsdag", "onsdag", "torsdag", "fredag", "lørdag"},
     {"januar", "februar", "marts", "april", "maj", "juni", "juli", "august", "september", "oktober", "november", "december"},
     {"første", "anden", "tredje", "fjerde", "femte", "sjette", "syvende", "ottende", "niende", "tiende", "elvte", "tolvte",
      "trettende", "fjortende", "femtende", "sekstende", "syttende", "attende", "nittende", "tyvende", "enogtyvende",
      "toogtyvende", "treogtyvende", "fireogtyvende", "femogtyvende", "seksogtyvende", "syvogtyvende", "otteogtyvende",
      "niogtyvende", "tredivte", "enogtredivte"},
     false},
    {{"söndag", "måndag", "tisdag", "onsdag", "torsdag", "fredag", "lördag"},
     {"januari", "februari", "mars", "april", "maj", "juni", "juli", "augusti", "september", "oktober", "november", "december"},
     {"första", "andra", "tredje", "fjärde", "femte", "sjätte", "sjunde", "åttonde", "nionde", "tionde", "elfte", "tolfte",
      "trettonde", "fjortonde", "femtonde", "sextonde", "sjuttonde", "artonde", "nittonde", "tjugonde", "tjugoförsta",
      "tjugoandra", "tjugotredje", "tjugofjärde", "tjugofemte", "tjugosjätte", "tjugosjunde", "tjugoåttonde",
      "tjugonionde", "trettionde", "trettioförsta"},
     false},
    {{"søndag", "mandag", "tirsdag", "onsdag", "torsdag", "fredag", "lørdag"},
     {"januar", "februar", "mars", "april", "mai", "juni", "juli", "august", "september", "oktober", "november", "desember"},
     {"første", "andre", "tredje", "fjerde", "femte", "sjette", "sjuende", "åttende", "niende", "tiende", "ellevte", "tolvte",
      "trettende", "fjortende", "femtende", "sekstende", "syttende", "attende", "nittende", "tjuende", "tjueførste",
      "tjueandre", "tjuetredje", "tjuefjerde", "tjuefemte", "tjuesjette", "tjuesjuende", "tjueåttende", "tjueniende",
      "trettiende", "trettiførste"},
     false},
    {{"niedziela", "poniedziałek", "wtorek", "środa", "czwartek", "piątek", "sobota"},
     {"stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca", "lipca", "sierpnia", "września", "października", "listopada", "grudnia"},
     {"pierwszy", "drugi", "trzeci", "czwarty", "piąty", "szósty", "siódmy", "ósmy", "dziewiąty", "dziesiąty", "jedenasty",
      "dwunasty", "trzynasty", "czternasty", "piętnasty", "szesnasty", "siedemnasty", "osiemnasty", "dziewiętnasty",
      "dwudziesty", "dwudziesty pierwszy", "dwudziesty drugi", "dwudziesty trzeci", "dwudziesty czwarty",
      "dwudziesty piąty", "dwudziesty szósty", "dwudziesty siódmy", "dwudziesty ósmy", "dwudziesty dziewiąty",
      "trzydziesty", "trzydziesty pierwszy"},
     false},
    {{"neděle", "pondělí", "úterý", "středa", "čtvrtek", "pátek", "sobota"},
     {"ledna", "února", "března", "dubna", "května", "června", "července", "srpna", "září", "října", "listopadu", "prosince"},
     {"prvního", "druhého", "třetího", "čtvrtého", "pátého", "šestého", "sedmého", "osmého", "devátého", "desátého",
      "jedenáctého", "dvanáctého", "třináctého", "čtrnáctého", "patnáctého", "šestnáctého", "sedmnáctého", "osmnáctého",
      "devatenáctého", "dvacátého", "dvacátého prvního", "dvacátého druhého", "dvacátého třetího", "dvacátého čtvrtého",
      "dvacátého pátého", "dvacátého šestého", "dvacátého sedmého", "dvacátého osmého", "dvacátého devátého", "třicátého",
      "třicátého prvního"},
     false},
    {{"nedeľa", "pondelok", "utorok", "streda", "štvrtok", "piatok", "sobota"},
     {"januára", "februára", "marca", "apríla", "mája", "júna", "júla", "augusta", "septembra", "októbra", "novembra", "decembra"},
     {"prvého", "druhého", "tretieho", "štvrtého", "piateho", "šiesteho", "siedmeho", "ôsmeho", "deviateho", "desiateho",
      "jedenásteho", "dvanásteho", "trinásteho", "štrnásteho", "pätnásteho", "šestnásteho", "sedemnásteho", "osemnásteho",
      "devätnásteho", "dvadsiateho", "dvadsiateho prvého", "dvadsiateho druhého", "dvadsiateho tretieho",
      "dvadsiateho štvrtého", "dvadsiateho piateho", "dvadsiateho šiesteho", "dvadsiateho siedmeho", "dvadsiateho ôsmeho",
      "dvadsiateho deviateho", "tridsiateho", "tridsiateho prvého"},
     false},
    {{"vasárnap", "hétfő", "kedd", "szerda", "csütörtök", "péntek", "szombat"},
     {"január", "február", "március", "április", "május", "június", "július", "augusztus", "szeptember", "október", "november", "december"},
     {"elseje", "másodika", "harmadika", "negyedike", "ötödike", "hatodika", "hetedike", "nyolcadika", "kilencedike", "tizedike",
      "tizenegyedike", "tizenkettedike", "tizenharmadika", "tizennegyedike", "tizenötödike", "tizenhatodika",
      "tizenhetedike", "tizen|nyolcadika", "tizen|kilencedike", "huszadika", "huszonegyedike", "huszon|kettedike",
      "huszon|harmadika", "huszon|negyedike", "huszonötödike", "huszonhatodika", "huszonhetedike", "huszon|nyolcadika",
      "huszon|kilencedike", "harmincadika", "harminc|egyedike"},
     true},
    {{"duminică", "luni", "marți", "miercuri", "joi", "vineri", "sâmbătă"},
     {"ianuarie", "februarie", "martie", "aprilie", "mai", "iunie", "iulie", "august", "septembrie", "octombrie", "noiembrie", "decembrie"},
     {"întâi", "doi", "trei", "patru", "cinci", "șase", "șapte", "opt", "nouă", "zece", "unsprezece", "doisprezece",
      "treisprezece", "paisprezece", "cincisprezece", "șaisprezece", "șaptesprezece", "optsprezece", "nouăsprezece",
      "douăzeci", "douăzeci și unu", "douăzeci și doi", "douăzeci și trei", "douăzeci și patru", "douăzeci și cinci",
      "douăzeci și șase", "douăzeci și șapte", "douăzeci și opt", "douăzeci și nouă", "treizeci", "treizeci și unu"},
     false},
    {{"nedjelja", "ponedjeljak", "utorak", "srijeda", "četvrtak", "petak", "subota"},
     {"siječnja", "veljače", "ožujka", "travnja", "svibnja", "lipnja", "srpnja", "kolovoza", "rujna", "listopada", "studenoga", "prosinca"},
     {"prvi", "drugi", "treći", "četvrti", "peti", "šesti", "sedmi", "osmi", "deveti", "deseti", "jedanaesti", "dvanaesti",
      "trinaesti", "četrnaesti", "petnaesti", "šesnaesti", "sedamnaesti", "osamnaesti", "devetnaesti", "dvadeseti",
      "dvadeset prvi", "dvadeset drugi", "dvadeset treći", "dvadeset četvrti", "dvadeset peti", "dvadeset šesti",
      "dvadeset sedmi", "dvadeset osmi", "dvadeset deveti", "trideseti", "tridesetprvi"},
     false},
};

}  // namespace

// A day of two words that is too long for one line goes on two lines (the small lines join again when they fit)
static void addDay(const char *day, WordPhrase &out) {
  static char pieces[2][32];
  const char *space = strchr(day, ' ');
  if (!space || strlen(day) <= 12) { out.small(day); return; }
  const size_t first = space - day;
  strlcpy(pieces[0], day, first + 1 < sizeof(pieces[0]) ? first + 1 : sizeof(pieces[0]));
  strlcpy(pieces[1], space + 1, sizeof(pieces[1]));
  out.small(pieces[0]);
  out.small(pieces[1]);
}

void sayDate(int language, int weekday, int day, int month, WordPhrase &out) {
  const DateWords &w = kDates[language];
  const char *weekdayName = w.weekdays[weekday % 7], *monthName = w.months[(month + 11) % 12], *dayName = w.days[(day + 30) % 31];
  if (w.monthFirst) {
    out.small(monthName);
    addDay(dayName, out);
    out.small(weekdayName);
  } else {
    out.small(weekdayName);
    addDay(dayName, out);
    out.small(monthName);
  }
}
